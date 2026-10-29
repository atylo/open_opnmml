// mixerscript - render a song with the ported engine exactly like mixer_diff.py drives the original DLL:
//   create mixer (44100 Hz), volume V, device D, compile into slot 3 / song 0, play, then render FRAMES in blocks.
// Output: raw int16 stereo samples.   Build: g++ -O2 -I.. -o mixerscript mixerscript.cpp ../mixer.cpp ../opx4.cpp ../seq.cpp ../mmlcomp.cpp
#include "mixer.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace opnmml;

int main(int argc, char** argv) {
    if (argc < 8) { fprintf(stderr, "usage: mixerscript song.mus out.bin DEVICE(4|5|6) VOLUME MUTE FRAMES BLOCK\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::string text; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
    fclose(f);
    Mixer mx(44100);
    mx.setVolume(atoi(argv[4]));
    mx.setDevice(atoi(argv[3]));
    mx.setMute((unsigned)strtoul(argv[5], nullptr, 0));
    // new slot API: compile as song 0 of slot 3 (after setDevice, which wipes every slot), then play it
    if (!mx.loadSong(3, 0, text.c_str(), false)) { fprintf(stderr, "compile error: %s\n", mx.lastError().c_str()); return 1; }
    mx.playSong(3, 0);
    long frames = atol(argv[6]); int block = atoi(argv[7]);
    FILE* out = fopen(argv[2], "wb");
    std::vector<int16_t> buf((size_t)block * 2);
    for (long done = 0; done < frames; ) {
        int k = (int)((frames - done) < block ? (frames - done) : block);
        mx.render(buf.data(), k);
        fwrite(buf.data(), 4, (size_t)k, out);
        done += k;
    }
    fclose(out);
    return 0;
}
