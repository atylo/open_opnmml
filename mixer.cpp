#include "mixer.hpp"
#include <cmath>

namespace opnmml {

static int checkedRate(int r) {
    // Below ~4 kHz a millisecond is less than one sample (the original would divide by zero); above 192 kHz nothing sensible.
    if (r < 8000 || r > 192000) throw std::invalid_argument("sample rate must be between 8000 and 192000 Hz");
    return r;
}

Mixer::Mixer(int r) : rate(checkedRate(r)), spm256((r << 8) / 1000), opx(r, 4000000) {
    opx.setMode(0);
    opx.setOutputGain(kFmOutputGain);
    for (int i = 0; i < 4; i++) seq[i] = new Sequencer(&opx);
}
Mixer::~Mixer() { for (int i = 0; i < 4; i++) delete seq[i]; }

void Mixer::setDevice(int dev) {
    switch (dev) {
    case 4: opx.setMode(0); break;
    case 5: opx.setMode(4); break;
    case 6: opx.setMode(5); break;
    default: break;
    }
}

void Mixer::setVolume(int v) {
    if (v < 0 || v > 512) throw std::invalid_argument("volume must be between 0 and 512");
    gain = (int)(std::pow(2.0, (double)v * -0.0375) * 256.0);
}

void Mixer::play(int slot, const Song* song) {
    if (slot < 0 || slot > 3 || !song) return;
    seq[slot]->load(song);
    seq[slot]->start();
}

void Mixer::step(int n) {
    acc += n << 8;
    int ticks = acc / spm256;
    acc %= spm256;
    for (int i = 0; i < ticks; i++) step1ms(seq, 4, &opx, mute);
}

void Mixer::render(int16_t* out, int frames) {
    if (frames <= 0) return;
    buf.assign((size_t)frames * 2, 0);
    int32_t* p = buf.data();
    int left = frames;
    while (left > 0) {
        int chunk = (left <= (spm256 >> 8)) ? left : (spm256 >> 8);
        step(chunk);
        opx.generate(p, chunk);
        p += 2 * chunk;
        left -= chunk;
    }
    for (int i = 0; i < frames * 2; i++) {
        int32_t v = (int32_t)(((int64_t)gain * buf[i]) >> 8);
        if (v > 32767) { v = 32767; ++clipped; } else if (v < -32768) { v = -32768; ++clipped; }
        out[i] = (int16_t)v;
    }
}

}  // namespace opnmml
