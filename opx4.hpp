// ================================================================================================================
// Opx4 - port of the sound chip emulator ("COpx4") inside wuopn.dll.
//
// What it emulates
//   * an FM synthesiser with 4 operators per channel: 8 channels / 32 operators (OPM, Sharp X1 - YM2151 style) or
//     3 channels / 12 operators (OPN, PC-88 - YM2203 style), selected by the chip mode;
//   * a 3-voice PSG (square waves + noise + hardware envelope, YM2149/AY style) mixed in alongside the FM.
//
// How it is driven
//   The sequencer (seq.cpp) never touches the chip's internals; it only writes registers, exactly like a real sound
//   driver would, via write()/writeOpn().  generate() then renders audio into a 32-bit stereo buffer.
//
// Fidelity note - the flat state image
//   The original keeps ALL its state in one flat struct addressed as `this + N` dwords.  This port keeps exactly that
//   layout (a flat int32 image with the same dword numbers as the Hex-Rays output) so every function can be compared
//   line by line against the decompile.  Dword numbers are given names below (G_*, OP_*, CH_*).  Pointer fields of the
//   original are stored as dword INDICES into the same image (so the code also works on a 64-bit host); G_DUMMY is
//   an always-zero cell the original calls unk_1003DA74 and uses as "no modulation input / no output".
//
//   Operator i (0..31) occupies dwords  OP_BASE + OP_STRIDE*i + OP_*      (116 bytes each in the original)
//   Channel  j (0..7)  occupies dwords  CH_BASE + CH_STRIDE*j + CH_*      ( 72 bytes each in the original)
//   Operator i belongs to channel i / 4; its slot within the channel is i % 4.
// ================================================================================================================
#pragma once
#include "seq.hpp"
#include <cstdint>
#include <vector>

namespace opnmml {

class Opx4 : public IChip {
public:
    // Chip types accepted by setMode() (the value sub_1001BB20 receives).  The sequencer/mixer only ever use
    // MODE_OPN (PC-88), MODE_OPM_X1 (Sharp X1) and MODE_OPN_MZ (MZ-2000, PSG output inverted = "PWM" sound).
    enum Mode {
        MODE_OPN         = 0,   // YM2203-like: 3 FM channels + PSG, 55.5 kHz internal rate
        MODE_OPM         = 1,   // YM2151-like: 8 FM channels, no PSG, 62.5 kHz internal rate; LFO uses a lookup table
        MODE_OPN_MASK_2  = 2,   // OPN variant with operator mask 0xFF0FF0 (not reachable from the game)
        MODE_OPN_24OP    = 3,   // OPN variant with 24 operators (not reachable from the game)
        MODE_OPM_X1      = 4,   // Sharp X1: OPM FM + PSG (the PSG is driven through the OPN register path)
        MODE_OPN_MZ      = 5    // MZ-2000: like MODE_OPN but the PSG output is combined differently
    };

    // outRate : sample rate of the audio produced by generate() (44100 in the game)
    // clock   : master clock in Hz (the plugin always uses 4,000,000)                      [sub_1001BC80]
    Opx4(int outRate, int clock);

    // Select the chip type and reset every register/operator.                              [sub_1001BB20]
    void setMode(int mode);

    int  mode() const override { return d[G_MODE]; }                                    // [sub_1001A470]

    // Register write, the way the sequencer does it ("port 0" only).
    //   write()    : chip-type dependent - OPM register map for MODE_OPM / MODE_OPM_X1, OPN map otherwise [sub_1001B7A0]
    //   writeOpn() : always the OPN/PSG register map (the X1 sequencer uses it for its PSG voices)         [sub_1001B220]
    void write(int reg, int val) override;
    void writeOpn(int reg, int val) override;
    // Read back a register of the OPN/PSG file (0x00-0x1F); the sequencer needs PSG register 7 (mixer).  [sub_10019DC0]
    int  read(int reg) override;

    // Render `n` stereo frames and ADD them (interleaved L,R as 32-bit ints) to buf.        [sub_1001A6E0]
    // The result is not yet clipped or scaled to 16 bits; Mixer does that.
    void generate(int32_t* buf, int n);

