// Port of wuopn.dll's CSeqMixer: owns the chip and 4 sequencer slots, renders 16-bit stereo.
#pragma once
#include "opx4.hpp"
#include <cstdint>
#include <vector>

namespace opnmml {

class Mixer {
public:
    explicit Mixer(int rate = 44100);                // sub_10023150 (chip clock is fixed at 4 MHz)
    ~Mixer();
    void setDevice(int dev);                         // sub_10022D80: 4 = PC88 (mode 0), 5 = X1 (mode 4), 6 = MZ (mode 5)
    void setVolume(int v);                           // sub_10022B70: gain = pow(2, v * -0.0375) * 256   (the game uses 32)
    void setMute(unsigned mask) { mute = mask; }     // sub_10022BC0
    void play(int slot, const Song* song);           // start song 0 of `song` in sequencer slot (the game's BGM slot is 3)
    void render(int16_t* out, int frames);           // sub_100233F0 / sub_100232E0
    Opx4& chip() { return opx; }
    Sequencer* sequencer(int slot) { return seq[slot]; }
private:
    void step(int n);                                // sub_10022E80
    int rate, spm256;                                // spm256 = (rate << 8) / 1000
    int acc = 0;                                     // this+16
    int gain = 256;                                  // this+12
    unsigned mute = 0;                               // this+64
    Opx4 opx;
    Sequencer* seq[4];
    std::vector<int32_t> buf;
};

}  // namespace opnmml
