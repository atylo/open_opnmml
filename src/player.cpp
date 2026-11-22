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

// "dir/bgm01_fm3.mus" -> "bgm01_fm3.mus"
std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

// dir + name, with the separator the user used for the folder (so a path typed as .\\bgm stays .\\bgm\\name)
std::string joinPath(const std::string& dir, const std::string& name) {
    if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) return dir + name;
    bool backslashes = dir.find('\\') != std::string::npos && dir.find('/') == std::string::npos;
    return dir + (backslashes ? '\\' : '/') + name;
}

std::string mmss(int64_t ms) {
    char b[48];
    long long s = ms / 1000;     // (a plain long is only 32 bits on Windows: frames * 1000 does not fit in it after 48 seconds)
    snprintf(b, sizeof b, "%lld:%02lld", s / 60, s % 60);
    return b;
}

bool isFolder(const std::string& p) {
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFDIR;
}

// a link (not followed when looking through sub-folders, so a link that points back up cannot make the search go round for ever)
bool isLink(const std::string& p) {
#ifdef _WIN32
    (void)p;
    return false;
#else
    struct stat sb;
    return lstat(p.c_str(), &sb) == 0 && (sb.st_mode & S_IFMT) == S_IFLNK;
#endif
}

bool nameLess(const std::string& a, const std::string& b) {
    std::string la = lower(a), lb = lower(b);
    return la != lb ? la < lb : a < b;
}

const int kMaxDepth = 12;       // the folder given is level 0; folders more than 12 levels below it are not searched

// The .mus files of a folder (sorted), then each sub-folder (sorted) in the same way.
void collectFolder(const std::string& dir, int depth, std::vector<std::string>& out, bool& tooDeep) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> fileNames, folderNames;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = joinPath(dir, n);
        if (isFolder(full)) { if (!isLink(full)) folderNames.push_back(n); }
        else if (hasMusExtension(n)) fileNames.push_back(n);
    }
    closedir(d);
    std::sort(fileNames.begin(), fileNames.end(), nameLess);
    std::sort(folderNames.begin(), folderNames.end(), nameLess);
    for (const std::string& n : fileNames) out.push_back(joinPath(dir, n));
    for (const std::string& n : folderNames) {
        if (depth + 1 > kMaxDepth) { tooDeep = true; continue; }
        collectFolder(joinPath(dir, n), depth + 1, out, tooDeep);
    }
}

const int kVolumeStep = 4;      // one press of + or -: 4 steps of the master volume = about 0.9 dB (27 steps = 6 dB)
const int kTempoStep = 5;       // one press of [ or ]: 5 beats per minute
const int kSlot = 3;            // the game plays its background music in slot 3
const char kChannelLetters[] = "ABCDEFGHIJK";   // as the music text and the status line call them

// the channel a key stands for (0 = A ... 10 = K), or -1
int channelOfKey(int c) {
    if (c >= '1' && c <= '9') return c - '1';
    if (c == '0') return 9;
    if (c == 'k' || c == 'K') return 10;
    return -1;
}

}  // namespace