    // Seed of the random table used for the noise generators and the "random" LFO waveform.
    // The DLL fills it with the C runtime's rand(); with the default seed 1 this port reproduces MSVC's sequence.
    static void setNoiseSeed(uint32_t seed);

    // Overall FM output gain, 1024 = x1.  The decompiled DLL uses 1024; see Mixer for the calibration actually applied.
    void setOutputGain(int g) { d[G_OUTPUT_GAIN] = g; }

private:
    // ---------------------------------------------------------------------------------------------------------
    // Layout of the state image (dword numbers, identical to the Hex-Rays `this + N` expressions)
    // ---------------------------------------------------------------------------------------------------------
    enum Global {
        G_MODE            = 1,    // current chip type (enum Mode)
        G_OPERATOR_MASK   = 2,    // bit i set = operator i is generated (OPN: 0xFFF = 12 operators, OPM: all 32)
        G_CLOCK           = 3,    // master clock in Hz
        G_OUTPUT_GAIN     = 4,    // FM output gain, 1024 = x1
        G_CHIP_RATE       = 5,    // internal sample rate: clock/72 (OPN) or clock/64 (OPM)
        G_OUTPUT_RATE     = 6,    // output sample rate
        G_RESAMPLE_ACC    = 7,    // 14-bit fixed point accumulator converting internal samples to output samples
        G_RESAMPLE_STEP   = 8,    //   added to the accumulator per internal sample = (outRate << 14) / chipRate
        G_PHASE_SCALE     = 9,    // (clock << 9) / outRate << 12 : converts a PSG period / LFO rate to a phase step
        G_CH3_MODE        = 139,  // OPN register 0x27 bits 6-7 (channel-3 special mode); see setCh3Mode()
        G_OPM_NOISE_STEP  = 140,  // OPM noise generator phase step (0 = noise off)
        G_OPM_NOISE_PHASE = 141,  // OPM noise generator phase
        G_AM_LFO          = 142,  // current tremolo (AM) LFO value, updated every 8 samples
        G_PM_LFO          = 143,  // current vibrato (PM) LFO value, updated every 8 samples
        G_UNUSED_144      = 144,  // cleared in reset, never read
        OP_BASE           = 147,  // first dword of operator 0
        CH_BASE           = 1075, // first dword of channel 0
        G_SAMPLE_COUNT    = 1219, // counts internal samples; the LFO is updated whenever (count & 7) == 0
        G_LFO_PHASE       = 1220, // LFO phase accumulator
        G_LFO_STEP        = 1221, // LFO phase step per update (0 = LFO off)
        G_LFO_PMD         = 1222, // LFO vibrato depth (0..127)
        G_LFO_AMD         = 1223, // LFO tremolo depth (0..127)
        G_LFO_WAVE        = 1224, // LFO waveform 0 saw, 1 square, 2 triangle, 3 noise
        // PSG ---------------------------------------------------------------------------------------------
        G_PSG_TONE_PHASE  = 1225, // 3 dwords: phase counter of tone generators A, B, C
        G_PSG_NOISE_PHASE = 1228, // phase counter of the noise generator
        G_PSG_ENV_PHASE   = 1229, // phase counter of the hardware envelope (top bits are the envelope position)
        G_PSG_TONE_STEP   = 1230, // 3 dwords: phase step of tone generators A, B, C (from PSG registers 0-5)
        G_PSG_NOISE_STEP  = 1233, // phase step of the noise generator (register 6)
        G_PSG_ENV_STEP    = 1234, // phase step of the envelope (registers 0x0B/0x0C)
        G_PSG_LFSR        = 1235, // noise shift register
        G_PSG_OUT_BITS    = 1236, // current square-wave level of tones A-C (bits 0-2) and noise (bits 3-5)
        G_PSG_ENV_SHAPE   = 1237, // envelope shape (register 0x0D, bits 0-3)
        G_PSG_CH_AMP      = 1238, // 3 dwords: fixed amplitude of channels A-C (registers 8-10)
        G_PSG_ENV_AMP     = 1241, // current envelope amplitude
        G_PSG_VOL_PTR     = 1242, // 3 dwords: dword index of the amplitude cell each channel uses (fixed or envelope)
        G_LEFTOVER_L      = 1251, // last output sample still owed to the next generate() call (left)
        G_LEFTOVER_R      = 1252, //   ... (right)
        G_LEFTOVER_COUNT  = 1253, // how many times it is owed
        G_DUMMY           = 1300  // always-zero cell (unk_1003DA74)
    };

