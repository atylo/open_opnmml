// mmldump: compile a .mus file with the wuopn compiler port and print the result.
// Build (MSYS2 MinGW64):  g++ -O2 -s -o mmldump.exe mmldump.cpp mmlcomp.cpp
#include "mmlcomp.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace opnmml;

static bool readFile(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// Compiler-level bytecode listing.  Operand counts come from the dispatcher (sub_1001DAF0)
// and from the emit sites in the compiler (sub_1001FA30).
static void listing(const std::vector<uint8_t>& c) {
    size_t i = 0;
    while (i < c.size()) {
        unsigned op = c[i];
        printf("  %04zx: ", i);
        if (op == 0) { printf("END\n"); break; }
        if (op < 0x80) {                       // note (1..126) or rest (127) + length
            unsigned len = c[i + 1];
            size_t n = 2;
            if (len & 0x80) { len = ((len & 0x7F) << 7) | c[i + 2]; n = 3; }
            if (op == 127) printf("REST %u\n", len);
            else printf("NOTE %u (oct %u key %u) len %u\n", op, op / 12, op % 12, len);
            i += n; continue;
        }
        int argc = -1; const char* name = "?";
        switch (op) {
            case 0x80: name = "VOL";      argc = 1; break;
            case 0x81: name = "DETUNE";   argc = 2; break;
            case 0x82: name = "TONE";     argc = 1; break;
            case 0x83: name = "TIE";      argc = 0; break;
            case 0x87: name = "TEMPO";    argc = 1; break;
            case 0x88: name = "PORTA(P)"; argc = 1; break;
            case 0x8F: name = "REGWRITE"; argc = 2; break;
            case 0x90: name = "BEND";     argc = 6; break;
            case 0x94: name = "R";        argc = 2; break;
            case 0x95: name = "FADE";     argc = 1; break;
            case 0x96: name = "VOL+";     argc = 1; break;
            case 0x97: name = "VOL-";     argc = 1; break;
            case 0x98: name = "PAN1";     argc = 0; break;
            case 0x99: name = "PAN2";     argc = 0; break;
            case 0x9A: name = "PAN3";     argc = 0; break;
            case 0xA0: name = "LFO_P";    argc = 4; break;
            case 0xA1: name = "LFO_A";    argc = 4; break;
            case 0xA2: name = "LFO_P_W";  argc = 4; break;
            case 0xA3: name = "LFO_A_W";  argc = 4; break;
            case 0xA5: name = "VENV";     argc = 2; break;
            case 0xC0: name = "LOOP_BEGIN"; argc = 1; break;
            case 0xC1: name = "LOOP_BREAK"; argc = 2; break;
            case 0xC2: name = "LOOP_END";   argc = 2; break;
            case 0xCF: name = "SONG_LOOP";  argc = 0; break;
        }
        if (argc < 0) { printf("UNKNOWN %02x\n", op); i++; continue; }
        printf("%s", name);
        for (int k = 0; k < argc; k++) printf(" %u", c[i + 1 + k]);
        printf("\n");
        i += 1 + argc;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: mmldump file.mus [-l]\n"); return 2; }
    bool list = (argc > 2 && std::string(argv[2]) == "-l");
    std::string text;
    if (!readFile(argv[1], text)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    Song s;
    if (!compile(text.c_str(), s)) { printf("COMPILE ERROR: %s\n", s.error.c_str()); return 1; }
    printf("title: %s\nmachine: %s\ntotal_offset(+8): %d\n", s.title.c_str(),
           s.machine == M_X1 ? "x1" : s.machine == M_MZ ? "mz" : "pc88", s.totalOffset);
    for (int n = 0; n < 128; n++) if (s.toneDefined[n]) {
        printf("tone %d:", n);
        for (int i = 0; i < 26; i++) printf(" %02x", s.tones[n].b[i]);
        printf("\n");
    }
    for (int n = 1; n < 64; n++) if (s.envs[n].count) {
        printf("env %d (loop idx %d):", n, s.envs[n].loop);
        for (int i = 0; i < s.envs[n].count; i++) printf(" %d", s.envs[n].v[i]);
        printf("\n");
    }
    int nch = s.x1Mode ? 11 : 6;
    for (int i = 0; i < nch; i++) {
        const Channel& c = s.ch[i];
        if (c.code.size() <= 1) continue;
        printf("channel %c: %zu bytes, %d ticks, loop point %d\n", 'A' + i, c.code.size(), c.totalTicks, c.loopPoint);
        if (list) listing(c.code);
        else { printf(" "); for (size_t k = 0; k < c.code.size(); k++) printf(" %02x", c.code[k]); printf("\n"); }
    }
    return 0;
}
