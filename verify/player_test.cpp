// ================================================================================================================
// player_test - tests of the interactive player's core (player.cpp) WITHOUT a sound card and without the original DLL.
//
//   player_test PROJECT_ROOT
//
// What is checked, and how:
//   * The sound the player hands out is compared sample for sample with what the engine itself produces when it is driven directly
//     (a "reference": a fresh Mixer, device PC88, volume 32, song 0 in slot 3, rendered in blocks of 128 frames).  Every key is checked the same
//     way: the reference gets the same operation at the same frame, and the two streams must stay identical.
//   * Pulling the sound in different chunk sizes (1 frame ... 50000) and through a fake sound device must give the same stream.
//   * Playlist handling, next / previous with wrap-around, the stop fade (its length is measured on the engine), once / loop mode, files that
//     cannot be played, the status line, and a long run of random keys (build this test with -fsanitize=address,undefined to catch memory errors).
// verify/player_test.sh builds and runs it, and also runs the mutation tests (the player is broken on purpose in 24 ways: each must be caught).
// ================================================================================================================
#include "player.hpp"
#include "screen.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define RMDIR(p) rmdir(p)
#endif

using namespace opnmml;

static int checks = 0, fails = 0;
#define CHECK(cond, what) do { ++checks; if (!(cond)) { ++fails; printf("  FAIL (line %d): %s\n", __LINE__, what); } } while (0)
static void section(const char* s) { printf("%s\n", s); }

static const int RATE = 44100;
static const int B = Player::kBlock;
typedef std::vector<int16_t> Pcm;

