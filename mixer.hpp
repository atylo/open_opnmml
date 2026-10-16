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

// FM output gain.  The chip's own constructor sets its output gain cell to 1024, but the mixer constructor then calls
// a one-line chip method ("this[+0x10] = 2048", sub_100195B0) before selecting the chip type.  Hex-Rays mislabelled that
// method as an anonymous library function and dropped its object, so the call is invisible in the decompile; it was
// found by disassembling the DLL.  Without it the FM comes out half as loud as the original.
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
    // Sub-millisecond phase of the sequencer timer (0 .. samples-per-millisecond * 256 - 1).  The original's audio thread is
    // already running when a song starts, so its phase at that moment is arbitrary; this lets a recording be matched exactly.
    void setTimerPhase(int phase) { acc = phase; }
    int  timerPhaseRange() const { return spm256; }

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
