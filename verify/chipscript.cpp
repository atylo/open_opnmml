// chipscript - drive the ported chip (Opx4) from a text script and dump the raw 32-bit samples.
// Used by chip_diff.py to compare the port with the original DLL's chip code.
//
// Script commands (one per line):
//   new RATE CLOCK     create the chip            gain G        set the chip output gain cell
//   mode M             select chip type (resets)  w REG VAL     register write (chip-type dependent map)
//   wo REG VAL         OPN/PSG-map write          gen N         render N frames, append raw int32 L,R pairs to the output
//   dump               append the chip's whole state image (1400 int32) to the output
// Build:  g++ -O2 -I.. -o chipscript chipscript.cpp ../opx4.cpp
#include "opx4.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace opnmml;

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: chipscript script.txt output.bin\n"); return 2; }
    FILE* in = fopen(argv[1], "r");
    FILE* out = fopen(argv[2], "wb");
    if (!in || !out) { fprintf(stderr, "cannot open files\n"); return 1; }
    std::unique_ptr<Opx4> chip;
    char cmd[16];
    while (fscanf(in, "%15s", cmd) == 1) {
        int a = 0, b = 0;
        if (!strcmp(cmd, "new"))       { fscanf(in, "%d %d", &a, &b); chip.reset(new Opx4(a, b)); }
        else if (!strcmp(cmd, "mode")) { fscanf(in, "%d", &a); chip->setMode(a); }
        else if (!strcmp(cmd, "gain")) { fscanf(in, "%d", &a); chip->setOutputGain(a); }
        else if (!strcmp(cmd, "w"))    { fscanf(in, "%d %d", &a, &b); chip->write(a, b); }
        else if (!strcmp(cmd, "wo"))   { fscanf(in, "%d %d", &a, &b); chip->writeOpn(a, b); }
        else if (!strcmp(cmd, "dump")) { fwrite(chip->stateImage().data(), 4, chip->stateImage().size(), out); }
        else if (!strcmp(cmd, "gen"))  {
            fscanf(in, "%d", &a);
            std::vector<int32_t> buf((size_t)a * 2, 0);
            chip->generate(buf.data(), a);
            fwrite(buf.data(), 4, buf.size(), out);
        } else { fprintf(stderr, "unknown command %s\n", cmd); return 1; }
    }
    fclose(out);
    return 0;
}