static std::string readText(const std::string& p) {
    std::ifstream f(p.c_str(), std::ios::binary);
    if (!f) { fprintf(stderr, "cannot read %s\n", p.c_str()); exit(2); }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static void writeText(const std::string& p, const std::string& t) { std::ofstream f(p.c_str(), std::ios::binary); f << t; }

// The engine driven directly: what the player must sound like.
struct Ref {
    std::unique_ptr<Mixer> m;
    explicit Ref(const std::string& text, int device = Mixer::DEVICE_PC88, bool once = false) {
        m.reset(new Mixer(RATE));
        m->setDevice(device);
        m->setVolume(32);
        m->setMute(0);
        if (once) m->setLooping(3, false);
        if (!m->loadSong(3, 0, text.c_str(), true)) { fprintf(stderr, "reference: cannot load the song\n"); exit(2); }
    }
    void blocks(Pcm& out, int n) {
        size_t o = out.size();
        out.resize(o + (size_t)n * B * 2);
        for (int i = 0; i < n; i++) m->render(&out[o + (size_t)i * B * 2], B);
    }
    bool anyActive() const {
        const Sequencer* s = m->sequencer(3);
        for (int i = 0; i < 11; i++) if (s->ch[i].active()) return true;
        return false;
    }
};

static std::unique_ptr<Player> makePlayer(const std::vector<std::string>& paths, bool once = false, bool begin = true) {
    Player::Options o; o.once = once;
    std::unique_ptr<Player> p(new Player(o));
    for (size_t i = 0; i < paths.size(); i++) p->addPath(paths[i]);
    if (begin) p->begin();
    return p;
}

// pull `frames` frames, the chunk sizes taken in turn from `sizes`
static Pcm pullN(Player& p, long frames, const std::vector<int>& sizes) {
    Pcm out((size_t)frames * 2);
    long done = 0; size_t k = 0;
    while (done < frames) {
        long n = sizes[k++ % sizes.size()];
        if (n > frames - done) n = frames - done;
        p.pull(&out[(size_t)done * 2], (int)n);
        done += n;
    }
    return out;
}
static Pcm pullBlocks(Player& p, int n) { return pullN(p, (long)n * B, std::vector<int>(1, B)); }

// number of leading samples that are equal
static size_t same(const Pcm& a, const Pcm& b, size_t from = 0, size_t n = (size_t)-1) {
    size_t lim = a.size() < b.size() ? a.size() : b.size();
    if (n != (size_t)-1 && from + n < lim) lim = from + n;
    size_t i = from;
    while (i < lim && a[i] == b[i]) i++;
    return i - from;
}
static bool allZero(const Pcm& a, size_t from = 0) { for (size_t i = from; i < a.size(); i++) if (a[i]) return false; return true; }
static bool hasMessage(const std::vector<std::string>& m, const char* part) {
    for (size_t i = 0; i < m.size(); i++) if (m[i].find(part) != std::string::npos) return true;
    return false;
}
static std::string mmss(long ms) { char b[32]; snprintf(b, sizeof b, "%ld:%02ld", ms / 1000 / 60, ms / 1000 % 60); return b; }

// A sound device that counts time by itself: it plays `RATE` frames per second of the time it is told about, and holds at most 2048 frames.
struct FakeOut : AudioOut {
    Pcm captured; long queued = 0; bool overflow = false, opened = false;
    bool open(int, std::string&) override { opened = true; return true; }
    int freeFrames() override { return (int)(2048 - queued); }
    void write(const int16_t* s, int frames) override {
        if (frames > freeFrames()) overflow = true;
        captured.insert(captured.end(), s, s + (size_t)frames * 2);
        queued += frames;
    }
    void close() override {}
    void advance(int ms) { queued -= (long)RATE * ms / 1000; if (queued < 0) queued = 0; }
};

// A console with a cursor, a fixed number of rows and scrolling at the bottom (like a real one): the screen block is tested against it.
struct FakeConsole : Terminal {
    int W, R, r = 0, c = 0; long scrolls = 0;
    std::vector<std::string> rows;
    FakeConsole(int w, int h) : W(w), R(h), rows((size_t)h, std::string((size_t)w, ' ')) {}
    int width() override { return W; }
    void lineFeed() { if (r == R - 1) { rows.erase(rows.begin()); rows.push_back(std::string((size_t)W, ' ')); scrolls++; } else r++; }
    void write(const std::string& t) override { for (size_t i = 0; i < t.size(); i++) { if (c >= W) { c = 0; lineFeed(); } rows[(size_t)r][(size_t)c++] = t[i]; } }
    void newline() override { c = 0; lineFeed(); }
    void up(int n) override { r -= n; if (r < 0) r = 0; c = 0; }
    void flush() override {}
    // what is on the screen: trailing blanks of every row and trailing blank rows are cut off
    std::vector<std::string> screen() const {
        std::vector<std::string> v;
        for (size_t i = 0; i < rows.size(); i++) { size_t e = rows[i].find_last_not_of(' '); v.push_back(e == std::string::npos ? "" : rows[i].substr(0, e + 1)); }
        while (!v.empty() && v.back().empty()) v.pop_back();
        return v;
    }
    bool lastColumnUsed() const { for (size_t i = 0; i < rows.size(); i++) if (rows[i][(size_t)W - 1] != ' ') return true; return false; }
};
typedef std::vector<std::string> Lines;
static std::string rtrim(const std::string& s) { size_t e = s.find_last_not_of(' '); return e == std::string::npos ? "" : s.substr(0, e + 1); }

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "..";
    const std::string f1 = root + "/bgm/bgm01_fm3.mus", f2 = root + "/bgm/bgm02_fm3.mus", f3 = root + "/bgm/bgm03_fm3.mus";
    const std::string fx1 = root + "/bgm/chama/bgm01_opm.mus";      // #machine x1: the X1 dialect, channels A-K
    const std::string t1 = readText(f1), t2 = readText(f2);

    std::string tmp = "player_test_tmp";
    MKDIR(tmp.c_str());
    const std::string s1 = tmp + "/short.mus", s2 = tmp + "/looping.mus", bad = tmp + "/bad.mus", mz = tmp + "/mz.mus";
    writeText(s1, "A t240 o4 l8 cdef\r\n");            // 0.5 s, no loop point: ends by itself
    writeText(s2, "A L t240 o4 l8 cdef\r\n");          // the same with a loop point: plays for ever
    writeText(bad, "A c]\r\n");                        // a stray ] : the compiler rejects it
    writeText(mz, "#machine mz\r\nA t120 c4\r\nD t120 c4\r\n");   // FM channel A on the MZ chip: silent
    const Pcm none;

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("1. the sound is the engine's sound, whatever the chunk size");
    const int NB = 2100;                                    // 268800 frames = 6.1 s
    Pcm want; { Ref r(t1); r.blocks(want, NB); }
    {
        const std::vector<std::vector<int>> policies = { {128}, {1}, {7, 333, 100}, {4096}, {1, 127, 129, 255}, {50000} };
        for (size_t k = 0; k < policies.size(); k++) {
            auto p = makePlayer({f1});
            Pcm got = pullN(*p, (long)NB * B, policies[k]);
            char what[96]; snprintf(what, sizeof what, "stream identical to the engine, chunk policy %d (first chunk %d)", (int)k, policies[k][0]);
            CHECK(same(got, want) == want.size(), what);
        }
        CHECK(!allZero(want), "the reference is not silent (otherwise the comparison proves nothing)");
        auto p = makePlayer({f1});
        pullBlocks(*p, 10);
        CHECK(p->framesDelivered() == 10L * B, "framesDelivered counts the frames handed out");
    }
    {
        // through a fake sound device that frees room at random
        auto p = makePlayer({f1});
        FakeOut out; unsigned seed = 12345;
        while ((long)out.captured.size() / 2 < (long)NB * B) {
            p->pump(out, 512);
            seed = seed * 1103515245u + 12345u;
            out.advance(1 + (int)((seed >> 16) % 30));
        }
        CHECK(!out.overflow, "pump never writes more than the device has room for");
        CHECK(same(out.captured, want, 0, want.size()) == want.size(), "stream through the fake device identical to the engine");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("2. pause");
    {
        auto p = makePlayer({f1});
        Pcm got = pullN(*p, 10001, {10001});                // pause at a frame that is not a block boundary
        p->key(' ');
        CHECK(p->paused(), "space pauses");
        Pcm quiet = pullN(*p, 3000, {1000});
        CHECK(allZero(quiet), "silence while paused");
        CHECK(p->framesDelivered() == 10001, "paused: time stands still");
        CHECK(std::string(p->statusLine()).find("paused") != std::string::npos, "status line says paused");
        p->key(' ');
        CHECK(!p->paused(), "space continues");
        Pcm more = pullN(*p, 20000, {777});
        got.insert(got.end(), more.begin(), more.end());
        CHECK(same(got, want, 0, got.size()) == got.size(), "after the pause the sound goes on exactly where it stopped");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("3. volume");
    {
        auto p = makePlayer({f1});
        Pcm got = pullBlocks(*p, 100);
        p->key(Player::kKeyUp); p->key(Player::kKeyUp); p->key(Player::kKeyUp);
        CHECK(p->volume() == 20, "three presses of Up: 32 -> 20 (4 per press)");
        Pcm more = pullBlocks(*p, 200);
        got.insert(got.end(), more.begin(), more.end());
        Ref r(t1); Pcm ref; r.blocks(ref, 100); r.m->setVolume(20); r.blocks(ref, 200);
        CHECK(same(got, ref) == ref.size(), "louder: identical to the engine with the same volume from the same frame");
        p->key(Player::kKeyDown); p->key(Player::kKeyDown);
        CHECK(p->volume() == 28, "two presses of Down: 20 -> 28");
        p->key('+'); p->key('-'); p->key('='); p->key('_');
        CHECK(p->volume() == 28, "+ and - no longer change the volume");
        for (int i = 0; i < 100; i++) p->key(Player::kKeyUp);
        CHECK(p->volume() == -200, "Up stops at -200");
        for (int i = 0; i < 300; i++) p->key(Player::kKeyDown);
        CHECK(p->volume() == 512, "Down stops at 512");
        pullBlocks(*p, 4);
        CHECK(std::string(p->statusLine()).find("vol 512") != std::string::npos, "status line shows the volume");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("4. channels (keys 1 2 3 4 5 6 7 8 9 0 K)");
    {
        auto p = makePlayer({f1});
        Pcm got = pullBlocks(*p, 100);
        p->key('3');
        CHECK(p->muteMask() == 4, "key 3 switches channel 3 (C) off (bit 2)");
        Pcm a = pullBlocks(*p, 100);
        p->key('3');
        CHECK(p->muteMask() == 0, "key 3 again switches it on");
        Pcm b = pullBlocks(*p, 100);
        got.insert(got.end(), a.begin(), a.end()); got.insert(got.end(), b.begin(), b.end());
        Ref r(t1); Pcm ref; r.blocks(ref, 100); r.m->setMute(4); r.blocks(ref, 100); r.m->setMute(0); r.blocks(ref, 100);
        CHECK(same(got, ref) == ref.size(), "mute and unmute identical to the engine");
        p->takeMessages();
        p->key('7');
        CHECK(p->muteMask() == 0 && p->takeMessages().empty(), "a PC-88 file has channels A-F only: key 7 does nothing and says nothing");
        p->key('0'); p->key('k');
        p->takeMessages();
        CHECK(p->muteMask() == 0, "0 and K (channels 10 and 11) do nothing in the PC-88 dialect");
        p->key('c'); p->key('A'); p->key('g'); p->key('n'); p->key('p');
        CHECK(p->muteMask() == 0 && p->index() == 0 && p->state() == Player::PLAYING, "letters do not mute channels, n and p do not change the file");

        // the X1 dialect has all eleven, on the keys 1 ... 9, 0, K
        auto x = makePlayer({fx1});
        pullBlocks(*x, 4);
        const char keys[] = "1234567890K";
        for (int i = 0; i < 11; i++) {
            char what[96];
            x->key(keys[i]);
            snprintf(what, sizeof what, "key %c switches channel %d off (and only that one)", keys[i], i + 1);
            CHECK(x->muteMask() == (1u << i), what);
            x->key(keys[i]);
            snprintf(what, sizeof what, "key %c again switches channel %d on", keys[i], i + 1);
            CHECK(x->muteMask() == 0, what);
        }
        x->key('k'); CHECK(x->muteMask() == (1u << 10), "k (lower case) works like K"); x->key('K');
        x->key('1');
        CHECK(x->statusLine().find(".BCDEFGHIJK") != std::string::npos, "the status line shows a switched-off channel as a dot");
        x->key('1');
        CHECK(x->statusLine().find("ABCDEFGHIJK") != std::string::npos, "and the channels by their letters A to K (the keys are numbers)");
        // the channels come back with the next file
        auto y = makePlayer({f1, f2});
        pullBlocks(*y, 4); y->key('1'); y->key(Player::kKeyRight);
        for (int i = 0; i < 4000 && y->index() == 0; i++) pullBlocks(*y, 1);
        CHECK(y->index() == 1 && y->muteMask() == 0, "next file: all channels on again");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("5. tempo");
    {
        auto p = makePlayer({f1});
        Pcm got = pullBlocks(*p, 100);
        int before = p->mixer()->tempo(3);
        p->key(']'); p->key(']');
        CHECK(p->mixer()->tempo(3) == before + 10, "] adds 5 beats per minute per press");
        Pcm more = pullBlocks(*p, 200);
        got.insert(got.end(), more.begin(), more.end());
        Ref r(t1); Pcm ref; r.blocks(ref, 100); r.m->setTempo(3, r.m->tempo(3) + 10); r.blocks(ref, 200);
        CHECK(same(got, ref) == ref.size(), "faster: identical to the engine with the same tempo from the same frame");
        for (int i = 0; i < 400; i++) p->key('[');
        CHECK(p->mixer()->tempo(3) == 1, "[ stops at 1");
        for (int i = 0; i < 400; i++) p->key(']');
        CHECK(p->mixer()->tempo(3) == 999, "] stops at 999");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("6. stop = the game's quick fade, then silence; space plays again");
    {
        Ref meas(t1); Pcm skip; meas.blocks(skip, 100);
        meas.m->stopSlot(3);
        int fadeBlocks = 0;
        while (meas.anyActive() && fadeBlocks < 3000) { meas.blocks(skip, 1); fadeBlocks++; }
        double fadeSec = (double)fadeBlocks * B / RATE;
        char msg[96]; snprintf(msg, sizeof msg, "the engine's stop fade lasts about 0.26 s (measured %.3f s)", fadeSec);
        CHECK(fadeSec > 0.2 && fadeSec < 0.35, msg);

        auto p = makePlayer({f1});
        Pcm got = pullBlocks(*p, 100);
        p->key('s');
        CHECK(p->state() == Player::STOPPING, "s starts the fade");
        CHECK(p->statusLine().compare(0, 7, "stopped") == 0, "status line says stopped at once (the short fade is not a state of its own)");
        int n = 0;
        while (p->state() != Player::IDLE && n < 3000) { Pcm b = pullBlocks(*p, 1); got.insert(got.end(), b.begin(), b.end()); n++; }
        CHECK(p->state() == Player::IDLE, "the sound dies away and the player goes idle");
        CHECK(n * B > (int)(fadeSec * RATE) && n * B < 6 * RATE, "the fade plus the tail take less than 6 s");
        Ref r(t1); Pcm ref; r.blocks(ref, 100); r.m->stopSlot(3); r.blocks(ref, n);
        size_t whole = got.size() - (size_t)B * 2;            // the very last block is the silent one that ended the tail: it is not handed out
        CHECK(same(got, ref, 0, whole) == whole, "stop: identical to the engine with stopSlot at the same frame (fade and tail)");
        CHECK(allZero(got, whole), "the block that ended the tail is silent");
        CHECK(allZero(pullN(*p, 5000, {5000})), "idle: silence");
        CHECK(!p->finishedPlaylist(), "a stop is not the end of the playlist");
        CHECK(p->statusLine().compare(0, 7, "stopped") == 0 && p->statusLine().find("--.-") == std::string::npos, "status line says stopped");
        p->key(' ');
        CHECK(p->state() == Player::PLAYING && p->framesDelivered() == 0 && p->index() == 0, "space after a stop plays the file again from the start");
        Pcm again = pullBlocks(*p, 5);
        CHECK(same(again, want) == again.size(), "the second play is identical to the first");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("7. next / previous (Right / Left arrow)");
    {
        Pcm want2; { Ref r(t2); r.blocks(want2, 30); }
        auto p = makePlayer({f1, f2, f3});
        pullBlocks(*p, 100);
        p->key(Player::kKeyRight);
        CHECK(p->state() == Player::STOPPING && p->index() == 0, "Right starts the fade of the current file first");
        CHECK(p->statusLine().compare(0, 7, "playing") == 0, "the status line keeps saying playing while the next file is coming");
        int n = 0; Pcm blk;
        while (p->index() == 0 && n < 3000) { blk = pullBlocks(*p, 1); n++; }
        CHECK(p->index() == 1 && p->state() == Player::PLAYING, "then the next file plays");
        Pcm got = blk;
        for (int i = 0; i < 20; i++) { Pcm b = pullBlocks(*p, 1); got.insert(got.end(), b.begin(), b.end()); }
        CHECK(same(got, want2) == got.size(), "the next file starts clean: identical to the engine from its first frame");

        auto settle = [](Player& q, int target) -> bool { for (int i = 0; i < 6000 && !(q.index() == target && q.state() == Player::PLAYING); i++) pullBlocks(q, 1); return q.index() == target; };
        auto a = makePlayer({f1, f2, f3}); pullBlocks(*a, 10); a->key(Player::kKeyRight); a->key(Player::kKeyRight);
        CHECK(settle(*a, 2), "Right twice in a row skips one file (1 is never played)");
        auto b = makePlayer({f1, f2, f3}); pullBlocks(*b, 10); b->key(Player::kKeyLeft);
        CHECK(settle(*b, 2), "Left at the first file wraps round to the last");
        b->key(Player::kKeyRight);
        CHECK(settle(*b, 0), "Right at the last file wraps round to the first");
        b->key(Player::kKeyRight); b->key(Player::kKeyLeft);
        CHECK(settle(*b, 0), "Right then Left ends where it began");

        auto c = makePlayer({f1, f2, f3}); pullBlocks(*c, 10); c->key(' ');
        c->key(Player::kKeyRight);
        CHECK(c->index() == 1 && !c->paused() && c->state() == Player::PLAYING && c->framesDelivered() == 0, "Right while paused switches at once");
        auto d = makePlayer({f1, f2, f3}); pullBlocks(*d, 10); d->key('s');
        for (int i = 0; i < 3000 && d->state() != Player::IDLE; i++) pullBlocks(*d, 1);
        d->key(Player::kKeyLeft);
        CHECK(d->index() == 2 && d->state() == Player::PLAYING, "Left after a stop switches at once");

        auto one = makePlayer({f1}); pullBlocks(*one, 10); one->key(Player::kKeyRight);
        CHECK(settle(*one, 0) && one->framesDelivered() < 10L * B, "Right with a single file plays it again from the start");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("8. the end of a song: loop for ever, or once");
    {
        auto p = makePlayer({s2});
        long frames = 0;
        for (int i = 0; i < 30 * RATE / B; i++) { pullBlocks(*p, 1); frames += B; }
        CHECK(p->state() == Player::PLAYING && p->index() == 0 && !p->finishedPlaylist(), "a song with a loop point plays for ever (30 s)");

        auto q = makePlayer({s1, s2, s1}, true);
        std::vector<int> when(3, -1);
        long t = 0;
        for (int i = 0; i < 60 * RATE / B && !q->finishedPlaylist(); i++) {
            pullBlocks(*q, 1); t++;
            if (q->index() >= 1 && when[1] < 0) when[1] = (int)t;
            if (q->index() >= 2 && when[2] < 0) when[2] = (int)t;
        }
        CHECK(when[1] > 0 && when[2] > when[1], "--once: every song ends, the next one starts");
        CHECK(when[1] * B > RATE / 2 && when[1] * B < 6 * RATE, "the first song (0.513 s) ends after 0.5 s and a short tail");
        CHECK(q->finishedPlaylist(), "--once: the end of the last song ends the playlist");
        CHECK(hasMessage(q->takeMessages(), "end of the playlist"), "message at the end of the playlist");

        auto r = makePlayer({s1, s2});                     // loop mode: a song WITHOUT a loop point still ends by itself
        for (int i = 0; i < 6 * RATE / B && r->index() == 0; i++) pullBlocks(*r, 1);
        CHECK(r->index() == 1, "loop mode: a song without a loop point ends and the next file follows");
        for (int i = 0; i < 20 * RATE / B; i++) pullBlocks(*r, 1);
        CHECK(r->index() == 1 && !r->finishedPlaylist(), "and the last one, with a loop point, plays on");

        auto s = makePlayer({s1});
        for (int i = 0; i < 6 * RATE / B && !s->finishedPlaylist(); i++) pullBlocks(*s, 1);
        CHECK(s->finishedPlaylist(), "a single song that ends by itself ends the playlist");

        auto u = makePlayer({s1, s2});                     // a key pressed during the natural tail wins over the automatic advance
        for (int i = 0; i < 6 * RATE / B && u->state() != Player::TAIL; i++) pullBlocks(*u, 1);
        CHECK(u->state() == Player::TAIL, "reached the tail of the first song");
        CHECK(u->statusLine().compare(0, 7, "playing") == 0, "the natural dying away of the last notes is not a state of its own");
        u->key('s');
        CHECK(u->statusLine().compare(0, 7, "stopped") == 0, "s during the tail: stopped");
        for (int i = 0; i < 2 * RATE / B; i++) pullBlocks(*u, 1);
        CHECK(u->state() == Player::IDLE && u->index() == 0, "s during the tail cancels the automatic advance");

        // a file that cannot be played is skipped on the way
        auto v = makePlayer({s1, bad, s1}, true);
        for (int i = 0; i < 20 * RATE / B && v->index() < 2 && !v->finishedPlaylist(); i++) pullBlocks(*v, 1);
        CHECK(v->index() == 2, "an automatic advance skips a file that cannot be played");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("8b. switching between loop and once while playing (key l)");
    {
        const std::string t_s2 = readText(s2);
        auto p = makePlayer({s2});
        Pcm got = pullBlocks(*p, 431);                      // 1.25 s of a song that loops for ever (its passes end at 0.513 s, 1.014 s, 1.515 s ...)
        CHECK(!p->playOnce() && p->statusLine().find("  loop  ") != std::string::npos, "the default is loop, and the status line says so");
        p->key('l');
        CHECK(p->playOnce() && p->statusLine().find("  once  ") != std::string::npos, "l switches to once, and the status line says so");
        int n = 0;
        while (p->state() != Player::IDLE && n < 3000) { Pcm b = pullBlocks(*p, 1); got.insert(got.end(), b.begin(), b.end()); n++; }
        CHECK(p->state() == Player::IDLE && p->finishedPlaylist(), "once: the song ends after the pass that was running (and with it the one-file playlist)");
        Ref r(t_s2); Pcm ref; r.blocks(ref, 431); r.m->setLooping(3, false); r.blocks(ref, n);
        size_t whole = got.size() - (size_t)B * 2;
        CHECK(same(got, ref, 0, whole) == whole, "identical to the engine switched to once at the same frame");
        CHECK(n > 0 && n * B < 5 * RATE, "and it ends soon: at the end of the running pass, plus the short tail");

        auto q = makePlayer({s2});
        Pcm gq = pullBlocks(*q, 431);
        q->key('l'); Pcm g2 = pullBlocks(*q, 10); q->key('l');
        CHECK(!q->playOnce(), "l twice is loop again");
        gq.insert(gq.end(), g2.begin(), g2.end());
        Pcm g3 = pullBlocks(*q, 3000); gq.insert(gq.end(), g3.begin(), g3.end());
        for (int i = 0; i < 20 * RATE / B; i++) pullBlocks(*q, 1);
        CHECK(q->state() == Player::PLAYING && !q->finishedPlaylist(), "switched back to loop in time: it plays on for ever");
        Ref r2(t_s2); Pcm ref2; r2.blocks(ref2, 431 + 10 + 3000);
        CHECK(same(gq, ref2) == ref2.size(), "switching there and back does not change the sound");

        Player::Options o; o.once = true;
        Player pp(o); pp.addPath(s2); pp.addPath(s2); pp.begin();
        CHECK(pp.playOnce() && pp.statusLine().find("  once  ") != std::string::npos, "--once: the player starts in once");
        pp.key('l');
        CHECK(!pp.playOnce(), "and l switches it to loop");

        auto w = makePlayer({s1, s2});                     // the mode stays for the next file: s2 has a loop point but now plays once
        w->key('l');
        for (int i = 0; i < 30 * RATE / B && !w->finishedPlaylist(); i++) pullBlocks(*w, 1);
        CHECK(w->finishedPlaylist() && w->index() == 1, "the chosen mode stays for the next file");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("9. playlist: files, folders and everything below them");
    {
        std::string dir = tmp + "/pl";
        std::vector<std::string> made, madeDirs;
        auto mk = [&](const std::string& d) { MKDIR(d.c_str()); madeDirs.push_back(d); };
        const std::string txt = readText(s1);
        auto mkf = [&](const std::string& f) { writeText(f, txt); made.push_back(f); };
        mk(dir); mk(dir + "/sub"); mk(dir + "/sub/deeper"); mk(dir + "/Asub"); mk(dir + "/zsub"); mk(dir + "/emptysub");
        mkf(dir + "/b.MUS"); mkf(dir + "/a.mus"); mkf(dir + "/D.Mus"); mkf(dir + "/c.txt");
        mkf(dir + "/sub/x.mus"); mkf(dir + "/sub/deeper/y.mus"); mkf(dir + "/Asub/q.mus"); mkf(dir + "/zsub/z.mus");
        Player::Options o; Player p(o);
        CHECK(p.addPath(dir) && p.trackCount() == 7, "a folder adds its .mus files and those of all the folders below it (any case; c.txt is not one)");
        const char* order[] = { "/a.mus", "/b.MUS", "/D.Mus", "/Asub/q.mus", "/sub/x.mus", "/sub/deeper/y.mus", "/zsub/z.mus" };
        bool inOrder = p.trackCount() == 7;
        for (int i = 0; i < 7 && inOrder; i++) inOrder = p.trackPath(i) == dir + order[i];
        CHECK(inOrder, "order: the files of a folder by name (not by case), then each sub-folder by name in the same way");
        Player p2(o);
        CHECK(p2.addPath(dir + "/") && p2.trackCount() == 7, "a folder name with a trailing slash works");
        CHECK(p2.addPath(s1) && p2.trackCount() == 8 && p2.trackPath(7) == s1, "files and folders mix, in the order given");
        CHECK(p2.addPath(dir + "/c.txt") && p2.trackCount() == 9, "a file given by name is taken whatever its extension");
        Player p3(o);
        CHECK(!p3.addPath(tmp + "/missing.mus") && hasMessage(p3.takeMessages(), "does not exist"), "a missing path is reported");
        std::string empty = tmp + "/empty", emptyInner = empty + "/inner"; mk(empty); mk(emptyInner);
        p3.takeMessages();
        CHECK(!p3.addPath(empty) && hasMessage(p3.takeMessages(), "no .mus files"), "a folder with no .mus files at all is reported");
        CHECK(p3.trackCount() == 0 && !p3.begin(), "nothing to play: begin() says so");

#ifndef _WIN32
        // a link that points back up is not followed (else the search would go round for ever)
        std::string loop = dir + "/loop";
        if (symlink(".", loop.c_str()) == 0) {
            Player p4(o);
            CHECK(p4.addPath(dir) && p4.trackCount() == 7, "a link to a folder is not followed");
            unlink(loop.c_str());
        }
#endif
        // folders more than 12 levels down are not searched
        std::string deep = tmp + "/deep";
        mk(deep);
        std::string d = deep;
        for (int i = 1; i <= 14; i++) {
            d += "/d" + std::to_string(i);
            mk(d);
            if (i == 3 || i == 12 || i == 13 || i == 14) mkf(d + "/f" + std::to_string(i) + ".mus");
        }
        Player p5(o);
        bool ok5 = p5.addPath(deep);
        CHECK(ok5 && p5.trackCount() == 2, "12 levels below the given folder are searched (the files at depth 3 and 12 are found), deeper ones are not (13 and 14)");
        CHECK(hasMessage(p5.takeMessages(), "12 levels"), "and that is said");

        for (size_t i = made.size(); i-- > 0;) remove(made[i].c_str());
        for (size_t i = madeDirs.size(); i-- > 0;) RMDIR(madeDirs[i].c_str());
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("10. files that cannot be played; what is said when a file starts");
    {
        auto p = makePlayer({bad, s1}, false, false);
        CHECK(p->begin() && p->index() == 1, "begin() skips a file with an error and plays the next");
        CHECK(hasMessage(p->takeMessages(), "bad.mus"), "the error names the file");
        auto q = makePlayer({bad, bad}, false, false);
        CHECK(!q->begin(), "nothing playable: begin() fails");
        CHECK(hasMessage(q->takeMessages(), "no file could be played"), "and says so");
        auto r = makePlayer({f1, f2});
        r->key(Player::kKeyRight);                         // keys before/after are harmless
        Player::Options o; Player idle(o);
        idle.key(Player::kKeyRight); idle.key(' '); idle.key(Player::kKeyUp); idle.key('3'); idle.key('l'); idle.key('h');
        Pcm z = pullN(idle, 1000, {333});
        CHECK(allZero(z), "a player that has not begun is silent and ignores the keys");
        CHECK(idle.statusLine() == "", "and has no status line");

        auto s = makePlayer({f1, f2});
        std::vector<std::string> m = s->screenLines();
        Ref ref(t1);
        const std::string& title = ref.m->songTitle(3, 0);
        CHECK(!title.empty() && m.size() == 3 && m[0] == title, "the screen: first line the title (no quotes, no path)");
        CHECK(m.size() == 3 && m[1] == "bgm01_fm3.mus   PC88   track 1 of 2", "second line: the file name, the machine and where it is in the list");
        CHECK(!hasMessage(m, "/bgm/") && !hasMessage(m, root.c_str()), "the folder is not shown");
        CHECK(s->takeMessages().empty(), "starting a file prints nothing (the screen lines are redrawn in place)");
        auto one = makePlayer({s1}); std::vector<std::string> mo = one->screenLines();
        CHECK(mo.size() == 3 && mo[0] == "" && mo[1] == "short.mus   PC88", "no title: an empty first line; one file: no track number");
        auto x = makePlayer({fx1});
        CHECK(hasMessage(x->screenLines(), "   X1"), "#machine x1 plays on the X1 chip");
        auto z2 = makePlayer({mz}); std::vector<std::string> mm = z2->takeMessages();
        CHECK(z2->screenLines()[1] == "mz.mus   MZ" && hasMessage(mm, "no FM sound") && hasMessage(mm, "channel(s) A"),
              "#machine mz: MZ chip, and a warning that the FM channel A is silent (a message that stays on the screen)");
        auto q2 = makePlayer({f1}); q2->takeMessages();
        CHECK(!q2->helpShown() && q2->screenLines().size() == 3, "the help is not shown at first: three lines");
        q2->key('?');
        CHECK(!q2->helpShown() && q2->screenLines().size() == 3, "? does nothing");
        q2->key('h');
        std::vector<std::string> help = q2->screenLines();
        CHECK(q2->helpShown() && help.size() == 13, "h adds the list of the keys below the three lines");
        CHECK(hasMessage(help, "space") && hasMessage(help, "Left / Right") && hasMessage(help, "Up / Down") && hasMessage(help, "1 2 ... 9 0 K")
              && hasMessage(help, "hide this list"), "the list names the keys");
        bool fits = !help.empty();
        for (size_t i = 3; i < help.size(); i++) if (help[i].size() > 66) fits = false;
        CHECK(fits, "every line is at most 66 characters: it fits even a narrow window");
        CHECK(q2->takeMessages().empty(), "the list is part of the screen, not printed as messages");
        q2->key('H');
        CHECK(!q2->helpShown() && q2->screenLines().size() == 3, "h again (or H) hides it");
        auto q3 = makePlayer({f1});
        q3->key('q'); CHECK(q3->wantsQuit(), "q quits");
        auto q4 = makePlayer({f1}); q4->key(27); CHECK(q4->wantsQuit(), "Esc quits");
        auto q5 = makePlayer({f1}); q5->key(3); CHECK(q5->wantsQuit(), "Ctrl-C (read as a key) quits");
        auto q6 = makePlayer({f1}); q6->key('x'); q6->key('7'); q6->key(300); q6->key(-1); CHECK(!q6->wantsQuit(), "other keys do nothing");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("11. status line");
    {
        auto p = makePlayer({f1, f2, f3});
        pullBlocks(*p, 100);
        std::string st = p->statusLine();
        Song song; compile(t1.c_str(), song); SongTiming tm; measureSong(song, 0, tm);
        CHECK(st.compare(0, 7, "playing") == 0, "the state comes first");
        char tempoText[32]; snprintf(tempoText, sizeof tempoText, "tempo %d  ", p->mixer()->tempo(3));
        CHECK(st.find("  loop  ") != std::string::npos && st.find("vol 32") != std::string::npos && st.find(tempoText) != std::string::npos, "loop or once, volume, tempo (the song's own)");
        CHECK(st.find("0:00 / " + mmss(tm.onceMs)) != std::string::npos, "elapsed time / length of one pass (the length is the measured one)");
        CHECK(st.find("ABC--F") != std::string::npos, "channels: the letter of a channel that plays, - for one without music (D and E here)");
        CHECK(st.size() >= 14 && st.compare(st.size() - 14, 14, "  h = controls") == 0, "ends with the hint for the help key");
        CHECK(st.size() <= 78, "fits a default console line");
        CHECK(st.find("bgm01") == std::string::npos && st.find("track") == std::string::npos && st.find("1/3") == std::string::npos
              && st.find("PC88") == std::string::npos && st.find('[') == std::string::npos,
              "file name, track number and machine are not repeated here (they are shown once, when the file starts)");
        CHECK(st.find("fading") == std::string::npos && st.find("ending") == std::string::npos, "no fading or ending message");
        pullBlocks(*p, 600);                                // 700 blocks = 2.03 s in all
        CHECK(p->statusLine().find("0:02 / " + mmss(tm.onceMs)) != std::string::npos, "the clock runs");
        std::string narrow = p->statusLine(60);
        CHECK(narrow.size() <= 59 && narrow.find("controls") == std::string::npos && narrow.find("ABC--F") != std::string::npos,
              "on a narrow console the hint is dropped first");
        CHECK(p->statusLine(73).find("h = controls") != std::string::npos, "and shown when there is room");
        p->key(' ');
        CHECK(p->statusLine().compare(0, 7, "paused ") == 0, "paused (the state field keeps its width, so the rest does not move)");
        auto x = makePlayer({fx1});
        pullBlocks(*x, 10);
        CHECK(x->statusLine().find("  ABCDEFGHIJK  ") != std::string::npos, "X1: all eleven channels, A to K");
        // the clock must not go wrong after 48 seconds (frames * 1000 does not fit in a 32-bit number; a long is 32 bits on Windows)
        auto c = makePlayer({s2});
        c->takeMessages();
        CHECK(sizeof(c->framesDelivered()) == 8, "the frame counter is 64 bits");
        long long total = 0;
        std::string at49, at61;
        for (int sec = 1; sec <= 61; sec++) {
            pullN(*c, RATE, {4410});
            total += RATE;
            if (sec == 49) at49 = c->statusLine();
            if (sec == 61) at61 = c->statusLine();
        }
        CHECK(at49.find("0:49 / ") != std::string::npos, "after 49 seconds the clock says 0:49 (not a negative number)");
        CHECK(at61.find("1:01 / ") != std::string::npos && at61.find(":-") == std::string::npos, "after 61 seconds it says 1:01");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("12. random keys for 3 minutes of sound (no crash, no hang, sane state)");
    {
        auto p = makePlayer({f1, f2, f3, s1, s2, fx1});
        unsigned seed = 777;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        const int keys[] = { ' ', 's', Player::kKeyLeft, Player::kKeyRight, Player::kKeyLeft, Player::kKeyRight, Player::kKeyUp, Player::kKeyDown, Player::kKeyUp, Player::kKeyDown, '[', ']', 'l', 'l', 'h',
                             '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'k', 'K', '?', 'a', 'n', 'p', 'x', '+', '-' };
        long frames = 0; bool ok = true;
        while (frames < 180L * RATE && ok) {
            if (rnd() % 15 == 0) p->key(keys[rnd() % (sizeof keys / sizeof keys[0])]);
            int n = 1 + (int)(rnd() % 3000);
            Pcm out((size_t)n * 2);
            p->pull(out.data(), n);
            frames += n;
            ok = p->index() >= 0 && p->index() < 6 && p->volume() >= -200 && p->volume() <= 512 && p->state() >= Player::IDLE && p->state() <= Player::TAIL
                 && p->mixer() && p->mixer()->tempo(3) >= 1 && p->statusLine().size() <= 78;
            p->takeMessages();
        }
        CHECK(ok, "state stays sane");
        CHECK(!p->wantsQuit(), "never quits by itself");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("13. the screen block: drawn once, then redrawn in place (simulated console that scrolls)");
    {
        // (a) at the bottom of a full window, 50 redraws: nothing is added, nothing is left over
        FakeConsole con(40, 8);
        for (int i = 1; i <= 6; i++) { con.write("old " + std::to_string(i)); con.newline(); }
        ScreenBlock sb(con);
        sb.draw({"Title", "file.mus   PC88", "playing  0:00"});
        CHECK(con.screen() == Lines({"old 2", "old 3", "old 4", "old 5", "old 6", "Title", "file.mus   PC88", "playing  0:00"}),
              "first draw: three lines below the earlier output (the window scrolled once to make room)");
        for (int t = 1; t <= 50; t++) sb.draw({"Title", "file.mus   PC88", "playing  0:" + std::string(t < 10 ? "0" : "") + std::to_string(t)});
        CHECK(con.screen() == Lines({"old 2", "old 3", "old 4", "old 5", "old 6", "Title", "file.mus   PC88", "playing  0:50"}),
              "after 50 redraws: still the same three lines, the earlier output untouched");
        CHECK(con.scrolls == 1 && sb.height() == 3, "and the window never scrolled again");
        sb.draw({"Another title", "other.mus   X1   track 2 of 9", "paused   "});
        CHECK(con.screen() == Lines({"old 2", "old 3", "old 4", "old 5", "old 6", "Another title", "other.mus   X1   track 2 of 9", "paused"}),
              "a new file: all three lines replaced in place (a shorter text leaves nothing of the longer one)");

        // (b) the list of the keys appears and disappears
        FakeConsole c2(40, 20);
        ScreenBlock b2(c2);
        Lines three = {"T", "F", "S1"}, thirteen = three;
        for (int i = 1; i <= 10; i++) thirteen.push_back("help " + std::to_string(i));
        b2.draw(three); b2.draw(thirteen);
        CHECK(c2.screen() == thirteen && b2.height() == 13, "the list of the keys is added below the three lines");
        thirteen[2] = "S2"; b2.draw(thirteen);
        CHECK(c2.screen() == thirteen, "and the status line goes on changing in place while it is shown");
        Lines back = {"T", "F", "S3"}; b2.draw(back);
        CHECK(c2.screen() == back && b2.height() == 3, "hidden again: the three lines remain, the rest is wiped");
        b2.draw(thirteen); b2.draw(back); b2.draw(thirteen);
        CHECK(c2.screen() == thirteen && c2.scrolls == 0, "showing and hiding again and again leaves nothing behind");

        // (c) a message that stays: printed where the block was, the block comes back below it
        FakeConsole c3(40, 20);
        ScreenBlock b3(c3);
        b3.draw(three);
        b3.printAbove({"error: bad.mus: oops", "warning: be careful"});
        CHECK(c3.screen() == Lines({"error: bad.mus: oops", "warning: be careful"}), "the block is erased and the two lines are printed in its place");
        b3.draw(three);
        CHECK(c3.screen() == Lines({"error: bad.mus: oops", "warning: be careful", "T", "F", "S1"}), "the block is drawn again below them");
        b3.draw(thirteen);
        b3.printAbove({"end of the playlist."});
        b3.draw(three);
        CHECK(c3.screen() == Lines({"error: bad.mus: oops", "warning: be careful", "end of the playlist.", "T", "F", "S1"}),
              "also when the block was tall (the list of the keys was open): no gap, nothing left over");

        // (d) a line that is too long is cut, and never reaches the last column
        FakeConsole c4(20, 6);
        ScreenBlock b4(c4);
        b4.draw({"0123456789012345678901234567890", "abc"});
        CHECK(c4.screen() == Lines({"0123456789012345678", "abc"}) && !c4.lastColumnUsed(), "a long line is cut to the width - 1 (the last column stays free)");
        b4.draw({"x", "y"});
        CHECK(c4.screen() == Lines({"x", "y"}), "and a shorter text afterwards wipes the rest of it");
        FakeConsole c5(2, 4);
        ScreenBlock b5(c5);
        b5.draw({"abc", "def"}); b5.draw({"gh", "i"});
        CHECK(c5.screen() == Lines({"g", "i"}), "a window of 2 columns does not crash");

        // (e) the end: the shell's prompt comes on a fresh line below the block
        FakeConsole c6(40, 10);
        ScreenBlock b6(c6);
        b6.draw(three); b6.finish(); c6.write("PROMPT>");
        CHECK(c6.screen() == Lines({"T", "F", "S1", "PROMPT>"}), "finish(): the block stays and the next text starts on a new line");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("14. the player's screen, as the front end draws it (simulated console)");
    {
        FakeConsole con(70, 24);
        ScreenBlock sb(con);
        Lines shown;
        auto refresh = [&](Player& p) {                         // what the front end does in its loop
            Lines m = p.takeMessages();
            if (!m.empty()) { sb.printAbove(m); shown.clear(); }
            Lines lines = p.screenLines(con.width());
            if (lines != shown) { sb.draw(lines); shown = lines; }
        };
        auto countRows = [&](const char* part) { int n = 0; Lines v = con.screen(); for (size_t i = 0; i < v.size(); i++) if (v[i].find(part) != std::string::npos) n++; return n; };
        auto tailIsShown = [&]() {                              // the last rows of the screen are exactly the lines that were drawn last
            Lines v = con.screen();
            if (v.size() < shown.size()) return false;
            for (size_t i = 0; i < shown.size(); i++) if (v[v.size() - shown.size() + i] != rtrim(shown[i].substr(0, (size_t)con.W - 1))) return false;
            return true;
        };
        auto p = makePlayer({f1, bad, f2, f3});
        refresh(*p);
        pullBlocks(*p, 300); refresh(*p);
        CHECK(countRows("track 1 of 4") == 1 && countRows("playing") == 1 && tailIsShown(), "the first file: title, file line, status line");
        pullBlocks(*p, 3000); refresh(*p);
        CHECK(countRows("playing") == 1 && countRows("0:0") == 1, "the clock goes on in the same line");

        p->key(Player::kKeyRight);                              // the next file (bad.mus) cannot be played: an error, then the one after it
        for (int i = 0; i < 4000 && p->index() == 0; i++) { pullBlocks(*p, 1); refresh(*p); }
        refresh(*p);
        CHECK(p->index() == 2, "the file with the error was skipped");
        CHECK(countRows("bad.mus") == 1, "the error is printed once, as an ordinary line");
        CHECK(countRows("track 1 of 4") == 0 && countRows("track 3 of 4") == 1 && countRows("bgm02_fm3.mus") == 1, "the file line was replaced in place, not added to");
        CHECK(countRows("playing") == 1 && tailIsShown(), "one status line, below everything else");
        size_t rowsBefore = con.screen().size();

        p->key('h'); refresh(*p);
        CHECK(con.screen().size() == rowsBefore + 10 && countRows("hide this list") == 1 && tailIsShown(), "h: the list of the keys appears below, in place");
        p->key('h'); refresh(*p);
        CHECK(con.screen().size() == rowsBefore && countRows("hide this list") == 0 && tailIsShown(), "h again: it is gone and nothing is left behind");
        for (int i = 0; i < 5; i++) { p->key('h'); refresh(*p); p->key('h'); refresh(*p); }
        CHECK(con.screen().size() == rowsBefore && tailIsShown(), "toggling it ten times changes nothing");

        // many files and keys: the screen never grows except by real messages, and always ends with what was drawn last
        auto q = makePlayer({f1, f2, f3, s1, s2, fx1});
        FakeConsole con2(79, 30); ScreenBlock sb2(con2); Lines shown2;
        unsigned seed = 4242; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        const int keys[] = { ' ', 's', Player::kKeyLeft, Player::kKeyRight, Player::kKeyUp, Player::kKeyDown, '[', ']', 'l', 'h', 'h', '1', '2', '3', '0', 'k' };
        bool ok = true; size_t maxRows = 0; long shownMessages = 0;
        for (int step = 0; step < 3000 && ok; step++) {
            if (rnd() % 12 == 0) q->key(keys[rnd() % (sizeof keys / sizeof keys[0])]);
            pullBlocks(*q, 1 + (int)(rnd() % 40));
            Lines m = q->takeMessages();
            if (!m.empty()) { sb2.printAbove(m); shown2.clear(); shownMessages += (long)m.size(); }
            Lines lines = q->screenLines(con2.width());
            if (lines != shown2) { sb2.draw(lines); shown2 = lines; }
            Lines v = con2.screen();
            if (v.size() < shown2.size()) ok = false;
            else for (size_t i = 0; i < shown2.size(); i++) if (v[v.size() - shown2.size() + i] != rtrim(shown2[i].substr(0, (size_t)con2.W - 1))) ok = false;
            if (v.size() > maxRows) maxRows = v.size();
        }
        CHECK(ok, "3000 random steps: the last rows of the screen are always exactly the lines that were drawn last");
        CHECK(maxRows <= 30 && con2.scrolls <= shownMessages + 20, "and the window scrolls only for real messages (and the first fill-up)");
    }

    // clean up
    remove(s1.c_str()); remove(s2.c_str()); remove(bad.c_str()); remove(mz.c_str());
    RMDIR(tmp.c_str());

    printf("\n%d checks, %d failed\n", checks, fails);
    printf(fails ? "PLAYER TEST FAILED\n" : "player test: all checks passed\n");
    return fails ? 1 : 0;
}
