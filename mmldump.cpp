// ================================================================================================================
// mmldump - compile a .mus file and show what the compiler produced: header information, FM tone definitions,
// volume envelopes, and the bytecode of every channel (as hex, or as a readable listing).
//
// Build:  g++ -O2 -s -o mmldump.exe mmldump.cpp mmlcomp.cpp
// ================================================================================================================
#include "cli.hpp"
#include "mmlcomp.hpp"
#include "songlen.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace opnmml;

static const char* kHelp =
"mmldump - compile a .mus file and print the result\n"
"\n"
"USAGE\n"
"  mmldump INPUT.mus [options]\n"
"\n"
"OPTIONS (all optional)\n"
"  -l, --listing    print each channel as a readable opcode listing instead of raw hexadecimal bytes\n"
"  -h, --help       show this text\n"
"\n"
"OUTPUT: title, machine, the packed 26-byte tone records, volume envelopes, and for each channel its length,\n"
"        total length in ticks (192 ticks = one whole note) and loop point.\n"
"EXIT STATUS: 0 = success, 1 = file or MML error, 2 = bad command line\n";

// Bytecode listing.  Operand counts follow the sequencer's opcode dispatcher (seq.cpp, SeqCh::dispatch).
//   0x01-0x7E  note number, then a length (1 byte, or 2 bytes when the first has bit 7 set)
//   0x7F       rest, then a length
//   0x80-0xFF  control commands (below); 0x00 ends the channel
static void listing(const std::vector<uint8_t>& c) {
    size_t i = 0;
    while (i < c.size()) {
        unsigned op = c[i];
        printf("  %04zx: ", i);
        if (op == 0) { printf("END\n"); break; }
        if (op < 0x80) {
            unsigned len = c[i + 1];
            size_t n = 2;
            if (len & 0x80) { len = ((len & 0x7F) << 7) | c[i + 2]; n = 3; }
            if (op == 127) printf("REST %u\n", len);
            else printf("NOTE %u (octave %u, semitone %u) length %u\n", op, op / 12, op % 12, len);
            i += n;
            continue;
        }
        int operands = -1;
        const char* name = "?";
        switch (op) {
            case 0x80: name = "VOLUME";           operands = 1; break;   // v
            case 0x81: name = "DETUNE";           operands = 2; break;   // D (16-bit)
            case 0x82: name = "TONE";             operands = 1; break;   // @
            case 0x83: name = "TIE";              operands = 0; break;   // &
            case 0x87: name = "TEMPO";            operands = 1; break;   // t
            case 0x88: name = "PORTAMENTO";       operands = 1; break;   // P
            case 0x8F: name = "REGISTER_WRITE";   operands = 2; break;   // y reg,value
            case 0x90: name = "PITCH_BEND";       operands = 6; break;   // note_bend (3 x 16-bit)
            case 0x94: name = "RELEASE_FADE";     operands = 2; break;   // R step,volume
            case 0x95: name = "FADE_OUT";         operands = 1; break;   // F
            case 0x96: name = "VOLUME_UP";        operands = 1; break;   // ) or (
            case 0x97: name = "VOLUME_DOWN";      operands = 1; break;
            case 0x98: name = "PAN_1";            operands = 0; break;   // p1
            case 0x99: name = "PAN_2";            operands = 0; break;   // p2
            case 0x9A: name = "PAN_3";            operands = 0; break;   // p3
            case 0xA0: name = "PITCH_LFO";        operands = 4; break;   // MP type,period,depth,delay
            case 0xA1: name = "VOLUME_LFO";       operands = 4; break;   // MA type,period,depth,delay
            case 0xA2: name = "PITCH_LFO_WIDE";   operands = 4; break;
            case 0xA3: name = "VOLUME_LFO_WIDE";  operands = 4; break;
            case 0xA5: name = "VOLUME_ENVELOPE";  operands = 2; break;   // EA number,speed
            case 0xC0: name = "LOOP_BEGIN";       operands = 1; break;   // [ (repeat count)
            case 0xC1: name = "LOOP_BREAK";       operands = 2; break;   // / (jump distance)
            case 0xC2: name = "LOOP_END";         operands = 2; break;   // ] (jump distance)
            case 0xCF: name = "SONG_LOOP_POINT";  operands = 0; break;   // L
        }
        if (operands < 0) { printf("UNKNOWN 0x%02x\n", op); i++; continue; }
        printf("%s", name);
        for (int k = 0; k < operands; k++) printf(" %u", c[i + 1 + k]);
        printf("\n");
        i += 1 + operands;
    }
}

