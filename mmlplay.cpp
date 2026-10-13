// mmlplay: render a .mus file to a 16-bit stereo WAV with the ported wuopn engine.
// Build (MSYS2 MinGW64):  g++ -O2 -s -o mmlplay.exe mmlplay.cpp mixer.cpp opx4.cpp seq.cpp mmlcomp.cpp
#include "mixer.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace opnmml;

static bool readFile(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return true;
}
static void put32(FILE* f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void put16(FILE* f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mmlplay in.mus out.wav [seconds=60] [-d PC88|X1|MZ] [-v volume=32] [-m mutemask=0] [-r rate=44100] [-s noiseSeed]\n");
        return 2;
    }
    double seconds = 60; int rate = 44100, vol = 32; unsigned mute = 0; int dev = -1; bool seed = false; uint32_t sd = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { std::string s = argv[++i]; dev = (s == "X1") ? 5 : (s == "MZ") ? 6 : 4; }
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) vol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mute = (unsigned)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) { seed = true; sd = (uint32_t)strtoul(argv[++i], nullptr, 0); }
        else seconds = atof(argv[i]);
    }
    std::string text;
    if (!readFile(argv[1], text)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    static Song song;
    if (!compile(text.c_str(), song)) { fprintf(stderr, "COMPILE ERROR: %s\n", song.error.c_str()); return 1; }
    if (seed) Opx4::setNoiseSeed(sd);
    Mixer mx(rate);
    if (dev < 0) dev = (song.machine == M_X1) ? 5 : (song.machine == M_MZ) ? 6 : 4;
    mx.setDevice(dev);
    mx.setVolume(vol);
    mx.setMute(mute);
    mx.play(3, &song);
    long total = (long)(seconds * rate);
    std::vector<int16_t> pcm((size_t)total * 2);
    // render in blocks like a streaming decoder would
    long done = 0;
    while (done < total) {
        int n = (int)((total - done) < 1024 ? (total - done) : 1024);
        mx.render(pcm.data() + done * 2, n);
        done += n;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    uint32_t bytes = (uint32_t)(total * 4);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, rate); put32(f, rate * 4); put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
    fwrite(pcm.data(), 1, bytes, f);
    fclose(f);
    fprintf(stderr, "wrote %s: %.1f s, %d Hz, device %d, volume %d\n", argv[2], seconds, rate, dev, vol);
    return 0;
}
