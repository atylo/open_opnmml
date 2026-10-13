// ================================================================================================================
// mmltrace - show the chip register writes a .mus file produces, with timestamps.
//
// The sequencer turns MML into a stream of "write value V to chip register R" commands, exactly like a real sound
// driver.  This tool runs the sequencer against a recording chip (no audio is generated) and prints that stream.
// It is useful for checking timing and for comparing against another implementation.
//
// Build:  g++ -O2 -s -o mmltrace.exe mmltrace.cpp seq.cpp mmlcomp.cpp
// ================================================================================================================
#include "cli.hpp"
#include "seq.hpp"
#include <cstdio>
#include <cstring>
#include <string>
using namespace opnmml;

static const char* kHelp =
"mmltrace - print the chip register writes produced by a .mus file\n"
"\n"
"USAGE\n"
"  mmltrace INPUT.mus [options]\n"
"\n"
"OPTIONS (all optional; give the option letter, a space, then only the value:  -t 3000)\n"
"  -t, --time MS    how many milliseconds of the song to simulate, 1 to 3600000.   Default: 5000\n"
"  -a, --all        print every write.  Without it, writes that repeat the previous value of the same\n"
"                   register (idle channels are silenced again every millisecond) are hidden.\n"
"  -h, --help       show this text\n"
"\n"
"OUTPUT: one line per write:   <millisecond>  <OPM|OPN>  <register> = <value>   (both in hexadecimal)\n"
"        OPM lines are the X1's YM2151 register map, OPN lines the PC-88's YM2203 map (also used for PSG registers).\n"
"EXIT STATUS: 0 = success, 1 = file or MML error, 2 = bad command line\n";

// Chip that only records writes.  It also keeps the OPN/PSG register shadow, because the sequencer reads PSG register 7.
struct TraceChip : IChip {
    int  chipMode;
    int  now = 0;               // current simulated millisecond
    bool hideRepeats = true;
    int  lastOpn[256];          // last value written to each OPN-map register (-1 = none)
    int  shadow[32] = {0};
    bool keyOffShown[8] = {false};
    unsigned long long total = 0, shown = 0;

    explicit TraceChip(int m) : chipMode(m) { for (int& v : lastOpn) v = -1; }
    int  mode() const override { return chipMode; }
    int  read(int reg) override { return (reg >= 0 && reg < 32) ? shadow[reg] : 0; }

    void write(int reg, int val) override { (chipMode == 1 || chipMode == 4) ? emitOpm(reg & 255, val & 255) : emitOpn(reg & 255, val & 255); }
    void writeOpn(int reg, int val) override { emitOpn(reg & 255, val & 255); }

    // OPN/PSG map.  Key-off of an FM channel is register 0x28 with no operator bits; repeated writes are suppressed.
    void emitOpn(int reg, int val) {
        ++total;
        if (reg < 32) shadow[reg] = val;
        if (hideRepeats && lastOpn[reg] == val && (reg < 0x10 || reg == 0x28)) return;
        lastOpn[reg] = (reg == 0x28) ? -1 : val;        // register 0x28 carries a channel number: do not collapse across channels
        if (hideRepeats && reg == 0x28 && (val & 0xF0) == 0) {          // key off: show once per channel until the next key on
            int ch = val & 3;
            if (keyOffShown[ch]) return;
            keyOffShown[ch] = true;
        } else if (reg == 0x28) {
            keyOffShown[val & 3] = false;
        }
        print("OPN", reg, val);
    }
    // OPM map.  Key on/off is register 0x08: low 3 bits channel, bits 3-6 operators.
    void emitOpm(int reg, int val) {
        ++total;
        if (hideRepeats && reg == 0x08) {
            int ch = val & 7;
            if ((val & 0x78) == 0) { if (keyOffShown[ch]) return; keyOffShown[ch] = true; }
            else keyOffShown[ch] = false;
        }
        print("OPM", reg, val);
    }
    void print(const char* kind, int reg, int val) { ++shown; printf("%7d  %s  %02x = %02x\n", now, kind, reg, val); }
};

int main(int argc, char** argv) {
    long ms = 5000;
    bool all = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* val = nullptr;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(kHelp, stdout); return 0; }
        if (!strcmp(a, "-a") || !strcmp(a, "--all")) { all = true; continue; }
        if (a[0] != '-') {
            if (path) { fprintf(stderr, "error: unexpected extra argument '%s' (only one input file; for the length use -t MS).\n", a); return 2; }
            path = a; continue;
        }
        if (cli::matchOption(argc, argv, i, "-t", "--time", val)) {
            if (!val) { fprintf(stderr, "error: option -t needs a value, e.g. -t 3000\n"); return 2; }
            if (!cli::parseInt("-t", val, 1, 3600000, ms, "milliseconds", "3000")) return 2;
        } else {
            fprintf(stderr, "error: unknown option '%s'.  Run 'mmltrace --help'.\n", a);
            return 2;
        }
    }
    if (!path) { fprintf(stderr, "error: no input file given.\n       Usage: mmltrace INPUT.mus [-t MS] [--all]   (see --help)\n"); return 2; }

    std::string text;
    if (!cli::readFile(path, text)) return 1;
    static Song song;
    if (!compile(text.c_str(), song)) { fprintf(stderr, "error: %s: MML error: %s\n", path, song.error.c_str()); return 1; }

    TraceChip chip(song.machine);
    chip.hideRepeats = !all;
    Sequencer seq(&chip);
    seq.load(&song);
    seq.start();
    Sequencer* slots[1] = {&seq};
    for (long t = 0; t < ms; t++) {
        chip.now = (int)t;
        step1ms(slots, 1, &chip, 0);
    }
    fprintf(stderr, "%llu register writes in %ld ms (%llu shown), final tempo %d\n", chip.total, ms, chip.shown, seq.tempo);
    return 0;
}
