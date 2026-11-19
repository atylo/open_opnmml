#include "player.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <sys/stat.h>

namespace opnmml {

namespace {

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

bool hasMusExtension(const std::string& n) {
    return n.size() > 4 && lower(n.substr(n.size() - 4)) == ".mus";
}

// "dir/bgm01_fm3.mus" -> "bgm01_fm3"
std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string n = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = n.find_last_of('.');
    if (dot != std::string::npos && dot > 0) n.resize(dot);
    return n;
}

std::string mmss(long ms) {
    char b[32];
    long s = ms / 1000;
    snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
    return b;
}

const int kVolumeStep = 4;      // one press of + or -: 4 steps of the master volume = about 0.9 dB (27 steps = 6 dB)
const int kTempoStep = 5;       // one press of [ or ]: 5 beats per minute
const int kSlot = 3;            // the game plays its background music in slot 3

}  // namespace

Player::Player(const Options& o) : opt(o) {
    memset(carry, 0, sizeof carry);
    for (int i = 0; i < kWin; i++) lo[i] = hi[i] = 0;
}

// ---- the playlist -------------------------------------------------------------------------------------------------------------------------

bool Player::addPath(const std::string& path) {
    std::string p = path;
    // Windows refuses a folder name that ends in a separator, so cut those off (but keep "/" and "C:/")
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\') && !(p.size() == 3 && p[1] == ':')) p.pop_back();
    struct stat sb;
    if (stat(p.c_str(), &sb) != 0) { say("error: '" + path + "' does not exist."); return false; }
    if ((sb.st_mode & S_IFMT) != S_IFDIR) { files.push_back(path); return true; }

    DIR* d = opendir(p.c_str());
    if (!d) { say("error: cannot open the folder '" + path + "'."); return false; }
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) if (hasMusExtension(e->d_name)) names.push_back(e->d_name);
    closedir(d);
    if (names.empty()) { say("warning: the folder '" + path + "' has no .mus files."); return false; }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    std::string prefix = p;
    if (prefix.back() != '/' && prefix.back() != '\\') prefix += '/';
    for (const std::string& n : names) files.push_back(prefix + n);
    return true;
}

bool Player::readTrack(int idx, std::string& text) {
    const std::string& path = files[idx];
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { say("error: cannot open '" + path + "': " + strerror(errno)); return false; }
    char buf[4096];
    size_t n;
    text.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) say("error: reading '" + path + "' failed.");
    return ok;
}

// Build a fresh engine for the file and start it.  Nothing changes when the file cannot be played.
bool Player::loadTrack(int idx) {
    std::string text;
    if (!readTrack(idx, text)) return false;
    const std::string& path = files[idx];

    // Compile once on its own to learn what the file is written for (machine, dialect, which channels have music) and how long it is,
    // the same way mmlplay does.
    std::unique_ptr<Song> song(new Song);
    if (!compile(text.c_str(), *song)) { say(song->describeError(path.c_str())); return false; }
    int device = (song->machine == M_X1) ? Mixer::DEVICE_X1 : (song->machine == M_MZ) ? Mixer::DEVICE_MZ : Mixer::DEVICE_PC88;

    std::unique_ptr<Mixer> m;
    try {
        m.reset(new Mixer(opt.rate));
        m->setDevice(device);                   // wipes the slots and sets the chip: a clean start
        m->setVolume(vol);
        m->setMute(0);
        if (opt.once) m->setLooping(kSlot, false);
        if (!m->loadSong(kSlot, 0, text.c_str(), true)) { say("error: " + path + ": " + m->lastError()); return false; }
    } catch (const std::exception& e) {
        say(std::string("error: ") + e.what());
        return false;
    }

    SongTiming timing;
    bool haveTiming = measureSong(*song, 0, timing) && !timing.truncated;

    mx = std::move(m);
    cur = idx;
    st = PLAYING;
    isPaused = false; allDone = false; autoAdvance = false;
    pendingIdx = -1;
    mute = 0;
    x1Dialect = song->x1Mode;
    deviceName = device == Mixer::DEVICE_X1 ? "X1" : device == Mixer::DEVICE_MZ ? "MZ" : "PC88";
    trackName = baseName(path);
    onceMs = haveTiming ? timing.onceMs : -1;
    delivered = 0; peak = 0; tailBlocks = 0; winPos = 0;
    carryPos = carryLen = 0;
    for (int i = 0; i < kWin; i++) lo[i] = hi[i] = 0;

    char head[64];
    snprintf(head, sizeof head, "%d/%d  ", idx + 1, (int)files.size());
    std::string line = std::string(head) + path + "  [" + deviceName + "]";
    const std::string& title = mx->songTitle(kSlot, 0);
    if (!title.empty()) line += "  \"" + title + "\"";
    say(line);
    // The MZ chip has no FM sound (the original is the same): only D, E and F are heard there.
    if (device == Mixer::DEVICE_MZ) {
        std::string silent;
        for (int c = 0; c < 11; c++)
            if ((c < 3 || c > 5) && song->ch[c].totalTicks > 0) { if (!silent.empty()) silent += ", "; silent += (char)('A' + c); }
        if (!silent.empty())
            say("warning: this file has music on channel(s) " + silent + ", but the MZ device has no FM sound: those channels are silent "
                "(the original plugin does the same).");
    }
    return true;
}

