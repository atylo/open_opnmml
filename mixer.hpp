// ================================================================================================================
// Mixer - port of wuopn.dll's CSeqMixer.  This is the top of the engine: it owns the chip and four sequencer slots
// (the game plays background music in slot 3 and sound effects in the others), and renders 16-bit stereo audio.
//
//   MML text --compile()--> Song --Mixer::play()--> Sequencer (11 channels, steps once per millisecond)
//        --register writes--> Opx4 chip --generate()--> 32-bit samples --gain, clip--> 16-bit samples
//
// Timing: audio is produced in chunks of at most one millisecond; before each chunk the sequencer is stepped for every
// millisecond boundary that has passed, so music timing is accurate to about one chunk (44 samples at 44.1 kHz).
// ================================================================================================================
#pragma once
#include "opx4.hpp"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace opnmml {

// EMPIRICAL CALIBRATION (not in the decompile): wuopn.cpp initialises the chip's output gain D(4) to 1024, but recordings
// of the original plugin (PC-88 and X1 files, 25-28 s each) fit an FM level 1.93-1.98x higher with PSG unchanged (R^2 ~ 0.98).
// A final FM gain of 2048 reproduces them (rms ratio 1.004, spread 0.011).  Set to 1024 for the value as decompiled.
static const int kFmOutputGain = 2048;

class Mixer {
public:
    // rate: output sample rate in Hz, 8000..192000 (the game uses 44100).  Throws std::invalid_argument otherwise.
    // The chip clock is fixed at 4 MHz.                                                                [sub_10023150]
    explicit Mixer(int rate = 44100);
    ~Mixer();
    // Machine whose sound hardware is emulated.  The numbers are the values the game's OPNSetDevice("PC88"/"X1"/"MZ")
    // passes to the plugin.  Selecting a device resets the chip.                                  [sub_10022D80]
    enum Device {
        DEVICE_PC88 = 4,   // NEC PC-8801:   YM2203 (OPN) 3 FM channels + 3 PSG        -> chip mode 0
        DEVICE_X1   = 5,   // Sharp X1turbo: YM2151 (OPM) 8 FM channels + 3 PSG        -> chip mode 4
        DEVICE_MZ   = 6    // Sharp MZ-2000: PWM beeper-style output                   -> chip mode 5
    };
    void setDevice(int dev);
    // Master volume.  gain = 2^(v * -0.0375) * 256, so 0 = unity gain, every +27 halves the level.  The game uses 32.
    // Accepted range 0..512 (throws std::invalid_argument otherwise).                              [sub_10022B70]
    void setVolume(int v);
    // Mute channels: bit n set silences channel n (0 = channel A ... 10 = channel K).  The game uses 0x7 on
    // PSG-only machines, which mutes the three FM channels.                                        [sub_10022BC0]
    void setMute(unsigned mask) { mute = mask; }
    // Start `song` in sequencer slot 0..3 (the game plays background music in slot 3).  `song` must outlive playback.
    void play(int slot, const Song* song);
    // Render `frames` stereo frames (interleaved L,R) to 16-bit.  Output beyond the 16-bit range is clipped.
    void render(int16_t* out, int frames);           // [sub_100233F0 / sub_100232E0]
    long clippedSamples() const { return clipped; }  // how many output samples have been clipped so far
    Opx4& chip() { return opx; }
    Sequencer* sequencer(int slot) { return seq[slot]; }
private:
    void step(int n);                                // sub_10022E80
    int rate, spm256;                                // spm256 = (rate << 8) / 1000 = samples per millisecond * 256
    long clipped = 0;
    int acc = 0;                                     // this+16
    int gain = 256;                                  // this+12
    unsigned mute = 0;                               // this+64
    Opx4 opx;
    Sequencer* seq[4];
    std::vector<int32_t> buf;
};

}  // namespace opnmml
