// mmltrace: compile a .mus file, run the ported sequencer for N milliseconds against a recording chip,
// and print every register write with its timestamp.
// Build:  g++ -O2 -s -o mmltrace.exe mmltrace.cpp mmlcomp.cpp seq.cpp
#include "seq.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace opnmml;

static bool readFile(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

struct TraceChip : IChip {
    int  m;
    int  now = 0;
    bool quietKeyOff = true;
    int  shadow[32] = {0};
    bool offSent[16] = {false};
    unsigned long long writes = 0, printed = 0;
    explicit TraceChip(int mode) : m(mode) {}
    int  mode() const override { return m; }
    int  read(int reg) override { return (reg >= 0 && reg < 32) ? shadow[reg] : 0; }
    void log(const char* kind, int reg, int val) {
        ++writes;
        reg &= 0xFF; val &= 0xFF;
        // idle channels get their key-off rewritten every millisecond; only show the first one
        int ch = -1; bool isOff = false;
        bool opm = (m == 1 || m == 4);
        if (kind[0] == 'K' ) { }
        if (opm && reg == 0x08 && std::string(kind) == "OPM") { ch = val & 7; isOff = (val & 0x78) == 0; if (!isOff) offSent[ch] = false; }
        if (!opm && reg == 0x28) { ch = val & 3; isOff = (val & 0xF0) == 0; if (!isOff) offSent[ch] = false; }
        if (quietKeyOff && isOff) { if (offSent[ch]) return; offSent[ch] = true; }
        if (reg < 0x10 && std::string(kind) == "OPN") {              // PSG registers: skip repeats of the same value
            static int lastv[16]; static bool init = false;
            if (!init) { for (int i = 0; i < 16; i++) lastv[i] = -1; init = true; }
            if (quietKeyOff && lastv[reg] == val) return;
            lastv[reg] = val;
        }
        ++printed;
        printf("%7d  %-3s  %02x = %02x\n", now, kind, reg, val);
    }
    void write(int reg, int val) override {
        if (m == 1 || m == 4) log("OPM", reg, val);
        else { if ((reg & 0xFF) < 32) shadow[reg & 0xFF] = val & 0xFF; log("OPN", reg, val); }
    }
    void writeOpn(int reg, int val) override {
        if ((reg & 0xFF) < 32) shadow[reg & 0xFF] = val & 0xFF;
        log("OPN", reg, val);
    }
};

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: mmltrace file.mus [milliseconds=5000] [-all]\n"); return 2; }
    int ms = (argc > 2) ? atoi(argv[2]) : 5000;
    bool all = false;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "-all")) all = true;
    std::string text;
    if (!readFile(argv[1], text)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    static Song song;
    if (!compile(text.c_str(), song)) { printf("COMPILE ERROR: %s\n", song.error.c_str()); return 1; }
    TraceChip chip(song.machine);
    chip.quietKeyOff = !all;
    Sequencer seq(&chip);
    seq.load(&song);
    seq.start();
    Sequencer* list[1] = {&seq};
    for (int t = 0; t < ms; t++) {
        chip.now = t;
        step1ms(list, 1, &chip, 0);
    }
    fprintf(stderr, "%llu writes, %llu shown, final tempo %d\n", chip.writes, chip.printed, seq.tempo);
    return 0;
}