// Load track idx; when it cannot be played go on to the next one in direction dir.  `automatic`: the player moved on by itself, so the
// end of the list ends the playlist instead of wrapping round.
void Player::startFrom(int idx, int dir, bool automatic) {
    int n = (int)files.size();
    for (int tries = 0; tries < n; tries++) {
        int i = idx + dir * tries;
        if (automatic) { if (i < 0 || i >= n) break; }
        else i = ((i % n) + n) % n;
        if (loadTrack(i)) return;
    }
    say("no file could be played.");
    st = IDLE;
    if (automatic) allDone = true;
}

bool Player::begin() {
    if (files.empty()) return false;
    startFrom(0, +1, true);
    return st == PLAYING;
}

// ---- controls -----------------------------------------------------------------------------------------------------------------------------

void Player::listKeys() {
    say("keys:  space  pause / continue (after a stop: play again)      s  stop (the game's quick fade)");
    say("       n  next file    p  previous file    + / -  louder / quieter (about 0.9 dB per press)");
    say("       [ / ]  slower / faster by 5 beats per minute (the song's own t commands change it again)");
    say("       A..K  switch that channel off or on (all channels come back with the next file)    q  quit");
    say("status line: [number/count] name  machine  time/length of one pass  vol  tempo  channels  state");
    say("channels: letter = playing, - = no music (or finished), . = switched off");
}

void Player::goTo(int idx) {
    // `idx` was worked out by the caller; pendingDir tells in which direction to look further if that file cannot be played
    if (st == IDLE || isPaused) { isPaused = false; startFrom(idx, pendingDir, false); return; }
    if (st == PLAYING) { mx->stopSlot(kSlot); st = STOPPING; }       // the game's own way of ending music: a quick fade, then the next
    pendingIdx = idx;
    autoAdvance = false;
}

void Player::key(int c) {
    if (c == 'q' || c == 'Q' || c == 27 || c == 3) { quitting = true; return; }
    if (c == '?') { listKeys(); return; }
    if (cur < 0 || !mx) return;
    int n = (int)files.size();

    if (c == ' ') {
        if (st == IDLE) { allDone = false; isPaused = false; pendingDir = 1; startFrom(cur, 1, false); }
        else isPaused = !isPaused;
    } else if (c == 's' || c == 'S') {
        if (st == PLAYING) { isPaused = false; mx->stopSlot(kSlot); st = STOPPING; pendingIdx = -1; autoAdvance = false; }
        else if (st == STOPPING || st == TAIL) { pendingIdx = -1; autoAdvance = false; isPaused = false; }
    } else if (c == 'n' || c == 'N' || c == 'p' || c == 'P') {
        int dir = (c == 'n' || c == 'N') ? 1 : -1;
        int base = pendingIdx >= 0 ? pendingIdx : cur;          // pressing twice quickly skips two files
        pendingDir = dir;
        goTo(((base + dir) % n + n) % n);
    } else if (c == '+' || c == '=') {
        vol = std::max(-200, vol - kVolumeStep);
        mx->setVolume(vol);
    } else if (c == '-' || c == '_') {
        vol = std::min(512, vol + kVolumeStep);
        mx->setVolume(vol);
    } else if (c == '[' || c == ']') {
        if (st != IDLE) mx->setTempo(kSlot, std::max(1, std::min(999, mx->tempo(kSlot) + (c == ']' ? kTempoStep : -kTempoStep))));
    } else if (c >= 0 && c < 128 && tolower(c) >= 'a' && tolower(c) <= 'k') {
        int ch = toupper(c) - 'A';
        if (ch >= nChannels()) {
            char b[96];
            snprintf(b, sizeof b, "channel %c does not exist: this file is in the PC-88 dialect (channels A-F).", (char)('A' + ch));
            say(b);
        } else {
            mute ^= 1u << ch;
            mx->setMute(mute);
        }
    }
}