    // Per-operator fields (added to OP_BASE + OP_STRIDE * operator number).
    enum OperatorField {
        OP_STRIDE         = 29,
        OP_ATTACK_RATE    = 0,    // 2 * AR                                  (register 0x50+ bits 0-4)
        OP_DECAY1_RATE    = 1,    // 2 * D1R                                 (register 0x60+ bits 0-4)
        OP_DECAY2_RATE    = 2,    // 2 * D2R                                 (register 0x70+ bits 0-4)
        OP_RELEASE_RATE   = 3,    // 4 * RR + 2                              (register 0x80+ bits 0-3)
        OP_SUSTAIN_LEVEL  = 4,    // D1L << 17                               (register 0x80+ bits 4-7)
        OP_TOTAL_LEVEL    = 5,    // 8 * TL                                  (register 0x40+ bits 0-6)
        OP_KEY_SCALE      = 6,    // KS 0..3                                 (register 0x50+ bits 6-7)
        OP_MULTIPLE       = 7,    // 2 * MUL (or 1 when MUL = 0)             (register 0x30+ bits 0-3)
        OP_DT2_OFFSET     = 8,    // DT2 index offset (OPM only)             (OPM register 0xC0+ bits 6-7)
        OP_AM_MASK        = 9,    // -1 if this operator is affected by the tremolo LFO, else 0
        OP_DT1_INDEX      = 10,   // start index into kDetuneOffsetByKeyCode (register 0x30+ bits 4-6)
        OP_CH3_FREQ       = 11,   // this operator's own frequency in channel-3 special mode
        OP_PHASE          = 12,   // phase accumulator
        OP_ENV_STATE      = 13,   // 0 attack, 1 decay, 2 sustain, 3 release, 4 off
        OP_PHASE_STEP     = 14,   // phase increment per sample (set by calcPhaseSteps)
        OP_ENV_LEVEL      = 15,   // envelope attenuation, 0 (loudest) .. 1023 (silent)
        OP_OUTPUT         = 16,   // this sample's output
        OP_UNUSED_17      = 17,
        OP_ENV_ACC        = 18,   // envelope accumulator (attenuation << 12)
        OP_UNUSED_19      = 19,
        OP_ENV_COUNTER    = 20,   // samples left until the next envelope update
        OP_MOD_INPUT_1    = 21,   // dword index of the cell whose low 16 bits modulate this operator's phase
        OP_MOD_INPUT_2    = 22,   //   a second modulation input
        OP_RATE_ATTACK    = 23,   // 4 envelope step cells (attack, decay1, decay2, release): mantissa | exponent << 24
        OP_RATE_DECAY1    = 24,
        OP_RATE_DECAY2    = 25,
        OP_RATE_RELEASE   = 26,
        OP_UNUSED_27      = 27,
        OP_CHANNEL        = 28    // dword index of this operator's channel record
    };

    // Per-channel fields (added to CH_BASE + CH_STRIDE * channel number).
    enum ChannelField {
        CH_STRIDE         = 18,
        CH_ALGORITHM      = 0,    // connection (bits 0-2) - decides which operators feed which; -1 = never set
        CH_FEEDBACK_SHIFT = 1,    // operator-1 self-feedback: output is shifted right by this (23 = feedback off)
        CH_KEY_CODE       = 2,    // key code used for key scaling and detune (OPN: 4 * code, OPM: code)
        CH_FREQUENCY      = 3,    // channel frequency (OPN: fnum << block, OPM: packed index + octave shift)
        CH_AM_DEPTH       = 4,    // tremolo sensitivity (AMS)
        CH_PM_DEPTH       = 5,    // vibrato sensitivity (PMS)
        CH_MASK_LEFT      = 6,    // -1 = channel is audible on the left
        CH_MASK_RIGHT     = 7,    // -1 = channel is audible on the right
        CH_DIRTY          = 8,    // 1 = frequency/rate registers changed, recompute rates before the next sample
        CH_CH3_SPECIAL    = 9,    // 1 = per-operator frequencies are used (channel-3 special mode)
        CH_FEEDBACK_OUT   = 10,   // operator-1 feedback value fed back into its own phase
        CH_FEEDBACK_PREV  = 11,   // previous operator-1 output
        CH_PM_FACTOR      = 12,   // current vibrato factor (2048 = none), refreshed every 8 samples
        CH_AM_FACTOR      = 13,   // current tremolo attenuation, refreshed every 8 samples
        CH_OUTPUT_PTR     = 14    // 4 dword indices of the operator outputs summed into this channel (14..17)
    };

