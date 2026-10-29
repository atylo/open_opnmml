// compdump - compile an MML file with the port and dump the result in the same shape engine.py dumps the DLL's:
// one line per item, hex.  Used by compile_diff.py.   Build: g++ -O2 -I.. -o compdump compdump.cpp ../mmlcomp.cpp
#include "mmlcomp.hpp"
#include <cstdio>
#include <string>
using namespace opnmml;
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    std::string text; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
    static Song s;
    if (!compile(text.c_str(), s)) { printf("ERROR\n%s\n%d\n", s.error.c_str(), s.errorLine); return 0; }
    printf("OK\n");
    printf("x1 %d\ntotal %d\n", s.x1Mode ? 1 : 0, s.totalOffset);
    for (int c = 0; c < 11; c++) { printf("code %d ", c); for (uint8_t v : s.ch[c].code) printf("%02x", v); printf("\n"); }
    printf("tones "); for (int i = 0; i < 128; i++) for (int k = 0; k < 26; k++) printf("%02x", s.tones[i].b[k]); printf("\n");
    printf("envs ");  // 64 x (count word, loop word, 64 words) = 132 bytes each, little endian
    for (int i = 0; i < 64; i++) {
        uint16_t cnt = (uint16_t)s.envs[i].count, loop = (uint16_t)s.envs[i].loop;
        printf("%02x%02x%02x%02x", cnt & 255, cnt >> 8, loop & 255, loop >> 8);
        for (int k = 0; k < 64; k++) { uint16_t v = (uint16_t)s.envs[i].v[k]; printf("%02x%02x", v & 255, v >> 8); }
    }
    printf("\n");
    return 0;
}
