// ================================================================================================================
// Opx4 - port of wuopn.dll's sound chip emulator.  See opx4.hpp for the overview and the state layout.
//
// How one output sample is produced (generate()):
//   1. every 8 chip samples the LFO is advanced and each channel gets a fresh vibrato factor and tremolo value;
//   2. for each of the (up to 32) operators:
//        - the envelope generator moves the operator's attenuation along attack -> decay -> sustain -> release;
//        - the phase accumulator advances by the operator's phase step (scaled by the channel's vibrato factor);
//        - the output is  sine[(phase + modulation inputs) & 1023]  attenuated by (TL + tremolo + envelope);
//   3. each channel sums the outputs of its "carrier" operators (the routing depends on the algorithm) and the first
//      operator's feedback is updated;
//   4. because the chip runs at 55.5/62.5 kHz but the output at 44.1 kHz, the chip samples are averaged into output
//      samples (a 14-bit fixed-point accumulator decides how many chip samples belong to each output sample);
//   5. the PSG is rendered separately, directly at the output rate, and added.
//
// Function names in [brackets] are the Hex-Rays names from wuopn.cpp, so each function can be compared with the decompile.
// ================================================================================================================
#include "opx4.hpp"
#include "chiptables.inc"
#include <cstring>

namespace opnmml {
namespace {

// ---- Random table for noise / random LFO  (word_10035A60, filled by sub_1001A490) -------------------------------
// The DLL fills 16384 entries with (rand() & 0xFFF) - 2048.  MSVC's rand() with the default seed 1 is this LCG.  If
// anything else in the host process called rand()/srand() first, the real table differs: use Opx4::setNoiseSeed().
int16_t  g_noise[16384];
bool     g_noiseReady = false;
uint32_t g_seed = 1;
int msvcRand() { g_seed = g_seed * 214013u + 2531011u; return (int)((g_seed >> 16) & 0x7FFF); }
void initNoiseTable() {
    if (g_noiseReady) return;
    g_noiseReady = true;
    for (int i = 0; i < 0x4000; i++) g_noise[i] = (int16_t)((msvcRand() & 0xFFF) - 2048);
}

// ---- Bounds-checked table reads --------------------------------------------------------------------------------
// A table index that runs off the end reads the neighbouring table (they are contiguous in the DLL, and in
// kChipRodata).  Anything beyond the stored data reads as 0 rather than crashing.
const long kRodataSize = (long)(sizeof kChipRodata / sizeof kChipRodata[0]);
const long kRateBlockSize = (long)(sizeof kRateBlock / sizeof kRateBlock[0]);
inline int rodata(const int* view, long index) {
    long i = (view - kChipRodata) + index;
    return (i >= 0 && i < kRodataSize) ? kChipRodata[i] : 0;
}
inline int rodataByte(const unsigned char* view, long index) {
    const unsigned char* base = reinterpret_cast<const unsigned char*>(kChipRodata);
    long i = (view - base) + index;
    return (i >= 0 && i < kRodataSize * 4) ? base[i] : 0;
}
inline int rateData(const int* view, long index) {
    long i = (view - kRateBlock) + index;
    return (i >= 0 && i < kRateBlockSize) ? kRateBlock[i] : 0;
}

// The DLL does all arithmetic in 32-bit registers, so overflow wraps; do the same without C++ undefined behaviour.
inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

}  // namespace

void Opx4::setNoiseSeed(uint32_t seed) { g_seed = seed; g_noiseReady = false; }

// =================================================================================================================
// Construction, chip type, reset
// =================================================================================================================

Opx4::Opx4(int outRate, int clock) : d(1400, 0) {                               // [sub_1001BC80]
    D(G_CLOCK) = clock;
    D(G_OUTPUT_GAIN) = 1024;
    D(G_OUTPUT_RATE) = outRate;
    D(G_RESAMPLE_ACC) = 0;
    D(G_RESAMPLE_STEP) = 0x4000;
    initNoiseTable();                                                           // [sub_1001A490]
    D(G_MODE) = MODE_OPN;
    setMode(D(G_MODE));
}

void Opx4::setMode(int m) {                                                     // [sub_1001BB20]
    D(G_MODE) = m;
    // Internal sample rate and the set of operators that are generated depend on the chip type.
    switch (m) {
    case MODE_OPN: case MODE_OPN_MZ:
        D(G_CHIP_RATE) = (int32_t)((uint32_t)D(G_CLOCK) / 72u);  D(G_OPERATOR_MASK) = 0xFFF;    break;   // 12 operators
    case MODE_OPM: case MODE_OPM_X1:
        D(G_CHIP_RATE) = (int32_t)((uint32_t)D(G_CLOCK) >> 6);   D(G_OPERATOR_MASK) = -1;       break;   // 32 operators
    case MODE_OPN_MASK_2:
        D(G_CHIP_RATE) = (int32_t)((uint32_t)D(G_CLOCK) / 72u);  D(G_OPERATOR_MASK) = 16715760; break;
    case MODE_OPN_24OP:
        D(G_CHIP_RATE) = (int32_t)((uint32_t)D(G_CLOCK) / 72u);  D(G_OPERATOR_MASK) = 0xFFFFFF; break;
    default: break;
    }
    // [sub_1001A450] chip samples -> output samples, 14-bit fixed point
    D(G_RESAMPLE_STEP) = (int32_t)(((uint32_t)D(G_OUTPUT_RATE) << 14) / (uint32_t)D(G_CHIP_RATE));
    // [sub_1001A6A0] scale used to convert PSG periods and LFO rates to phase steps (wraps in 32 bits like the DLL)
    D(G_PHASE_SCALE) = (int32_t)((((uint32_t)D(G_CLOCK) << 9) / (uint32_t)D(G_OUTPUT_RATE)) << 12);
    reset();
    B(5000) = 0;       // byte flag at +5000: selects an unused fast-forward path in the DLL (sub_10019760); always 0
}

void Opx4::reset() {                                                            // [sub_1001B7F0]
    B(40) = 0; B(41) = 0;                                       // two flag bytes the DLL clears; never read again
    memset(bytes() + SHADOW_OPN, 0, 0x200);                     // both register shadows (OPN/PSG and OPM)
    memset(bytes() + 4 * OP_BASE, 0, 32 * OP_STRIDE * 4);       // all operators
    memset(bytes() + 4 * CH_BASE, 0, 8 * CH_STRIDE * 4);        // all channels
    for (int op = 0; op < 32; op++) {
        D(opIdx(op, OP_MOD_INPUT_1)) = G_DUMMY;
        D(opIdx(op, OP_MOD_INPUT_2)) = G_DUMMY;
        D(opIdx(op, OP_ENV_ACC))     = 4190208;                 // 1023 << 12: fully attenuated
        D(opIdx(op, OP_ENV_COUNTER)) = 8;
        D(opIdx(op, OP_UNUSED_27))   = makeEnvelopeRate(0);
        D(opIdx(op, OP_CHANNEL))     = chIdx(op >> 2, 0);
        // Operator 1 of every channel is modulated by its own feedback value.
        if ((op & 3) == 0) D(opIdx(op, OP_MOD_INPUT_1)) = chIdx(op >> 2, CH_FEEDBACK_OUT);
    }
    for (int ch = 0; ch < 8; ch++) {
        D(chIdx(ch, CH_ALGORITHM))  = -1;                       // forces connectOperators() on the first write
        D(chIdx(ch, CH_MASK_LEFT))  = -1;
        D(chIdx(ch, CH_MASK_RIGHT)) = -1;
        D(chIdx(ch, CH_DIRTY))      = 1;
        for (int k = 0; k < 4; k++) D(chIdx(ch, CH_OUTPUT_PTR + k)) = G_DUMMY;
    }
    D(G_CH3_MODE) = 0; D(G_OPM_NOISE_STEP) = 0; D(G_PM_LFO) = 0; D(G_AM_LFO) = 0;
    D(G_SAMPLE_COUNT) = 0; D(G_LFO_PHASE) = 0; D(G_LFO_STEP) = 0;
    D(G_LFO_WAVE) = 2; D(G_LFO_AMD) = 127; D(G_LFO_PMD) = 127;
    D(G_UNUSED_144) = 0;
    setLfoDepth((int8_t)255);                                   // PMD = 127
    setLfoDepth(127);                                           // AMD = 127
    D(G_LFO_WAVE) = 2 & 3;                                      // [sub_100039F0] triangle
    for (int r = 0; r < 13; r++) write(r, 0);                   // clear PSG registers 0..12 (or OPM 0..12)
    write(7, 56);                                               // PSG mixer: tones on, noise off
    D(G_PSG_LFSR) = 424936722;                                  // noise shift register seed
    D(G_PSG_OUT_BITS) = 0;
    D(G_LEFTOVER_COUNT) = 0; D(G_LEFTOVER_L) = 0; D(G_LEFTOVER_R) = 0;
    for (int op = 0; op < 32; op++) D(opIdx(op, OP_ENV_STATE)) = 4;    // all operators off [sub_1001A3F0]
}

int Opx4::read(int reg) { return (reg >= 0 && reg < 0x20) ? B(SHADOW_OPN + reg) : 0; }

void Opx4::write(int reg, int val) {                                            // [sub_1001B7A0]
    if (D(G_MODE) == MODE_OPM || D(G_MODE) == MODE_OPM_X1) opmWrite(reg, val);
    else                                                    writeOpn(reg, val);
}

// =================================================================================================================
// Register handlers (each decodes one hardware register field into the operator/channel records)
// =================================================================================================================

// OPM 0x19: bit 7 = 1 sets the vibrato depth (PMD), bit 7 = 0 the tremolo depth (AMD).
void Opx4::setLfoDepth(int8_t v) { if (v < 0) D(G_LFO_PMD) = v & 0x7F; else D(G_LFO_AMD) = v & 0x7F; }   // [sub_100033D0]

// OPN 0xA0 (low byte) with 0xA4 (high byte): v = block (bits 11-13) and F-number (bits 0-10).
void Opx4::setChannelFrequency(int ch, int v) {                                 // [sub_10003410]
    int blockAndFnum = (int16_t)v & 0x3FFF;
    int freq = (blockAndFnum & 0x7FF) << (blockAndFnum >> 11);
    D(chIdx(ch, CH_KEY_CODE))  = 4 * rodataByte(kKeyCodeFromFnum, blockAndFnum >> 7);
    D(chIdx(ch, CH_FREQUENCY)) = freq;
    D(opIdx(4 * ch, OP_CH3_FREQ)) = freq;
    D(chIdx(ch, CH_DIRTY)) = 1;
}

// OPN 0xA8-0xAA: frequency of one operator of channel 3 (used only in channel-3 special mode).
void Opx4::setCh3OperatorFrequency(int op, int v) {                             // [sub_100034B0]
    int freq = ((int16_t)v & 0x7FF) << (((int16_t)v & 0x3FFF) >> 11);
    D(opIdx(op, OP_CH3_FREQ)) = freq;
    D(chIdx(op >> 2, CH_DIRTY)) = 1;
}

// OPN 0xB4: bits 0-2 vibrato sensitivity (PMS), bits 4-5 tremolo sensitivity (AMS), bits 6-7 pan.
void Opx4::setOpnLfoSensitivity(int ch, int v) {                                // [sub_10003520]
    D(chIdx(ch, CH_AM_DEPTH)) = rodata(kAmsDepth, (v >> 4) & 3);
    D(chIdx(ch, CH_PM_DEPTH)) = rodata(kOpnPmsDepth, v & 7);
    setPan(ch, v >> 6);
}

// OPN 0xB0 / OPM 0x20: bits 0-2 algorithm, bits 3-5 feedback level.
void Opx4::setAlgorithmFeedback(int ch, int v) {                                // [sub_10003580]
    connectOperators(ch, (D(chIdx(ch, CH_ALGORITHM)) & 0x18) | (v & 7));
    int feedback = (v >> 3) & 7;
    D(chIdx(ch, CH_FEEDBACK_SHIFT)) = feedback ? 9 - feedback : 23;           // 23 shifts everything away = off
}

// OPN 0x27 bits 6-7: channel-3 special mode.  The DLL passes (value >> 6) and then shifts by 6 AGAIN, so the mode
// number is always 0 and this never takes effect - kept as decompiled.
void Opx4::setCh3Mode(int v) {                                                  // [sub_100035F0]
    int mode = v >> 6;
    if (mode != D(G_CH3_MODE)) {
        D(G_CH3_MODE) = mode;
        D(chIdx(2, CH_DIRTY)) = 1;  D(chIdx(5, CH_DIRTY)) = 1;
        D(chIdx(2, CH_CH3_SPECIAL)) = mode;  D(chIdx(5, CH_CH3_SPECIAL)) = mode;
    }
}

// Key on/off for one channel: bit i of keyMask corresponds to the operator slot kKeyOnSlotOrder[i].
// Key on restarts an operator that is releasing or off; key off sends a sounding one into release.
void Opx4::keyOnOff(int ch, int keyMask) {                                      // [sub_10003660]
    for (int i = 0; i < 4; i++) {
        int op = 4 * ch + rateData(kKeyOnSlotOrder, i);
        if (keyMask & (1 << i)) {
            if ((uint32_t)D(opIdx(op, OP_ENV_STATE)) >= 3u) {
                D(opIdx(op, OP_ENV_STATE)) = 0;                 // attack
                D(opIdx(op, OP_PHASE)) = 0;
                D(opIdx(op, OP_ENV_COUNTER)) = 0;
            }
        } else if ((uint32_t)D(opIdx(op, OP_ENV_STATE)) < 3u) {
            D(opIdx(op, OP_ENV_COUNTER)) = 0;
            D(opIdx(op, OP_ENV_STATE)) = 3;                     // release
        }
    }
}

// Pan bits (bit 0 = left, bit 1 = right) -> channel masks (-1 audible, 0 muted).
void Opx4::setPan(int ch, int v) {                                              // [sub_10003740]
    D(chIdx(ch, CH_MASK_LEFT))  = -(v & 1);
    D(chIdx(ch, CH_MASK_RIGHT)) = -((v >> 1) & 1);
}

// 0x30+: bits 0-3 frequency multiple, bits 4-6 detune 1.
void Opx4::setDetuneMultiple(int op, int v) {                                   // [sub_10003790]
    D(opIdx(op, OP_MULTIPLE))   = (v & 0xF) ? 2 * (v & 0xF) : 1;
    D(opIdx(op, OP_DT1_INDEX))  = rodata(kDetuneBaseIndex, (v >> 4) & 7);
    D(chIdx(op >> 2, CH_DIRTY)) = 1;
}

// 0x40+: total level, 0.75 dB per step (stored x8: the envelope scale is 10.7 units per dB).
void Opx4::setTotalLevel(int op, int v) { D(opIdx(op, OP_TOTAL_LEVEL)) = 8 * (v & 0x7F); }   // [sub_10003810]

// 0x50+: bits 0-4 attack rate, bits 6-7 key scale.
void Opx4::setKeyScaleAttack(int op, int v) {                                   // [sub_10003840]
    D(opIdx(op, OP_ATTACK_RATE)) = 2 * (v & 0x1F);
    D(opIdx(op, OP_KEY_SCALE))   = (v >> 6) & 3;
    D(chIdx(op >> 2, CH_DIRTY))  = 1;
}

// 0x60+: bit 7 tremolo enable (AM), bits 0-4 first decay rate.
void Opx4::setDecay1AndAm(int op, int v) {                                      // [sub_100038A0]
    D(opIdx(op, OP_DECAY1_RATE)) = 2 * (v & 0x1F);
    D(opIdx(op, OP_AM_MASK))     = -((v >> 7) != 0);
    D(chIdx(op >> 2, CH_DIRTY))  = 1;
}

// 0x70+ (OPM 0xC0+): bits 0-4 second decay rate; OPM also has detune 2 in bits 6-7.
void Opx4::setDecay2AndDt2(int op, int v) {                                     // [sub_10003900]
    D(opIdx(op, OP_DECAY2_RATE)) = 2 * (v & 0x1F);
    D(opIdx(op, OP_DT2_OFFSET))  = rodata(kDt2IndexOffset, (v >> 6) & 3);
    D(chIdx(op >> 2, CH_DIRTY))  = 1;
}

// 0x80+: bits 0-3 release rate, bits 4-7 sustain level (where decay 1 hands over to decay 2).
void Opx4::setSustainRelease(int op, int v) {                                   // [sub_10003960]
    D(opIdx(op, OP_RELEASE_RATE))  = 4 * (v & 0xF) + 2;
    D(opIdx(op, OP_SUSTAIN_LEVEL)) = ((v >> 4) & 0xF) << 17;
    D(chIdx(op >> 2, CH_DIRTY))    = 1;
}

// OPM 0x38: bits 0-1 tremolo sensitivity, bits 4-6 vibrato sensitivity.
void Opx4::setOpmLfoSensitivity(int ch, int v) {                                // [sub_100032D0]
    D(chIdx(ch, CH_AM_DEPTH)) = (v & 3) ? (255 << ((v & 3) - 1)) : 0;
    D(chIdx(ch, CH_PM_DEPTH)) = rodata(kOpmPmsDepth, (v >> 4) & 7);
}

// OPM 0x28 (key code: octave in bits 4-6, note in bits 0-3) with 0x30 (key fraction, bits 2-7).
void Opx4::setOpmKeyCode(int ch, int8_t keyCode, int keyFraction) {             // [sub_10003340]
    D(chIdx(ch, CH_KEY_CODE)) = keyCode & 0x7F;
    int kc = D(chIdx(ch, CH_KEY_CODE));
    D(chIdx(ch, CH_FREQUENCY)) = ((kc >> 4) << 16) + (keyFraction >> 2) + (rodata(kOpmNoteToSemitone, kc & 0xF) << 6);
    D(chIdx(ch, CH_DIRTY)) = 1;
}

// =================================================================================================================
// OPN / PSG register write  [sub_1001B220]
//   0x00-0x0F  PSG registers      0x22 LFO      0x27 channel-3 mode      0x28 key on/off
//   0x30-0x8F  operator registers (DT/MUL, TL, KS/AR, AM/D1R, D2R, D1L/RR)
//   0xA0-0xAA  frequency          0xB0 algorithm/feedback               0xB4 sensitivity/pan
// Only "port 0" (channels 1-3) is decoded; the second port of an OPNA is not implemented.
// =================================================================================================================
void Opx4::writeOpn(int reg, int val) {
    int r = reg & 0xFF, v = val & 0xFF;
    B(SHADOW_OPN + r) = (uint8_t)v;
    if (r >= 0x30) {
        if (r < 0xA0) {
            if ((r & 3) != 3) {                                  // slot 3 of the register map is unused
                int op = ((r >> 2) & 3) + 4 * (r & 3);            // register order is S1,S3,S2,S4 per channel
                switch (r & 0xF0) {
                case 0x30: setDetuneMultiple(op, v); break;
                case 0x40: setTotalLevel(op, v); break;
                case 0x50: setKeyScaleAttack(op, v); break;
                case 0x60: setDecay1AndAm(op, v); break;
                case 0x70: setDecay2AndDt2(op, v & 0x1F); break;
                case 0x80: setSustainRelease(op, v); break;
                default: break;                                   // 0x90 (SSG-EG) is not implemented
                }
            }
        } else if ((r & 3) != 3) {
            int ch = r & 3;
            int high = B(SHADOW_OPN + r + 4) & 0x3F;              // matching high-byte register (0xA4+ / 0xAC+)
            switch (r & 0xFC) {
            case 0xA0: setChannelFrequency(ch, v | (high << 8)); break;
            case 0xA8: setCh3OperatorFrequency(rateData(kCh3SlotForReg, r - 0xA8) + 4, v | (high << 8)); break;
            case 0xB0: setAlgorithmFeedback(ch, v); break;
            case 0xB4: if (D(G_MODE) == MODE_OPN) v |= 0xC0;      // plain OPN has no pan: force both sides on
                       setOpnLfoSensitivity(ch, v); break;
            default: break;
            }
        }
        return;
    }
    switch (r) {
    case 0: case 2: case 4: {                                    // PSG tone period, low byte (A, B, C)
        int voice = r >> 1;
        uint32_t period = (uint32_t)((v + (B(SHADOW_OPN + r + 1) << 8)) & 0xFFF);
        if (period == 0) period = 1;
        D(G_PSG_TONE_STEP + voice) = (int32_t)((uint32_t)D(G_PHASE_SCALE) / period);
        if ((uint32_t)D(G_PSG_TONE_STEP + voice) > 0x2000000u) D(G_PSG_TONE_STEP + voice) = 0;   // too high to render
        break;
    }
    case 1: case 3: case 5: {                                    // PSG tone period, high byte
        int voice = r >> 1;
        uint32_t period = (uint32_t)((B(SHADOW_OPN + r - 1) + (v << 8)) & 0xFFF);
        if (period == 0) period = 1;
        D(G_PSG_TONE_STEP + voice) = (int32_t)((uint32_t)D(G_PHASE_SCALE) / period);
        if ((uint32_t)D(G_PSG_TONE_STEP + voice) > 0x2000000u) D(G_PSG_TONE_STEP + voice) = 0;
        break;
    }
    case 6: {                                                    // PSG noise period
        uint32_t period = (uint32_t)(v & 0x1F);
        if (period == 0) period = 1;
        D(G_PSG_NOISE_STEP) = (int32_t)(((uint32_t)D(G_PHASE_SCALE) / period) >> 1);
        break;
    }
    case 8: case 9: case 10:                                     // PSG channel volume: bits 0-3 level, bit 4 = use envelope
        D(G_PSG_CH_AMP + (r - 8)) = rodata(kPsgAmplitude, 2 * (v & 0xF));
        D(G_PSG_VOL_PTR + (r - 8)) = (v & 0x10) ? G_PSG_ENV_AMP : (G_PSG_CH_AMP + (r - 8));
        break;
    case 0xB: case 0xC: {                                        // PSG envelope period
        D(G_PSG_ENV_STEP) = (int32_t)(uint16_t)(B(SHADOW_OPN + 0xB) | (B(SHADOW_OPN + 0xC) << 8));
        if (D(G_PSG_ENV_STEP) == 0) D(G_PSG_ENV_STEP) = 1;
        D(G_PSG_ENV_STEP) = (int32_t)(((uint32_t)(D(G_PHASE_SCALE) / D(G_PSG_ENV_STEP))) >> 1);
        break;
    }
    case 0xD:                                                    // PSG envelope shape (restarts the envelope)
        D(G_PSG_ENV_PHASE) = 0;
        D(G_PSG_ENV_SHAPE) = v & 0xF;
        break;
    case 0x22:                                                   // LFO control: bit 3 enable, bits 0-2 speed
        if (v & 8) D(G_LFO_STEP) = rodata(kLfoRate, v & 7);
        else       { D(G_LFO_STEP) = 0; D(G_LFO_PHASE) = 0; D(G_SAMPLE_COUNT) = 0; }
        break;
    case 0x27: setCh3Mode(v >> 6); break;
    case 0x28: if ((v & 3) != 3) keyOnOff(v & 3, v >> 4); break;  // bits 0-1 channel, bits 4-7 operators
    default: break;
    }
}

// =================================================================================================================
// OPM register write  [sub_1001AE40]
//   0x01 test/LFO reset   0x08 key on/off   0x0F noise   0x18 LFO speed   0x19 LFO depth   0x1B LFO waveform
//   0x20 pan/feedback/algorithm   0x28 key code   0x30 key fraction   0x38 LFO sensitivity
//   0x40-0xFF operator registers (DT1/MUL, TL, KS/AR, AM/D1R, DT2/D2R, D1L/RR)
// =================================================================================================================
void Opx4::opmWrite(int reg, int val) {
    int r = reg & 0xFF, v = val & 0xFF;
    B(SHADOW_OPM + r) = (uint8_t)v;
    switch (r) {
    case 0x01: if (v & 2) D(G_LFO_PHASE) = 0; return;
    case 0x08: keyOnOff(v & 7, v >> 3); return;                  // bits 0-2 channel, bits 3-6 operators
    case 0x0F: D(G_OPM_NOISE_STEP) = (v & 0x80) ? (int32_t)(0x400000 / (32 * (32 - (v & 0x1F)))) : 0; return;
    case 0x18: D(G_LFO_STEP) = ((v & 0xF) + 16) << 13 >> (15 - (v >> 4)); return;
    case 0x19: setLfoDepth((int8_t)v); return;
    case 0x1B: D(G_LFO_WAVE) = v & 3; return;
    default: break;
    }
    if (r < 0x20) return;                                        // everything else below 0x20 is ignored
    if (r >= 0x40) {
        int op = ((r >> 3) & 3) + 4 * (r & 7);                    // OPM: channel = r & 7, slot = bits 3-4
        switch (r & 0xE0) {
        case 0x40: setDetuneMultiple(op, v); break;
        case 0x60: setTotalLevel(op, v); break;
        case 0x80: setKeyScaleAttack(op, v); break;
        case 0xA0: setDecay1AndAm(op, v); break;
        case 0xC0: setDecay2AndDt2(op, v); break;
        case 0xE0: setSustainRelease(op, v); break;
        default: break;
        }
    } else {
        int ch = r & 7;
        switch (r & 0xF8) {
        case 0x20: setAlgorithmFeedback(ch, v); setPan(ch, v >> 6); break;
        case 0x28: case 0x30:                                    // key code and key fraction are separate registers
            setOpmKeyCode(ch, (int8_t)B(SHADOW_OPM + 0x28 + ch), B(SHADOW_OPM + 0x30 + ch));
            break;
        case 0x38: setOpmLfoSensitivity(ch, v); break;
        default: break;
        }
    }
}

// =================================================================================================================
// Operator routing by algorithm  [sub_10019F30]
// An operator's output is fed to other operators through OP_MOD_INPUT_1/2 (pointer cells), and the operators that
// reach the speakers ("carriers") are listed in CH_OUTPUT_PTR 0..3.  Unused inputs/outputs point at G_DUMMY (= 0).
//   Operators are numbered by register slot 0..3 (the order of the 0x30+ registers: S1, S3, S2, S4 of the hardware).
// =================================================================================================================
void Opx4::connectOperators(int ch, int a3) {
    int c = chIdx(ch, CH_ALGORITHM);
    if (D(c) == a3) return;
    D(c) = a3;
    const int out0 = opIdx(4 * ch + 0, OP_OUTPUT), out1 = opIdx(4 * ch + 1, OP_OUTPUT);
    const int out2 = opIdx(4 * ch + 2, OP_OUTPUT), out3 = opIdx(4 * ch + 3, OP_OUTPUT);
    const int X = G_DUMMY;
    // slotNIn* = what modulates the operator in register slot N; o0..o3 = the cells summed into the channel output.
    int slot2In, slot1InA, slot1InB, slot3InA, slot3InB, o0, o1, o2, o3;
    switch (a3 & 7) {
    case 0: slot2In = out0; slot1InA = out2; slot1InB = X;    slot3InA = out1; slot3InB = X;    o0 = out3; o1 = X;    o2 = X;    o3 = X;    break;
    case 1: slot2In = X;    slot1InA = out0; slot1InB = out2; slot3InA = out1; slot3InB = X;    o0 = out3; o1 = X;    o2 = X;    o3 = X;    break;
    case 2: slot2In = X;    slot1InA = out2; slot1InB = X;    slot3InA = out1; slot3InB = out0; o0 = out3; o1 = X;    o2 = X;    o3 = X;    break;
    case 3: slot2In = out0; slot1InA = X;    slot1InB = X;    slot3InA = out1; slot3InB = out2; o0 = out3; o1 = X;    o2 = X;    o3 = X;    break;
    case 4: slot2In = out0; slot1InA = X;    slot1InB = X;    slot3InA = out1; slot3InB = X;    o0 = out2; o1 = out3; o2 = X;    o3 = X;    break;
    case 5: slot2In = out0; slot1InA = out0; slot1InB = X;    slot3InA = out0; slot3InB = X;    o0 = out1; o1 = out2; o2 = out3; o3 = X;    break;
    case 6: slot2In = out0; slot1InA = X;    slot1InB = X;    slot3InA = X;    slot3InB = X;    o0 = out1; o1 = out2; o2 = out3; o3 = X;    break;
    default:slot2In = X;    slot1InA = X;    slot1InB = X;    slot3InA = X;    slot3InB = X;    o0 = out0; o1 = out1; o2 = out2; o3 = out3; break;
    }
    D(opIdx(4 * ch + 2, OP_MOD_INPUT_1)) = slot2In;
    D(opIdx(4 * ch + 1, OP_MOD_INPUT_1)) = slot1InA;
    D(opIdx(4 * ch + 1, OP_MOD_INPUT_2)) = slot1InB;
    D(opIdx(4 * ch + 3, OP_MOD_INPUT_1)) = slot3InA;
    D(opIdx(4 * ch + 3, OP_MOD_INPUT_2)) = slot3InB;
    D(chIdx(ch, CH_OUTPUT_PTR + 0)) = o0;
    D(chIdx(ch, CH_OUTPUT_PTR + 1)) = o1;
    D(chIdx(ch, CH_OUTPUT_PTR + 2)) = o2;
    D(chIdx(ch, CH_OUTPUT_PTR + 3)) = o3;
}

// =================================================================================================================
// Envelope rates and phase steps
// =================================================================================================================

// Converts an envelope rate (0..63) to a "step cell": the low 24 bits are the amount the envelope moves each time it
// updates, the top byte is the exponent that sets how many samples pass between updates.  [sub_1001A510]
int Opx4::makeEnvelopeRate(int rate) {
    if (rate > 63) rate = 63;
    if ((rate >> 2) >= 12) return rateData(kRateMantissaFast, rate - 48) | (rateData(kRateExponent, rate >> 2) << 24);
    if (rate <= 2) return 251658240;                             // 0x0F000000: effectively no movement
    return rateData(kRateMantissa, rate & 3) | (rateData(kRateExponent, rate >> 2) << 24);
}

// Phase step (frequency) of each operator: multiple * (detune offset + channel frequency).   [sub_10019E00 / E90]
// OPN: the channel frequency is the F-number shifted by the block.
// OPM: the frequency comes from kOpmFrequencyTable, shifted by the octave, plus the DT2 offset.
void Opx4::calcPhaseSteps(int ch) {
    int chb = chIdx(ch, 0), m = D(G_MODE);
    for (int slot = 0; slot < 4; slot++) {
        int o = opIdx(4 * ch + slot, 0);
        int detune = rodata(kDetuneOffsetByKeyCode, (long)D(o + OP_DT1_INDEX) + (D(chb + CH_KEY_CODE) >> 2));
        if (m == MODE_OPM || m == MODE_OPM_X1) {
            uint32_t f = (uint32_t)D(chb + CH_FREQUENCY);
            int base = rodata(kOpmFrequencyTable, (long)D(o + OP_DT2_OFFSET) + (uint16_t)f) << ((f >> 16) & 0xFF);
            uint32_t r = (uint32_t)mul32(D(o + OP_MULTIPLE), add32(detune, base));
            D(o + OP_PHASE_STEP) = (int32_t)(r >> 1);
        } else {
            int freq = (D(chb + CH_CH3_SPECIAL) != 0) ? D(o + OP_CH3_FREQ) : D(chb + CH_FREQUENCY);
            D(o + OP_PHASE_STEP) = mul32(D(o + OP_MULTIPLE), add32(detune, freq));
        }
    }
}

// Recompute everything that depends on a channel's frequency and rate registers.  Rates speed up with the key code
// (key scaling): the key code is shifted right by (5 - KS) and added to each rate.   [sub_1001A580]
void Opx4::updateChannelRates(int ch) {
    calcPhaseSteps(ch);
    int chb = chIdx(ch, 0);
    for (int slot = 0; slot < 4; slot++) {
        int o = opIdx(4 * ch + slot, 0);
        int scale = D(chb + CH_KEY_CODE) >> (5 - (D(o + OP_KEY_SCALE) & 0xFF));
        int attack = D(o + OP_ATTACK_RATE);  if (attack != 0) attack += scale;
        D(o + OP_RATE_ATTACK) = makeEnvelopeRate(attack) - 0x4000000;
        int decay1 = D(o + OP_DECAY1_RATE);  if (decay1 != 0) decay1 += scale;
        D(o + OP_RATE_DECAY1) = makeEnvelopeRate(decay1);
        int decay2 = D(o + OP_DECAY2_RATE);  if (decay2 != 0) decay2 += scale;
        D(o + OP_RATE_DECAY2) = makeEnvelopeRate(decay2);
        int release = D(o + OP_RELEASE_RATE); if (release != 0) release += scale;
        D(o + OP_RATE_RELEASE) = makeEnvelopeRate(release);
    }
}

// =================================================================================================================
// LFO  [sub_10003A40] - advanced every 8 chip samples
// =================================================================================================================
void Opx4::updateLfo() {
    int am = 0, pm = 0;
    if (D(G_LFO_STEP) != 0) {
        D(G_LFO_PHASE) = add32(D(G_LFO_PHASE), 8 * D(G_LFO_STEP));
        int p = D(G_LFO_PHASE) >> 12;
        switch (D(G_LFO_WAVE)) {
        case 0:                                                  // sawtooth
            if ((D(G_LFO_PHASE) & 0x8000000) != 0) { pm = (((D(G_LFO_PHASE) >> 12) & 0x7FFF) >> 1) - 0x4000; am = (int)(uint16_t)p >> 1; }
            else                                   { pm = ((D(G_LFO_PHASE) >> 12) & 0x7FFF) >> 1;            am = (int)(uint16_t)p >> 1; }
            break;
        case 1:                                                  // square
            if ((D(G_LFO_PHASE) & 0x8000000) != 0) { pm = -16384; am = 0x7FFF; }
            else                                   { pm = 0x3FFF;  am = 0; }
            break;
        case 2:                                                  // triangle
            switch ((D(G_LFO_PHASE) >> 26) & 3) {
            case 0: pm = (D(G_LFO_PHASE) >> 12) & 0x3FFF;             am = (D(G_LFO_PHASE) >> 12) & 0x7FFF; break;
            case 1: pm = 0x4000 - ((D(G_LFO_PHASE) >> 12) & 0x3FFF);  am = (D(G_LFO_PHASE) >> 12) & 0x7FFF; break;
            case 2: pm = -((D(G_LFO_PHASE) >> 12) & 0x3FFF);          am = 0x8000 - ((D(G_LFO_PHASE) >> 12) & 0x7FFF); break;
            default:pm = ((D(G_LFO_PHASE) >> 12) & 0x3FFF) - 0x4000;  am = 0x8000 - ((D(G_LFO_PHASE) >> 12) & 0x7FFF); break;
            }
            break;
        case 3:                                                  // noise
            pm = 8 * g_noise[(p >> 9) & 0x3FFF];
            am = pm + 0x4000;
            break;
        default: break;
        }
        D(G_PM_LFO) = (D(G_LFO_PMD) * pm) >> 14;
        D(G_AM_LFO) = (D(G_LFO_AMD) * am) >> 14;
    }
}

// =================================================================================================================
// PSG  [sub_10019990] - three square-wave voices, one noise generator, one hardware envelope.
// Runs at the output sample rate.  The result is mono and is added to both stereo channels.
// =================================================================================================================
void Opx4::renderPsg(int32_t* buf, int n) {
    int polarity = (D(G_MODE) == MODE_OPN_MZ) ? -2 : 1;          // MZ-2000: the voices are subtracted instead (PWM-like)
    do {
        // Hardware envelope: top bits of the phase counter give the position, the shape register decides the curve.
        int pos = (int)(((uint32_t)D(G_PSG_ENV_PHASE) >> 25) & 0x1F);
        if ((D(G_PSG_ENV_SHAPE) & 8) != 0) {
            int shape = D(G_PSG_ENV_SHAPE) & 3;
            if (shape == 1)      { if ((uint32_t)D(G_PSG_ENV_PHASE) >> 30 != 0) pos = 31; }
            else if (shape == 2) { if ((D(G_PSG_ENV_PHASE) & 0x40000000) != 0) pos = 31 - pos; }
            else if (shape == 3) { if ((uint32_t)D(G_PSG_ENV_PHASE) >> 30 != 0) pos = 0; }
            if ((D(G_PSG_ENV_SHAPE) & 4) == 0) pos = 31 - pos;
        } else if ((D(G_PSG_ENV_SHAPE) & 4) != 0) {
            pos &= -(((uint32_t)D(G_PSG_ENV_PHASE) >> 30) == 0);
        } else {
            pos = ((uint32_t)D(G_PSG_ENV_PHASE) >> 30 != 0) ? 0 : 31 - pos;
        }
        D(G_PSG_ENV_AMP) = rodata(kPsgAmplitude, pos);
        if ((uint32_t)D(G_PSG_ENV_PHASE) >> 30 != 0) {           // envelope period finished: hold or repeat
            if ((D(G_PSG_ENV_SHAPE) & 1) == 0 && (D(G_PSG_ENV_SHAPE) & 8) != 0)
                D(G_PSG_ENV_PHASE) = (int32_t)((uint32_t)D(G_PSG_ENV_PHASE) & ~0x80000000u);
            else
                D(G_PSG_ENV_PHASE) = 0x40000000;
        }
        // Mixer register 7 (shadow byte) combined with the current square-wave levels decides which voices sound.
        int mixer = D(G_PSG_OUT_BITS) | B(SHADOW_OPN + 7);
        int audible = mixer & (mixer >> 3);
        int ampC = d[D(G_PSG_VOL_PTR + 2)], ampB = d[D(G_PSG_VOL_PTR + 1)], ampA = d[D(G_PSG_VOL_PTR + 0)];
        int sample = (ampC & -((audible >> 2) & 1)) + (ampB & -((audible >> 1) & 1)) + (ampA & -(audible & 1))
                     - polarity * ((ampC + ampB + ampA) >> 1);
        // Advance the generators.
        D(G_PSG_TONE_PHASE + 0) = add32(D(G_PSG_TONE_PHASE + 0), D(G_PSG_TONE_STEP + 0));
        D(G_PSG_TONE_PHASE + 1) = add32(D(G_PSG_TONE_PHASE + 1), D(G_PSG_TONE_STEP + 1));
        D(G_PSG_TONE_PHASE + 2) = add32(D(G_PSG_TONE_PHASE + 2), D(G_PSG_TONE_STEP + 2));
        D(G_PSG_NOISE_PHASE)    = add32(D(G_PSG_NOISE_PHASE), D(G_PSG_NOISE_STEP));
        D(G_PSG_ENV_PHASE)      = add32(D(G_PSG_ENV_PHASE), D(G_PSG_ENV_STEP));
        D(G_PSG_OUT_BITS) ^= (D(G_PSG_TONE_PHASE + 0) & 0x2000000) != 0;           // toggle when the counter overflows
        D(G_PSG_OUT_BITS) ^= 2 * ((D(G_PSG_TONE_PHASE + 1) & 0x2000000) != 0);
        D(G_PSG_OUT_BITS) ^= 4 * ((D(G_PSG_TONE_PHASE + 2) & 0x2000000) != 0);
        D(G_PSG_TONE_PHASE + 0) &= 0x1FFFFFF; D(G_PSG_TONE_PHASE + 1) &= 0x1FFFFFF; D(G_PSG_TONE_PHASE + 2) &= 0x1FFFFFF;
        if ((D(G_PSG_NOISE_PHASE) & 0x2000000) != 0) {           // noise: 17-bit-style LFSR step
            D(G_PSG_NOISE_PHASE) &= 0x1FFFFFF;
            if ((D(G_PSG_LFSR) & 1) != 0) D(G_PSG_LFSR) ^= 0x8302;
            D(G_PSG_LFSR) = (int32_t)((uint32_t)D(G_PSG_LFSR) >> 1);
            D(G_PSG_OUT_BITS) = (-(D(G_PSG_LFSR) & 1) & 0x38) | (D(G_PSG_OUT_BITS) & 7);
        }
        buf[0] = add32(buf[0], sample);
        buf[1] = add32(buf[1], sample);
        buf += 2;
        --n;
    } while (n != 0);
}

// =================================================================================================================
// Sample generator  [sub_1001A6E0]
// =================================================================================================================
void Opx4::generate(int32_t* out, int n) {
    if (n <= 0) return;
    // (A fast-forward path guarded by the byte flag at +5000 exists in the DLL; it is never enabled.)

    // 1. Recompute rates/frequencies for channels whose registers changed since the last call.
    for (int ch = 0; ch < 8; ch++) {
        if (D(chIdx(ch, CH_DIRTY)) != 0) {
            updateChannelRates(ch);
            D(chIdx(ch, CH_DIRTY)) = 0;
        }
    }
    // 2. PSG (only chip types that have one).
    switch (D(G_MODE)) {
    case MODE_OPN: case MODE_OPN_MASK_2: case MODE_OPM_X1: case MODE_OPN_MZ: renderPsg(out, n); break;
    default: break;
    }
    // 3. Output samples still owed from the previous call (an internal sample can span two calls).
    while (D(G_LEFTOVER_COUNT) != 0) {
        --D(G_LEFTOVER_COUNT);
        --n;
        out[0] = add32(out[0], D(G_LEFTOVER_L));
        out[1] = add32(out[1], D(G_LEFTOVER_R));
        out += 2;
    }
    if (n == 0) return;

    int chipSamples = 0, sumLeft = 0, sumRight = 0;      // chip samples accumulated for the current output sample
    for (;;) {
        // 4. Produce chip samples until enough time has passed for one output sample.
        do {
            if ((D(G_SAMPLE_COUNT) & 7) == 0) {          // every 8 chip samples: LFO step and per-channel LFO factors
                updateLfo();
                for (int ch = 0; ch < 8; ch++) {
                    int pmFactor = 0, amValue = 0;
                    if (D(G_MODE) == MODE_OPM) {
                        if (D(chIdx(ch, CH_PM_DEPTH)) != 0)
                            pmFactor = rodata(kOpmPitchModTable, mul32(D(chIdx(ch, CH_PM_DEPTH)), D(G_PM_LFO)) >> 7);
                        if (D(chIdx(ch, CH_AM_DEPTH)) != 0) amValue = (D(chIdx(ch, CH_AM_DEPTH)) * D(G_AM_LFO)) >> 8;
                    } else {
                        pmFactor = ((D(chIdx(ch, CH_PM_DEPTH)) * D(G_PM_LFO)) >> 7) + 2048;
                        amValue = (D(chIdx(ch, CH_AM_DEPTH)) * D(G_AM_LFO)) >> 8;
                    }
                    D(chIdx(ch, CH_PM_FACTOR)) = pmFactor;
                    D(chIdx(ch, CH_AM_FACTOR)) = amValue;
                }
            }
            // Operators
            uint32_t active = (uint32_t)D(G_OPERATOR_MASK);
            for (int op = 0; op < 32; op++, active >>= 1) {
                int o = opIdx(op, 0);
                int chb = D(o + OP_CHANNEL);              // dword index of this operator's channel record
                if (active & 1) {
                    if (D(o + OP_ENV_STATE) == 4) {
                        D(o + OP_OUTPUT) = 0;            // off
                    } else {
                        // Envelope: when the counter runs out, take a step according to the current state.
                        if (--D(o + OP_ENV_COUNTER) <= 0) {
                            int step = D(o + D(o + OP_ENV_STATE) + OP_RATE_ATTACK) & 0xFFFFF;
                            switch (D(o + OP_ENV_STATE)) {
                            case 0:                      // attack: exponential approach to 0 attenuation
                                D(o + OP_ENV_ACC) >>= 10;
                                D(o + OP_ENV_ACC) += mul32(step, -D(o + OP_ENV_ACC)) >> 20;
                                D(o + OP_ENV_ACC) = (int32_t)((uint32_t)D(o + OP_ENV_ACC) << 10);
                                if (D(o + OP_ENV_ACC) <= 0) {
                                    D(o + OP_ENV_STATE) = 1;
                                    D(o + OP_ENV_ACC) = 0;
                                    if (D(o + OP_SUSTAIN_LEVEL) == 0) D(o + OP_ENV_STATE) = 2;
                                }
                                break;
                            case 1:                      // decay 1: until the sustain level is reached
                                D(o + OP_ENV_ACC) = add32(D(o + OP_ENV_ACC), step);
                                if (D(o + OP_ENV_ACC) >= D(o + OP_SUSTAIN_LEVEL)) {
                                    D(o + OP_ENV_STATE) = 2;
                                    D(o + OP_ENV_ACC) = D(o + OP_SUSTAIN_LEVEL);
                                }
                                break;
                            case 2: case 3:              // decay 2 / release: keep attenuating
                                D(o + OP_ENV_ACC) = add32(D(o + OP_ENV_ACC), step);
                                break;
                            case 4:
                                D(o + OP_ENV_ACC) = 4190208;
                                break;
                            default: break;
                            }
                            if (D(o + OP_ENV_ACC) > 4190208) { D(o + OP_ENV_STATE) = 4; D(o + OP_ENV_ACC) = 4190208; }
                            D(o + OP_ENV_LEVEL) = D(o + OP_ENV_ACC) / 4096;
                            D(o + OP_ENV_COUNTER) = 1 << (((uint32_t)D(o + D(o + OP_ENV_STATE) + OP_RATE_ATTACK) >> 24) & 0xFF);
                        }
                        // Total attenuation: tremolo (if enabled for this operator) + total level + envelope.
                        int atten = (D(o + OP_AM_MASK) & D(chb + CH_AM_FACTOR)) + D(o + OP_TOTAL_LEVEL) + D(o + OP_ENV_LEVEL);
                        if (atten > 1023) atten = 1023;
                        // Phase advance (scaled by the channel's vibrato factor, 2048 = x1).
                        D(o + OP_PHASE) = add32(D(o + OP_PHASE), (int32_t)((uint32_t)mul32(D(chb + CH_PM_FACTOR), D(o + OP_PHASE_STEP)) >> 10));
                        // Output = sine(phase + modulators) attenuated: the low 6 bits of `atten` pick a mantissa
                        // (127..64), the rest is a right shift (each 64 units = 6 dB).
                        int index = (W(D(o + OP_MOD_INPUT_2)) + W(D(o + OP_MOD_INPUT_1)) + (uint16_t)((uint32_t)D(o + OP_PHASE) >> 13)) & 0x3FF;
                        D(o + OP_OUTPUT) = (kSineTable[index] * ((atten & 0x3F) ^ 0x7F)) >> ((atten >> 6) + 5);
                    }
                } else {
                    D(o + OP_OUTPUT) = 0;
                }
            }
            // OPM noise: replaces the output of operator 32 (channel 8, slot 4).
            if (D(G_OPM_NOISE_STEP) != 0) {
                const int noiseOp = 31;
                int amp = 1023 - (D(opIdx(noiseOp, OP_TOTAL_LEVEL)) + D(opIdx(noiseOp, OP_ENV_LEVEL)));
                if (amp < 0) amp = 0;
                D(opIdx(noiseOp, OP_OUTPUT)) = (amp * g_noise[(uint32_t)D(G_OPM_NOISE_PHASE) >> 16 & 0x3FFF]) >> 10;
                D(G_OPM_NOISE_PHASE) = add32(D(G_OPM_NOISE_PHASE), D(G_OPM_NOISE_STEP));
            }
            // Channels: sum the carriers, update operator-1 feedback, split into left/right by the pan masks.
            for (int ch = 0; ch < 8; ch++) {
                int op0Out = D(opIdx(4 * ch, OP_OUTPUT));
                D(chIdx(ch, CH_FEEDBACK_OUT)) = (op0Out + D(chIdx(ch, CH_FEEDBACK_PREV))) >> D(chIdx(ch, CH_FEEDBACK_SHIFT));
                D(chIdx(ch, CH_FEEDBACK_PREV)) = op0Out;
                int mix = d[D(chIdx(ch, CH_OUTPUT_PTR + 3))] + d[D(chIdx(ch, CH_OUTPUT_PTR + 2))]
                        + d[D(chIdx(ch, CH_OUTPUT_PTR + 1))] + d[D(chIdx(ch, CH_OUTPUT_PTR + 0))];
                sumLeft  += mix & D(chIdx(ch, CH_MASK_LEFT));
                sumRight += mix & D(chIdx(ch, CH_MASK_RIGHT));
            }
            ++chipSamples;
            ++D(G_SAMPLE_COUNT);
            D(G_RESAMPLE_ACC) = add32(D(G_RESAMPLE_ACC), D(G_RESAMPLE_STEP));
        } while ((D(G_RESAMPLE_ACC) >> 14) == 0);

        // 5. Average the chip samples into output sample(s), apply the output gain.
        int outputsDue = D(G_RESAMPLE_ACC) >> 14;
        D(G_RESAMPLE_ACC) &= 0x3FFF;
        int left  = (sumLeft  / chipSamples * D(G_OUTPUT_GAIN)) >> 10;
        int right = (sumRight / chipSamples * D(G_OUTPUT_GAIN)) >> 10;
        bool restart = false;
        for (;;) {
            out[0] = add32(out[0], left);
            out[1] = add32(out[1], right);
            out += 2;
            if (--n <= 0) break;
            if (--outputsDue == 0) { chipSamples = 0; sumRight = 0; sumLeft = 0; restart = true; break; }
        }
        if (!restart) {                                  // remember samples owed to the next call
            D(G_LEFTOVER_COUNT) = outputsDue - 1;
            D(G_LEFTOVER_L) = left;
            D(G_LEFTOVER_R) = right;
            return;
        }
    }
}

}  // namespace opnmml
