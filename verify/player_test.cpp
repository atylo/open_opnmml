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
// verify/player_test.sh builds and runs it, and also runs the mutation tests (the player is broken on purpose in fourteen ways: each must be caught).
// ================================================================================================================
#include "../player.hpp"
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
        p->key('+'); p->key('+'); p->key('=');
        CHECK(p->volume() == 20, "three presses of + : 32 -> 20 (4 per press)");
        Pcm more = pullBlocks(*p, 200);
        got.insert(got.end(), more.begin(), more.end());
        Ref r(t1); Pcm ref; r.blocks(ref, 100); r.m->setVolume(20); r.blocks(ref, 200);
        CHECK(same(got, ref) == ref.size(), "louder: identical to the engine with the same volume from the same frame");
        p->key('-'); p->key('_');
        CHECK(p->volume() == 28, "two presses of - : 20 -> 28");
        for (int i = 0; i < 100; i++) p->key('+');
        CHECK(p->volume() == -200, "+ stops at -200");
        for (int i = 0; i < 300; i++) p->key('-');
        CHECK(p->volume() == 512, "- stops at 512");
        pullBlocks(*p, 4);
        CHECK(std::string(p->statusLine()).find("vol 512") != std::string::npos, "status line shows the volume");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("4. channels");
    {
        auto p = makePlayer({f1});
        Pcm got = pullBlocks(*p, 100);
        p->key('C');
        CHECK(p->muteMask() == 4, "C switches channel C off (bit 2)");
        CHECK(std::string(p->statusLine()).find("tempo") != std::string::npos, "status line present");
        Pcm a = pullBlocks(*p, 100);
        p->key('c');
        CHECK(p->muteMask() == 0, "c (lower case) switches it on again");
        Pcm b = pullBlocks(*p, 100);
        got.insert(got.end(), a.begin(), a.end()); got.insert(got.end(), b.begin(), b.end());
        Ref r(t1); Pcm ref; r.blocks(ref, 100); r.m->setMute(4); r.blocks(ref, 100); r.m->setMute(0); r.blocks(ref, 100);
        CHECK(same(got, ref) == ref.size(), "mute and unmute identical to the engine");
        p->takeMessages();
        p->key('G');
        CHECK(p->muteMask() == 0 && hasMessage(p->takeMessages(), "does not exist"), "a PC-88 file has no channel G: message, nothing changes");

        // the X1 dialect has all eleven
        auto x = makePlayer({fx1});
        pullBlocks(*x, 4);
        x->key('K');
        CHECK(x->muteMask() == (1u << 10), "X1 file: K switches channel 11 off");
        std::string st = x->statusLine();
        size_t pos = st.find("tempo ");
        CHECK(pos != std::string::npos, "x1 status line");
        if (pos != std::string::npos) {
            size_t c = st.find("  ", pos) + 2;
            CHECK(st.size() >= c + 11 && st[c + 10] == '.', "the muted channel is shown as a dot");
        }
        // the channels come back with the next file
        auto y = makePlayer({f1, f2});
        pullBlocks(*y, 4); y->key('A'); y->key('n');
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
        CHECK(std::string(p->statusLine()).find("fading") != std::string::npos, "status line says fading");
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
        CHECK(std::string(p->statusLine()).find("stopped") != std::string::npos, "status line says stopped");
        p->key(' ');
        CHECK(p->state() == Player::PLAYING && p->framesDelivered() == 0 && p->index() == 0, "space after a stop plays the file again from the start");
        Pcm again = pullBlocks(*p, 5);
        CHECK(same(again, want) == again.size(), "the second play is identical to the first");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("7. next / previous");
    {
        Pcm want2; { Ref r(t2); r.blocks(want2, 30); }
        auto p = makePlayer({f1, f2, f3});
        pullBlocks(*p, 100);
        p->key('n');
        CHECK(p->state() == Player::STOPPING && p->index() == 0, "n starts the fade of the current file first");
        int n = 0; Pcm blk;
        while (p->index() == 0 && n < 3000) { blk = pullBlocks(*p, 1); n++; }
        CHECK(p->index() == 1 && p->state() == Player::PLAYING, "then the next file plays");
        Pcm got = blk;
        for (int i = 0; i < 20; i++) { Pcm b = pullBlocks(*p, 1); got.insert(got.end(), b.begin(), b.end()); }
        CHECK(same(got, want2) == got.size(), "the next file starts clean: identical to the engine from its first frame");

        auto settle = [](Player& q, int target) -> bool { for (int i = 0; i < 6000 && !(q.index() == target && q.state() == Player::PLAYING); i++) pullBlocks(q, 1); return q.index() == target; };
        auto a = makePlayer({f1, f2, f3}); pullBlocks(*a, 10); a->key('n'); a->key('n');
        CHECK(settle(*a, 2), "n twice in a row skips one file (1 is never played)");
        auto b = makePlayer({f1, f2, f3}); pullBlocks(*b, 10); b->key('p');
        CHECK(settle(*b, 2), "p at the first file wraps round to the last");
        b->key('n');
        CHECK(settle(*b, 0), "n at the last file wraps round to the first");
        b->key('n'); b->key('p');
        CHECK(settle(*b, 0), "n then p ends where it began");

        auto c = makePlayer({f1, f2, f3}); pullBlocks(*c, 10); c->key(' ');
        c->key('n');
        CHECK(c->index() == 1 && !c->paused() && c->state() == Player::PLAYING && c->framesDelivered() == 0, "n while paused switches at once");
        auto d = makePlayer({f1, f2, f3}); pullBlocks(*d, 10); d->key('s');
        for (int i = 0; i < 3000 && d->state() != Player::IDLE; i++) pullBlocks(*d, 1);
        d->key('p');
        CHECK(d->index() == 2 && d->state() == Player::PLAYING, "p after a stop switches at once");

        auto one = makePlayer({f1}); pullBlocks(*one, 10); one->key('n');
        CHECK(settle(*one, 0) && one->framesDelivered() < 10L * B, "n with a single file plays it again from the start");
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
        u->key('s');
        for (int i = 0; i < 2 * RATE / B; i++) pullBlocks(*u, 1);
        CHECK(u->state() == Player::IDLE && u->index() == 0, "s during the tail cancels the automatic advance");

        // a file that cannot be played is skipped on the way
        auto v = makePlayer({s1, bad, s1}, true);
        for (int i = 0; i < 20 * RATE / B && v->index() < 2 && !v->finishedPlaylist(); i++) pullBlocks(*v, 1);
        CHECK(v->index() == 2, "an automatic advance skips a file that cannot be played");
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("9. playlist: files and folders");
    {
        std::string dir = tmp + "/pl", sub = dir + "/sub";
        MKDIR(dir.c_str()); MKDIR(sub.c_str());
        const std::string txt = readText(s1);
        writeText(dir + "/b.MUS", txt); writeText(dir + "/a.mus", txt); writeText(dir + "/D.Mus", txt);
        writeText(dir + "/c.txt", txt); writeText(sub + "/x.mus", txt);
        Player::Options o; Player p(o);
        CHECK(p.addPath(dir) && p.trackCount() == 3, "a folder adds its .mus files only (any case), not the sub-folder");
        CHECK(p.trackPath(0) == dir + "/a.mus" && p.trackPath(1) == dir + "/b.MUS" && p.trackPath(2) == dir + "/D.Mus", "sorted by name, not by case");
        Player p2(o);
        CHECK(p2.addPath(dir + "/") && p2.trackCount() == 3, "a folder name with a trailing slash works");
        CHECK(p2.addPath(s1) && p2.trackCount() == 4 && p2.trackPath(3) == s1, "files and folders mix, in the order given");
        CHECK(p2.addPath(dir + "/c.txt") && p2.trackCount() == 5, "a file given by name is taken whatever its extension");
        Player p3(o);
        CHECK(!p3.addPath(tmp + "/missing.mus") && hasMessage(p3.takeMessages(), "does not exist"), "a missing path is reported");
        CHECK(!p3.addPath(sub + "x") , "another missing path");
        std::string empty = tmp + "/empty"; MKDIR(empty.c_str());
        p3.takeMessages();
        CHECK(!p3.addPath(empty) && hasMessage(p3.takeMessages(), "no .mus files"), "a folder without .mus files is reported");
        CHECK(p3.trackCount() == 0 && !p3.begin(), "nothing to play: begin() says so");
        remove((dir + "/b.MUS").c_str()); remove((dir + "/a.mus").c_str()); remove((dir + "/D.Mus").c_str());
        remove((dir + "/c.txt").c_str()); remove((sub + "/x.mus").c_str());
        RMDIR(sub.c_str()); RMDIR(dir.c_str()); RMDIR(empty.c_str());
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
        r->key('n');                                       // keys before/after are harmless
        Player::Options o; Player idle(o);
        idle.key('n'); idle.key(' '); idle.key('+'); idle.key('C');
        Pcm z = pullN(idle, 1000, {333});
        CHECK(allZero(z), "a player that has not begun is silent and ignores the keys");
        CHECK(idle.statusLine() == "", "and has no status line");

        auto s = makePlayer({f1, f2});
        std::vector<std::string> m = s->takeMessages();
        Ref ref(t1);
        std::string head = "1/2  " + f1 + "  [PC88]";
        CHECK(!m.empty() && m[0].compare(0, head.size(), head) == 0, "a started file is announced: number/count, path, machine");
        const std::string& title = ref.m->songTitle(3, 0);
        CHECK(title.empty() || (!m.empty() && m[0].find("\"" + title + "\"") != std::string::npos), "with its title");
        auto x = makePlayer({fx1}); std::vector<std::string> mx = x->takeMessages();
        CHECK(!mx.empty() && mx[0].find("[X1]") != std::string::npos, "#machine x1 plays on the X1 chip");
        auto z2 = makePlayer({mz}); std::vector<std::string> mm = z2->takeMessages();
        CHECK(!mm.empty() && mm[0].find("[MZ]") != std::string::npos && hasMessage(mm, "no FM sound") && hasMessage(mm, "channel(s) A"),
              "#machine mz: MZ chip, and a warning that the FM channel A is silent");
        auto q2 = makePlayer({f1}); q2->takeMessages();
        q2->key('?');
        CHECK(hasMessage(q2->takeMessages(), "space"), "? lists the keys");
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
        CHECK(st.compare(0, 7, "[1/3] b") == 0, "starts with [number/count] and the file name");
        char tempoText[32]; snprintf(tempoText, sizeof tempoText, "tempo %d  ", p->mixer()->tempo(3));
        CHECK(st.find("PC88") != std::string::npos && st.find("vol 32") != std::string::npos && st.find(tempoText) != std::string::npos, "machine, volume, tempo (the song's own)");
        CHECK(st.find("0:00/" + mmss(tm.onceMs)) != std::string::npos, "elapsed time / length of one pass (the length is the measured one)");
        pullBlocks(*p, 600);                                // 700 blocks = 2.03 s in all
        CHECK(p->statusLine().find("0:02/" + mmss(tm.onceMs)) != std::string::npos, "the clock runs");
        CHECK(st.size() >= 7 && st.compare(st.size() - 7, 7, "playing") == 0, "ends with the state");
        CHECK(st.size() <= 78, "fits a default console line");
        std::string longName = tmp + "/a_very_long_file_name_that_would_not_fit_on_one_console_line_at_all_for_sure.mus";
        writeText(longName, readText(fx1));
        auto q = makePlayer({longName});
        pullBlocks(*q, 10);
        std::string sl = q->statusLine();
        CHECK(sl.size() <= 78 && sl.find('~') != std::string::npos, "a long name is shortened so that the line still fits");
        CHECK(sl.size() >= 7 && sl.compare(sl.size() - 7, 7, "playing") == 0, "and the state is still there");
        CHECK(p->statusLine(66).size() <= 65 && p->statusLine(66).find('~') != std::string::npos, "the width can be chosen");
        remove(longName.c_str());
    }

    // -----------------------------------------------------------------------------------------------------------------------------------
    section("12. random keys for 3 minutes of sound (no crash, no hang, sane state)");
    {
        auto p = makePlayer({f1, f2, f3, s1, s2, fx1});
        unsigned seed = 777;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        const char keys[] = " snpnp+-+-[]abcdefghijkABCDEFGHIJK?";
        long frames = 0; bool ok = true;
        while (frames < 180L * RATE && ok) {
            if (rnd() % 15 == 0) p->key(keys[rnd() % (sizeof keys - 1)]);
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

    // clean up
    remove(s1.c_str()); remove(s2.c_str()); remove(bad.c_str()); remove(mz.c_str());
    RMDIR(tmp.c_str());

    printf("\n%d checks, %d failed\n", checks, fails);
    printf(fails ? "PLAYER TEST FAILED\n" : "player test: all checks passed\n");
    return fails ? 1 : 0;
}