    // Byte offsets (from the start of the image) of the register shadows.
    enum { SHADOW_OPN = 42, SHADOW_OPM = 298 };

    std::vector<int32_t> d;       // the state image

    int32_t&  D(int i)    { return d[i]; }
    uint8_t*  bytes()     { return reinterpret_cast<uint8_t*>(d.data()); }
    uint8_t&  B(int off)  { return bytes()[off]; }
    uint16_t  W(int idx)  { return (uint16_t)d[idx]; }               // low 16 bits of a dword cell
    static int opIdx(int op, int field) { return OP_BASE + OP_STRIDE * op + field; }
    static int chIdx(int ch, int field) { return CH_BASE + CH_STRIDE * ch + field; }

    void reset();                                      // [sub_1001B7F0]
    void opmWrite(int reg, int val);                   // OPM register map          [sub_1001AE40]
    void connectOperators(int ch, int algorithm);      // algorithm routing         [sub_10019F30]
    void updateChannelRates(int ch);                   // recompute rates/steps     [sub_1001A580]
    void calcPhaseSteps(int ch);                       // per-operator frequency    [sub_10019E00 / sub_10019E90]
    int  makeEnvelopeRate(int rate);                   // rate 0..63 -> step cell   [sub_1001A510]
    void updateLfo();                                  // [sub_10003A40]
    void renderPsg(int32_t* buf, int n);               // [sub_10019990]

    // Register handlers.  Comments give the hardware register they implement and the original function.
    void setLfoDepth(int8_t v);                        // OPM 0x19: bit 7 selects PMD (1) or AMD (0)      [sub_100033D0]
    void setChannelFrequency(int ch, int v);           // OPN 0xA0/0xA4: 14-bit block+fnum                [sub_10003410]
    void setCh3OperatorFrequency(int op, int v);       // OPN 0xA8-0xAA: per-operator frequency           [sub_100034B0]
    void setOpnLfoSensitivity(int ch, int v);          // OPN 0xB4: PMS, AMS, pan                         [sub_10003520]
    void setAlgorithmFeedback(int ch, int v);          // OPN 0xB0 / OPM 0x20: algorithm + feedback       [sub_10003580]
    void setCh3Mode(int v);                            // OPN 0x27                                        [sub_100035F0]
    void keyOnOff(int ch, int keyMask);                // OPN 0x28 / OPM 0x08: key on/off per operator    [sub_10003660]
    void setPan(int ch, int v);                        // OPM 0x20 bits 6-7 / OPN 0xB4 bits 6-7           [sub_10003740]
    void setDetuneMultiple(int op, int v);             // 0x30+: DT1, MUL                                 [sub_10003790]
    void setTotalLevel(int op, int v);                 // 0x40+: TL                                       [sub_10003810]
    void setKeyScaleAttack(int op, int v);             // 0x50+: KS, AR                                   [sub_10003840]
    void setDecay1AndAm(int op, int v);                // 0x60+: AM enable, D1R                           [sub_100038A0]
    void setDecay2AndDt2(int op, int v);               // 0x70+: D2R (OPM 0xC0+: DT2 + D2R)               [sub_10003900]
    void setSustainRelease(int op, int v);             // 0x80+: D1L, RR                                  [sub_10003960]
    void setOpmLfoSensitivity(int ch, int v);          // OPM 0x38: PMS, AMS                              [sub_100032D0]
    void setOpmKeyCode(int ch, int8_t keyCode, int keyFraction);   // OPM 0x28/0x30                       [sub_10003340]
};

}  // namespace opnmml