Player::Player(const Options& o) : opt(o), once(o.once) {
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
    closedir(d);
    std::vector<std::string> found;
    bool tooDeep = false;
    collectFolder(p, 0, found, tooDeep);
    if (tooDeep) say("warning: folders nested more than 12 levels below '" + path + "' were not searched.");
    if (found.empty()) { say("warning: the folder '" + path + "' has no .mus files (nor do its sub-folders)."); return false; }
    for (const std::string& f : found) files.push_back(f);
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
        if (once) m->setLooping(kSlot, false);
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
    onceMs = haveTiming ? timing.onceMs : -1;
    delivered = 0; peak = 0; tailBlocks = 0; winPos = 0;
    carryPos = carryLen = 0;
    for (int i = 0; i < kWin; i++) lo[i] = hi[i] = 0;

    // What is shown while the file plays: its title (when it has one), then the file name, the machine and where it is in the list.
    titleText = mx->songTitle(kSlot, 0);
    infoText = fileName(path) + "   " + deviceName;
    if (files.size() > 1) {
        char b[48];
        snprintf(b, sizeof b, "   track %d of %d", idx + 1, (int)files.size());
        infoText += b;
    }
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

std::vector<std::string> Player::helpLines() {
    std::vector<std::string> h;
    h.push_back("  space          pause / continue (after a stop: play again)");
    h.push_back("  s              stop (the game's quick fade)");
    h.push_back("  Left / Right   previous / next file (wraps round)");
    h.push_back("  Up / Down      louder / quieter (about 0.9 dB per press)");
    h.push_back("  [ / ]          slower / faster by 5 beats per minute");
    h.push_back("  l              switch between loop and once (each song once)");
    h.push_back("  1 2 ... 9 0 K  channel A ... K off / on (1 = A ... 9 = I, 0 = J)");
    h.push_back("  h              hide this list");
    h.push_back("  q or Esc       quit");
    h.push_back("  channels: letter = plays, - = no music, . = switched off");
    return h;
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
    if (cur < 0 || !mx) return;
    int n = (int)files.size();

    if (c == ' ') {
        if (st == IDLE) { allDone = false; isPaused = false; pendingDir = 1; startFrom(cur, 1, false); }
        else isPaused = !isPaused;
    } else if (c == 's' || c == 'S') {
        if (st == PLAYING) { isPaused = false; mx->stopSlot(kSlot); st = STOPPING; pendingIdx = -1; autoAdvance = false; }
        else if (st == STOPPING || st == TAIL) { pendingIdx = -1; autoAdvance = false; isPaused = false; }
    } else if (c == kKeyLeft || c == kKeyRight) {
        int dir = (c == kKeyRight) ? 1 : -1;
        int base = pendingIdx >= 0 ? pendingIdx : cur;          // pressing twice quickly skips two files
        pendingDir = dir;
        goTo(((base + dir) % n + n) % n);
    } else if (c == 'l' || c == 'L') {
        once = !once;
        mx->setLooping(kSlot, !once);       // takes effect where a channel next reaches the end of its text
    } else if (c == 'h' || c == 'H') {
        showHelp = !showHelp;
    } else if (c == kKeyUp) {
        vol = std::max(-200, vol - kVolumeStep);
        mx->setVolume(vol);
    } else if (c == kKeyDown) {
        vol = std::min(512, vol + kVolumeStep);
        mx->setVolume(vol);
    } else if (c == '[' || c == ']') {
        if (st != IDLE) mx->setTempo(kSlot, std::max(1, std::min(999, mx->tempo(kSlot) + (c == ']' ? kTempoStep : -kTempoStep))));
    } else if (channelOfKey(c) >= 0) {
        int ch = channelOfKey(c);
        if (ch < nChannels()) {             // a PC-88 file has no channels G..K: the keys 7, 8, 9, 0 and K do nothing there
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

// What the status line calls the state.  The short fades (a stop, a change of file) and the dying away of the last notes are not shown as
// states of their own: after `s` it says "stopped" at once, and while the player moves on to another file it keeps saying "playing".
const char* Player::shownState() const {
    if (isPaused) return "paused";
    if (st == IDLE) return "stopped";
    if ((st == STOPPING || st == TAIL) && pendingIdx < 0 && !autoAdvance) return "stopped";
    return "playing";
}

std::string Player::statusLine(int width) const {
    if (cur < 0 || !mx) return "";
    const Sequencer* s = mx->sequencer(kSlot);
    std::string chans;
    for (int i = 0; i < nChannels(); i++)
        chans += ((mute >> i) & 1) ? '.' : s->ch[i].active() ? kChannelLetters[i] : '-';
    std::string time = mmss(delivered * 1000 / opt.rate) + " / " + (onceMs >= 0 ? mmss(onceMs) : "?:??");
    char b[96];
    snprintf(b, sizeof b, "%-7s  %s  %s  vol %d  tempo %d  ", shownState(), time.c_str(), once ? "once" : "loop", vol, mx->tempo(kSlot));
    std::string line = b + chans;
    const char* hint = "  h = controls";
    if ((int)(line.size() + strlen(hint)) <= width - 1) line += hint;
    return line;
}

std::vector<std::string> Player::screenLines(int width) const {
    std::vector<std::string> l;
    l.push_back(titleText);
    l.push_back(infoText);
    l.push_back(statusLine(width));
    if (showHelp) { std::vector<std::string> h = helpLines(); l.insert(l.end(), h.begin(), h.end()); }
    return l;
}

}  // namespace opnmml