int main(int argc, char** argv) {
    bool list = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(kHelp, stdout); return 0; }
        if (!strcmp(a, "-l") || !strcmp(a, "--listing")) { list = true; continue; }
        if (a[0] == '-') { fprintf(stderr, "error: unknown option '%s'.  Run 'mmldump --help'.\n", a); return 2; }
        if (path) { fprintf(stderr, "error: unexpected extra argument '%s' (only one input file).\n", a); return 2; }
        path = a;
    }
    if (!path) { fprintf(stderr, "error: no input file given.\n       Usage: mmldump INPUT.mus [-l]   (see --help)\n"); return 2; }

    std::string text;
    if (!cli::readFile(path, text)) return 1;
    static Song s;
    if (!compile(text.c_str(), s)) { fprintf(stderr, "%s\n", s.describeError(path).c_str()); return 1; }

    printf("title: %s\nmachine: %s\ntotal_offset (internal, n+8): %d\n", s.titleOf(0).c_str(),
           s.machine == M_X1 ? "x1" : s.machine == M_MZ ? "mz" : "pc88", s.totalOffset);
    for (int n = 0; n < 128; n++) if (s.toneDefined[n]) {
        printf("tone %d:", n);
        for (int i = 0; i < 26; i++) printf(" %02x", s.tones[n].b[i]);
        printf("\n");
    }
    for (int n = 1; n < 64; n++) if (s.envs[n].count) {
        printf("envelope %d (loops back to entry %d):", n, s.envs[n].loop);
        for (int i = 0; i < s.envs[n].count; i++) printf(" %d", s.envs[n].v[i]);
        printf("\n");
    }
    int channels = s.x1Mode ? 11 : 6;
    for (int i = 0; i < channels; i++) {
        const Channel& c = s.ch[i];
        if (c.code.size() <= 1) continue;
        printf("channel %c: %zu bytes, %d ticks, loop point at tick %d\n", 'A' + i, c.code.size(), c.totalTicks, c.loopPoint);
        if (list) listing(c.code);
        else { printf(" "); for (size_t k = 0; k < c.code.size(); k++) printf(" %02x", c.code[k]); printf("\n"); }
    }

    // ---- how long the song is: a silent dry run of the sequencer (see songlen.hpp) ----
    SongTiming tm;
    if (measureSong(s, 0, tm)) {
        printf("timing (dry run of the sequencer, 1 ms steps):\n");
        for (int i = 0; i < 11; i++) {
            const ChannelTiming& t = tm.ch[i];
            if (!t.used) continue;
            printf("  channel %c: %s at %.3f s", 'A' + i, t.fadeEnded ? "cut by a fade-out" : t.loops ? "first pass ends" : "ends", t.endMs / 1000.0);
            if (t.loops) printf(", loop starts at %.3f s, one loop lasts %.3f s", t.loopStartMs / 1000.0, t.loopMs / 1000.0);
            printf("\n");
        }
        printf("song: played once = %.3f s", tm.onceMs / 1000.0);
        if (tm.loops) printf(" (intro %.3f s, then a loop of %.3f s that repeats for ever)", tm.introMs / 1000.0, tm.loopMs / 1000.0);
        else printf(" (does not loop; it stops by itself)");
        if (tm.fadeEnd) printf(", ends with a fade-out");
        printf("\n");
        if (!tm.uniform) printf("note: the channels do not all loop by the same length (or only some of them loop), so the parts drift against each other; see the per-channel times above.\n");
        else if (tm.loops) {
            long lo = -1, hi = -1;
            for (int i = 0; i < 11; i++) if (tm.ch[i].used) { if (lo < 0 || tm.ch[i].loopStartMs < lo) lo = tm.ch[i].loopStartMs; if (tm.ch[i].loopStartMs > hi) hi = tm.ch[i].loopStartMs; }
            if (hi - lo > 1) printf("note: the channels enter their loop at different times (%.3f to %.3f s), but every loop has the same length, so they stay together on every repeat.\n", lo / 1000.0, hi / 1000.0);
        }
        if (tm.truncated) printf("note: the measurement hit the time limit; the values above are incomplete.\n");
    }
    return 0;
}
