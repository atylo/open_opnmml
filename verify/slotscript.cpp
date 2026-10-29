// slotscript - the port's Mixer driven by text commands on stdin (one command per line), for verify/slot_diff.py.
// Every command prints one line (or, for "state", a block ending with "end") on stdout.
//   dev N | vol N | mute M | load SLOT NUM PATH PLAY | playn SLOT NUM | stop SLOT | cut SLOT | tempo SLOT | settempo SLOT N
//   title SLOT NUM | render FRAMES BLOCK | state
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <fstream>
#include <memory>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#define private public            // test harness only: look inside the sequencer
#include "mixer.hpp"
#undef private
using namespace opnmml;

static void hexs(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", p[i]); }

static void dumpState(Mixer& mx) {
    for (int s = 0; s < 4; s++) {
        const Song& S = mx.slotSong(s);
        for (int c = 0; c < 11; c++) { printf("code %d %d ", s, c); hexs(S.ch[c].code.data(), S.ch[c].code.size()); printf("\n"); }
        printf("tones %d ", s); for (int i = 0; i < 128; i++) hexs(S.tones[i].b, 26); printf("\n");
        printf("envs %d ", s);
        for (int i = 0; i < 64; i++) {
            const Envelope& e = S.envs[i]; uint8_t b[132];
            b[0] = e.count & 0xFF; b[1] = (e.count >> 8) & 0xFF; b[2] = e.loop & 0xFF; b[3] = (e.loop >> 8) & 0xFF;
            for (int k = 0; k < 64; k++) { b[4 + 2 * k] = e.v[k] & 0xFF; b[5 + 2 * k] = (e.v[k] >> 8) & 0xFF; }
            hexs(b, 132);
        }
        printf("\n");
        printf("starts %d ", s);
        for (int n = 0; n < 64; n++) for (int c = 0; c < 11; c++) { uint32_t v = (uint32_t)S.songs[n].start[c]; uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; hexs(b, 4); }
        printf("\n");
        const Sequencer* q = mx.sequencer(s);
        printf("rt %d %d %d %d %d %d %d %d %d\n", s, q->tempo, q->counter, q->timerAcc, q->timerStep, q->fadeAcc, q->fadeStep, q->masterAtten, (int)q->lastMask);
        for (int c = 0; c < 11; c++) {
            const SeqCh& h = q->ch[c];
            printf("rtch %d %d %d %d %d %d %d %d %d %d\n", s, c, h.base != nullptr ? 1 : 0, (int)h.state, (int)h.len, (int)h.off, (int)h.loopOff, h.owns ? 1 : 0, (int)h.tone, (int)h.vol);
        }
    }
    printf("end\n");
}

int main() {
    Mixer mx(44100);
    mx.setVolume(32);
    mx.setDevice(4);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 20);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd; in >> cmd;
        if (cmd == "dev")      { int d; in >> d; mx.setDevice(d); printf("ok\n"); }
        else if (cmd == "vol") { int v; in >> v; mx.setVolume(v); printf("ok\n"); }
        else if (cmd == "mute"){ unsigned m; in >> m; mx.setMute(m); printf("ok\n"); }
        else if (cmd == "load") {
            int slot, num, play; std::string path; in >> slot >> num >> path >> play;
            std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
            bool ok = mx.loadSong(slot, num, ss.str().c_str(), play != 0);
            if (ok) printf("ok\n"); else printf("err %d %s\n", mx.slotSong(slot).errorLine, mx.lastError().c_str());
        }
        else if (cmd == "playn")   { int s, n; in >> s >> n; mx.playSong(s, n); printf("ok\n"); }
        else if (cmd == "stop")    { int s; in >> s; mx.stopSlot(s); printf("ok\n"); }
        else if (cmd == "cut")     { int s; in >> s; if (s >= 0 && s <= 3) mx.stopAndReset(s); printf("ok\n"); }   // sub_10022C80
        else if (cmd == "tempo")   { int s; in >> s; printf("tempo %d\n", mx.tempo(s)); }
        else if (cmd == "settempo"){ int s, b; in >> s >> b; mx.setTempo(s, b); printf("ok\n"); }
        else if (cmd == "title")   { int s, n; in >> s >> n; const std::string& t = mx.songTitle(s, n); printf("title "); hexs((const uint8_t*)t.data(), t.size()); printf("\n"); }
        else if (cmd == "render") {
            long frames; int block; in >> frames >> block;
            std::vector<int16_t> b((size_t)block * 2);
            printf("pcm ");
            for (long done = 0; done < frames; ) {
                int k = (int)((frames - done) < block ? (frames - done) : block);
                mx.render(b.data(), k);
                hexs((const uint8_t*)b.data(), (size_t)k * 4);
                done += k;
            }
            printf("\n");
        }
        else if (cmd == "state") dumpState(mx);
        else if (cmd == "quit") break;
        else printf("bad %s\n", cmd.c_str());
        fflush(stdout);
    }
    return 0;
}