// ---- audio --------------------------------------------------------------------------------------------------------------------------------

bool Player::anyChannelActive() const {
    const Sequencer* s = mx->sequencer(kSlot);
    for (int i = 0; i < 11; i++) if (s->ch[i].active()) return true;
    return false;
}

void Player::renderBlock() {
    mx->render(carry, kBlock);
    carryPos = 0; carryLen = kBlock;

    int bl = 32767, bh = -32768;
    for (int i = 0; i < kBlock * 2; i++) {
        int v = carry[i];
        if (v < bl) bl = v;
        if (v > bh) bh = v;
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
    }
    lo[winPos] = bl; hi[winPos] = bh;
    winPos = (winPos + 1) % kWin;

    if ((st == PLAYING || st == STOPPING) && !anyChannelActive()) {
        // Every channel has stopped: the music is over (it ended by itself, or the fade finished).  Let the last notes die away.
        autoAdvance = (st == PLAYING);
        st = TAIL;
        tailBlocks = 0;
        return;
    }
    if (st == TAIL) {
        tailBlocks++;
        // Like mmlplay's one-pass file: the tail is over when the last 40 ms move by no more than 0.2 % of the track's loudest sample
        // (never less than the chip's own constant floor of 2 steps); give up after 5 s.
        bool over = tailBlocks * kBlock >= (long)opt.rate * 5;
        if (!over && tailBlocks >= kWin) {
            int l = 32767, h = -32768;
            for (int i = 0; i < kWin; i++) { if (lo[i] < l) l = lo[i]; if (hi[i] > h) h = hi[i]; }
            int limit = (int)(peak * 0.002) > 2 ? (int)(peak * 0.002) : 2;
            over = (h - l) <= limit;
        }
        if (over) afterTail();
    }
}

void Player::afterTail() {
    st = IDLE;
    if (pendingIdx >= 0) {
        int t = pendingIdx;
        pendingIdx = -1;
        startFrom(t, pendingDir, false);
    } else if (autoAdvance) {
        autoAdvance = false;
        if (cur + 1 < (int)files.size()) startFrom(cur + 1, +1, true);
        else { say("end of the playlist."); allDone = true; }
    }
}

void Player::pull(int16_t* out, int frames) {
    for (;;) {
        if (frames <= 0) return;
        if (isPaused || st == IDLE || !mx) { std::fill(out, out + 2 * (size_t)frames, (int16_t)0); return; }
        if (carryPos >= carryLen) { renderBlock(); continue; }       // may start the next track or go idle: look again
        int n = carryLen - carryPos;
        if (n > frames) n = frames;
        memcpy(out, carry + 2 * carryPos, (size_t)n * 2 * sizeof(int16_t));
        carryPos += n; out += 2 * n; frames -= n;
        delivered += n;
    }
}

void Player::pump(AudioOut& out, int chunk) {
    std::vector<int16_t> buf((size_t)chunk * 2);
    while (out.freeFrames() >= chunk) {
        pull(buf.data(), chunk);
        out.write(buf.data(), chunk);
    }
}

// ---- display ------------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> Player::takeMessages() {
    std::vector<std::string> r;
    r.swap(msgs);
    return r;
}

std::string Player::statusLine(int width) const {
    if (cur < 0 || !mx) return "";
    char b[64];
    snprintf(b, sizeof b, "[%d/%d] ", cur + 1, (int)files.size());
    std::string head = b;

    const Sequencer* s = mx->sequencer(kSlot);
    std::string chans;
    for (int i = 0; i < nChannels(); i++)
        chans += ((mute >> i) & 1) ? '.' : s->ch[i].active() ? (char)('A' + i) : '-';
    const char* state = isPaused ? "paused" : st == PLAYING ? "playing" : st == STOPPING ? "fading" : st == TAIL ? "ending" : "stopped";
    std::string time = mmss(delivered * 1000L / opt.rate) + "/" + (onceMs >= 0 ? mmss(onceMs) : "?:??");
    snprintf(b, sizeof b, "  %s  %s  vol %d  tempo %d  ", deviceName.c_str(), time.c_str(), vol, mx->tempo(kSlot));
    std::string rest = b + chans + "  " + state;

    int budget = width - 1 - (int)head.size() - (int)rest.size();
    std::string name = trackName;
    if (budget < 4) budget = 4;
    if ((int)name.size() > budget) name = name.substr(0, (size_t)budget - 1) + "~";
    return head + name + rest;
}

}  // namespace opnmml
