// ================================================================================================================
// Mixer - port of the original plugin's CSeqMixer.  This is the top of the engine: it owns the chip and four sequencer slots
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
#include "seq.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace opnmml {

// FM output gain.  The chip's own constructor sets its output gain cell to 1024, but the mixer constructor then calls
// a one-line chip method ("this[+0x10] = 2048", sub_100195B0) before selecting the chip type.  The decompiler mislabelled that
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
        DEVICE_MZ   = 6    // Sharp MZ-2000: PWM beeper-style output (PSG only: FM is silent, as in the original) -> chip mode 5
    };
    // Select the emulated machine, exactly like OPNSetDevice: first EVERY slot is stopped and wiped (all loaded songs,
    // tones and envelopes are forgotten, sub_10022DF0), then the chip is switched and reset.  Returns false for a number
    // that is not 4, 5 or 6 (the slots are still wiped and the chip is left alone, as in the original).      [sub_10022D80]
    bool setDevice(int dev);
    // Stop and wipe all four slots without touching the chip.                                                [sub_10022DF0]
    void resetAll();
    // Master volume.  gain = 2^(v * -0.0375) * 256, so 0 = unity gain, every +27 halves the level.  The game uses 32.
    // Accepted range 0..512 (throws std::invalid_argument otherwise).                              [sub_10022B70]
    void setVolume(int v);
    // Mute channels: bit n set silences channel n (0 = channel A ... 10 = channel K).  The game uses 0x7 on
    // PSG-only machines, which mutes the three FM channels.                                        [sub_10022BC0]
    void setMute(unsigned mask) { mute = mask; }
    // ---- songs: four slots, each holding up to 64 numbered songs that share tones and envelopes ----------------------
    // Compile `text` as song `num` of `slot` (OPNPlaySE).  Slot 0..3 (the game plays background music in slot 3); num is
    // clamped to 0..63 like the original.  The slot is stopped first and the song is APPENDED to what the slot already
    // holds, so it may use (and overwrite) tones/envelopes defined by earlier songs of the same slot.  With play=true the
    // song starts at once.  Returns false on an error (message in lastError(), the slot is left unchanged) or a bad
    // slot number.                                                                                        [sub_10023060]
    bool loadSong(int slot, int num, const char* text, bool play = false);
    const std::string& lastError() const { return lastErr; }
    // Start song `num` of `slot`.  A number that was never loaded plays the first song of the slot (see Sequencer::start);
    // use songLoaded() to tell.                                                                            [sub_10022D30]
    void playSong(int slot, int num);
    // OPNStopSE: begin the quick fade-out (about 0.26 s) of a slot.                                         [sub_10022CE0]
    void stopSlot(int slot);
    // Cut a slot at once, with NO fade: its channels stop, and the mixer keys off the chip voices they were using, so FM notes die away through
    // their own release (a PSG note stops instantly).  This is the original's stop+reset (sub_10022C80), which it uses internally before
    // loading a song.  OPNStopSE (stopSlot) is the version with the quick fade-out.
    void cutSlot(int slot);
    // Title of a loaded song ("" if none or the slot is invalid).                                          [sub_10022AF0]
    const std::string& songTitle(int slot, int num) const;
    bool songLoaded(int slot, int num) const;        // true when song `num` of `slot` has been compiled since the last wipe
    // OPNTimer: tempo (beats per minute) of a slot, -1 for a bad slot.  The tempo is 120 when a song starts and changes with
    // the song's own t commands.                                                                           [sub_10022A50]
    int  tempo(int slot) const;
    // Change the tempo of a slot.  bpm <= 0 is ignored (the original divides by zero for 0).  Returns false then.  [sub_10022A90]
    bool setTempo(int slot, int bpm);
    // Read-only access to a slot's songs (tones, envelopes, code) for tools.
    const Song& slotSong(int slot) const { return slots[slot < 0 ? 0 : slot > 3 ? 3 : slot]; }
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
    void stopAndReset(int slot);                     // sub_10022C80: cut the slot at once and rebind its channel data
    std::string lastErr;
    Opx4 opx;
    Song slots[4];                                   // the songs of the four slots (CSoundSequencer's song storage)
    Sequencer* seq[4];
    std::vector<int32_t> buf;
};

}  // namespace opnmml
