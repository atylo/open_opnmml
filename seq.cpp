// ================================================================================================================
// Sequencer - port of wuopn.dll's bytecode player (classes CSoundSeqCh and CSoundSequencer).
//
// The MML compiler (mmlcomp.cpp) turns each channel of a song into a byte stream.  This file plays those streams:
//   * Sequencer::tick() runs once per MUSICAL tick (48 per quarter note); the mixer calls step1ms() every millisecond
//     and a timer inside decides when a musical tick is due (so tempo changes simply change the timer step);
//   * SeqCh::tick() advances one channel: it reads commands until it reaches a note or rest, counts the note's length
//     down, runs the pitch/volume modulations (LFOs, envelope, bend, portamento, release) and finally writes the chip
//     registers that make the note sound.
//
// Bytecode (see mmldump.cpp for a readable listing):
//   0x01-0x7E  NOTE n, length      0x7F REST, length      0x00 end of channel
//   0x80 VOLUME  0x81 DETUNE  0x82 TONE  0x83 TIE  0x87 TEMPO  0x88 PORTAMENTO  0x8F REGISTER_WRITE  0x90 PITCH_BEND
//   0x94 RELEASE_FADE  0x95 FADE_OUT  0x96/0x97 VOLUME_UP/DOWN  0x98-0x9A PAN  0xA0-0xA3 LFOs  0xA5 VOLUME_ENVELOPE
//   0xC0 LOOP_BEGIN  0xC1 LOOP_BREAK  0xC2 LOOP_END  0xCF SONG_LOOP_POINT
//
// Function names in comments (sub_XXXXXXXX) are the Hex-Rays names in wuopn.cpp.
// ================================================================================================================
#include "seq.hpp"
#include "tables.inc"
#include <cstring>

namespace opnmml {

static const uint8_t* tonePtr(const Song* s, int n) { return s->tones[n & 0x7F].b; }   // sub_1001BDF0

// ---------------------------------------------------------------- SeqCh

namespace {
// Events a channel collects while processing one tick, acted on at the end of SeqCh::tick().
enum TickFlags {
    FLAG_NEW_NOTE = 1,   // a new note starts: write the tone's operator registers and key on
    FLAG_KEY_OFF  = 2,   // key off first (needed before retriggering, and when a note or rest ends the sound)
    FLAG_UPDATE   = 4    // pitch and volume have to be (re)written to the chip
};
}  // namespace

void SeqCh::reset() {                      // sub_1001BF40
    len = 0; passes = 0; base = nullptr; size = 0; curPitch = 0; pitch16 = 0; vol = 0;
    bendLen = 0; bendRate = 0; bend = 0; depth = 0; detune = 0; pan = 3; porta = 0; tone = 1;
    owns = false; lfoAType = 0; lfoPType = 0; envNo = 0; rAcc = 0; rDec = 0; rCnt = 0; rStep = 0;
    envCnt = 0; envPeriod = 0; off = 0; loopOff = 0; state = 0;
}

void SeqCh::stop() { state = 0; keyOff(); }                 // sub_1001F890 -> sub_1001EA80

void SeqCh::keyOn() {                                       // sub_1001EA10
    if (chip->mode() != 0) { if (index <= 7) chip->write(8, index + 120); }
    else if (index < 3)    chip->write(0x28, index + 240);
}

void SeqCh::keyOff() {                                      // sub_1001EA80
    if (chip->mode() != 0) { if (index <= 7) chip->write(8, index); }
    else if (index < 3)    chip->write(0x28, index);
}

// Program pitch and volume of this channel into the chip.                                       [sub_1001E350]
//   toneNo : tone/instrument number (for PSG voices it also encodes the mixer: bit 6 = tone on, bit 5 = noise on,
//            bits 0-4 = noise period)
//   pitch  : 16 steps per semitone, 192 per octave (0 = lowest note)
//   volume : 0..15 (15 = loudest); FM carriers get 4 total-level steps of attenuation per volume step below 15
// What is written depends on the chip: OPM channel 0-7 -> TL registers + key code + key fraction;
// OPM channel 8-10 -> PSG voice; OPN channel 0-2 -> TL registers + F-number; OPN channel 3-5 -> PSG voice.
void SeqCh::setNote(int toneNo, int pitch, int volume) {
    const Song* S = seq->song;
    int m = chip->mode();
    if (m != 0 && m != 5) {
        if (index > 7) {                                    // X1: PSG voices (OPN register file)
            int period = kPsgPeriodTable[pitch <= 192 ? 0 : pitch - 192];
            int n = index - 8;
            if ((toneNo >> 5) != 0) {
                int mixerBits = ((toneNo & 0x40) != 0) | ((toneNo & 0x20) != 0 ? 8 : 0);
                int mixerReg = chip->read(7);
                chip->writeOpn(7, ~(mixerBits << n) & ((9 << n) | mixerReg));
                chip->writeOpn(6, toneNo & 0x1F);
            } else {
                int mixerReg = chip->read(7);
                chip->writeOpn(7, ~(1 << n) & ((9 << n) | mixerReg));
            }
            chip->writeOpn(2 * n, period);
            chip->writeOpn(2 * n + 1, period >> 8);
            if (state == 0) volume = 0;
            chip->writeOpn(n + 8, volume);
        } else {                                            // X1: OPM FM voices
            int keyPos = pitch - 47; if (keyPos < 0) keyPos = 0;
            int oct = keyPos / 192, semi = (keyPos % 192) >> 4;
            static const int kc[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
            const uint8_t* t = tonePtr(S, toneNo & 0x7F);
            for (int i = 0; i < 4; i++) {
                int tl = t[6 * i + 1];
                if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - volume); if (tl > 0x7F) tl = 127; }
                chip->write(index + 8 * i + 96, tl);
            }
            chip->write(index + 32, (pan << 6) | t[24]);
            chip->write(index + 40, kc[semi] | (16 * oct));
            chip->write(index + 48, 16 * (keyPos & 0xF));
        }
    } else if (index >= 3) {                                // PC-88: PSG voices
        int period = kPsgPeriodTable[pitch <= 192 ? 0 : pitch - 192];
        int n = index - 3;
        if ((toneNo >> 5) != 0) {
            int mixerBits = ((toneNo & 0x40) != 0) | ((toneNo & 0x20) != 0 ? 8 : 0);
            int mixerReg = chip->read(7);
            chip->write(7, ~(mixerBits << n) & ((9 << n) | mixerReg));
            chip->write(6, toneNo & 0x1F);
        } else {
            int mixerReg = chip->read(7);
            chip->write(7, ~(1 << n) & ((9 << n) | mixerReg));
        }
        chip->write(2 * n, period);
        chip->write(2 * n + 1, period >> 8);
        if (state == 0) volume = 0;
        chip->write(n + 8, volume);
    } else {                                                // PC-88: OPN FM voices
        int oct = pitch / 192, fr = pitch % 192;
        int fn = kOpnFnumTable[fr] | (oct << 11);
        const uint8_t* t = tonePtr(S, toneNo & 0x7F);
        for (int j = 0; j < 4; j++) {
            int tl = t[6 * j + 1];
            if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - volume); if (tl > 0x7F) tl = 127; }
            chip->write(index + 4 * j + 64, tl);
        }
        chip->write((index - 92) & 0xFF, fn >> 8);
        chip->write((index - 96) & 0xFF, fn);
    }
}

// Execute one control command (opcode >= 0x80).  p enters pointing at the opcode and leaves pointing at the next
// command.                                                                                      [sub_1001DAF0]
void SeqCh::dispatch(uint8_t op, const uint8_t*& p) {
    uint8_t arg = *++p;
    ++p;
    auto rd = [&p]() -> uint8_t { return *p++; };
    switch (op) {
    case 0x80: vol = arg; break;
    case 0x81: detune = (int16_t)(rd() | (arg << 8)); break;
    case 0x82: tone = arg; break;
    case 0x83: tie = 1; --p; break;
    case 0x87: seq->setTempo(arg); break;
    case 0x88: porta = (int8_t)arg; break;
    case 0x8F: { int value = rd(); chip->write(arg, value); break; }
    case 0x90: {
        int16_t a = (int16_t)(rd() | (arg << 8));
        bend = (int32_t)a << 6;
        uint8_t h = rd();
        bendRate = (int16_t)(rd() | (h << 8));
        uint8_t h2 = rd();
        bendLen = (uint16_t)(rd() | (h2 << 8));
        break;
    }
    case 0x94: rStep = arg; rDec = rd(); rCnt = 0; break;
    case 0x95: seq->setFade(arg); break;
    case 0x96: vol += arg; if (vol > 15) vol = 15; break;
    case 0x97: vol -= arg; if (vol & 0x80) vol = 0; break;
    case 0x98: pan = 1; --p; break;
    case 0x99: pan = 2; --p; break;
    case 0x9A: pan = 3; --p; break;
    case 0x9B: pan = 4; --p; break;
    case 0x9C: pan = 5; --p; break;
    case 0x9F: case 0xA6: case 0xF0: case 0xF1: case 0xF2: case 0xF3:
        break;                                              // two-byte no-ops
    case 0xA0: case 0xA2:
        lfoPType = arg;
        lfoPPeriod = rd();
        { int8_t d = (int8_t)rd(); lfoPDepth = (op == 0xA2) ? (int16_t)(4 * d) : (int16_t)d; }
        lfoPDelay = rd();
        lfoPDcnt = lfoPDelay; lfoPCnt = lfoPPeriod; lfoPPhase = 0;
        break;
    case 0xA1: case 0xA3:
        lfoAType = arg;
        lfoAPeriod = rd();
        { int8_t d = (int8_t)rd(); lfoADepth = (op == 0xA3) ? (int16_t)(4 * d) : (int16_t)d; }
        lfoADelay = rd();
        lfoADcnt = lfoADelay; lfoACnt = lfoAPeriod; lfoAPhase = 0;
        break;
    case 0xA5:
        envNo = (int8_t)arg; envPeriod = rd(); envCnt = envPeriod; envIdx = 0;
        break;
    case 0xC0:
        if (depth < 5) { cnt[depth] = arg; ++depth; }
        break;
    case 0xC1: {
        uint16_t skip = (uint16_t)(rd() | (arg << 8));
        if (depth != 0 && cnt[depth - 1] == 1) { p += skip; --depth; }
        break;
    }
    case 0xC2: {
        uint16_t back = (uint16_t)(*p | (arg << 8));
        ++p;
        if (depth != 0) {
            if (--cnt[depth - 1] != 0) p -= back;
            else --depth;
        }
        break;
    }
    case 0xCF: loopOff = (int32_t)((--p) - base); break;
    default: --p; break;                                     // unknown: one-byte skip
    }
}

// Advance this channel by one musical tick (1/48 of a quarter note).                            [sub_1001EAF0]
//   mayWrite : false when a higher-priority sequencer is currently driving the chip; the channel still advances but
//              does not touch the registers.
// Returns true while the channel is still playing.
bool SeqCh::tick(bool mayWrite) {
    const Song* S = seq->song;
    bool running = true;
    int flags = 0;
    int16_t pitch = 0;
    int volume = 0;
    const uint8_t* p = nullptr;
    if (base == nullptr || state == 0) running = false;
    if (running) {
        p = base + off;
        tie = 0;
        while (len == 0) {
            uint8_t op = *p;
            if (op == 0) {
                ++passes;
                if (loopOff == 0) {
                    passes = 0xFF; state = 0;
                    off = (int32_t)(p - base);
                    flags |= FLAG_KEY_OFF; running = false;
                    if (owns && mayWrite) { setNote(tone, curPitch, 15); keyOff(); }
                    break;
                }
                if (passes > 0x7F) passes = 127;
                off = loopOff;
                p = base + off;
                op = *p;
            }
            if (op >= 0x7F) {
                if (op == 127) {
                    uint8_t b = *++p; ++p;
                    if (b < 0x80) len = b; else { len = (uint16_t)(*p + ((b & 0x7F) << 7)); ++p; }
                    if (state != 4 && state != 3) {
                        rCnt = 0;
                        if (rStep != 0) state = 3;
                        else { state = 4; flags |= FLAG_KEY_OFF; }
                    }
                } else {
                    dispatch(op, p);
                }
            } else {
                pitch16 = (int16_t)(16 * op);
                uint8_t b = *++p; ++p;
                if (b < 0x80) len = b; else { len = (uint16_t)(*p + ((b & 0x7F) << 7)); ++p; }
                state = 2;
                if (tie == 0) {
                    envIdx = 0; lfoAPhase = 0; lfoPPhase = 0;
                    lfoPCnt = lfoPPeriod; lfoPDcnt = lfoPDelay;
                    lfoACnt = lfoAPeriod; lfoADcnt = lfoADelay;
                    envCnt = envPeriod;
                    rAcc = 0;
                    flags |= FLAG_NEW_NOTE | FLAG_KEY_OFF;
                }
            }
        }
        off = (int32_t)(p - base);
    }
    if (running) {
        flags |= FLAG_UPDATE;
        --len;
        pitch = (int16_t)(detune + pitch16);
        if (lfoPType != 0) {
            if (lfoPDcnt != 0) --lfoPDcnt;
            else if (lfoPCnt != 0) --lfoPCnt;
            else { lfoPCnt = lfoPPeriod; ++lfoPPhase; }
            switch (lfoPType) {
            case 1: if (lfoPPhase & 1) pitch += lfoPDepth; break;
            case 2: {
                int lfoDelta;
                if (lfoPPhase & 1) lfoDelta = lfoPCnt * lfoPDepth / lfoPPeriod;
                else               lfoDelta = (lfoPPeriod - lfoPCnt) * lfoPDepth / lfoPPeriod;
                pitch += lfoDelta;
                break;
            }
            case 4: {
                int ph = lfoPPhase & 3;
                if (ph == 1) pitch += lfoPDepth; else if (ph == 3) pitch -= lfoPDepth;
                break;
            }
            default: break;
            }
        }
        if (porta != 0 && ((porta * curPitch + (8 - porta) * pitch) >> 3) != curPitch)
            pitch = (int16_t)((porta * curPitch + (8 - porta) * pitch) >> 3);
        if (bendLen != 0) {
            --bendLen;
            bend -= bendRate;
            pitch += (int16_t)(bend >> 6);
        }
        if (pitch >= 0) { if (pitch > 1536) pitch = 1536; } else pitch = 0;
        curPitch = pitch;

        int16_t level = (int16_t)vol;
        if (state == 4) {
            if (index >= 3) level = 0;
        } else {
            if (state == 3) {
                if (rCnt != 0) --rCnt;
                else { rCnt = rStep; rAcc += rDec; }
                level -= (int8_t)rAcc;
                if (level < 0) { state = 4; flags |= FLAG_KEY_OFF; }
            }
            if (lfoAType != 0) {
                if (lfoADelay != 0) --lfoADelay;             // the original decrements the stored delay itself
                else if (lfoACnt != 0) --lfoACnt;
                else { lfoACnt = lfoAPeriod; ++lfoAPhase; }
                switch (lfoAType) {
                case 1: if (lfoAPhase & 1) level += lfoADepth; break;
                case 2: {
                    int lfoDelta;
                    if (lfoAPhase & 1) lfoDelta = lfoACnt * lfoADepth / lfoAPeriod;
                    else               lfoDelta = (lfoAPeriod - lfoACnt) * lfoADepth / lfoAPeriod;
                    level += lfoDelta;
                    break;
                }
                case 4: {
                    int ph = lfoAPhase & 3;
                    if (ph == 1) level += lfoADepth; else if (ph == 3) level -= lfoADepth;
                    break;
                }
                default: break;
                }
            }
            if (envNo != 0) {
                const Envelope& e = S->envs[envNo & 63];
                if (envCnt != 0) --envCnt;
                else {
                    envCnt = envPeriod;
                    if (++envIdx == e.count) envIdx = (int16_t)(int8_t)e.loop;
                }
                level += e.v[envIdx];
            }
            if (state == 1) level = 0;
        }
        int16_t levelAfterFade = (int16_t)(level - seq->fadeAtten());
        volume = levelAfterFade - seq->masterAtten;
        if (volume >= 0) { if (volume > 15) volume = 15; } else volume = 0;
    }
    if (mayWrite) {
        if (owns) {
            if (flags & FLAG_KEY_OFF) { keyOff(); flags &= ~FLAG_KEY_OFF; }
            if (flags & FLAG_UPDATE) { setNote(tone, pitch, volume); flags &= ~FLAG_UPDATE; }
        }
        if (flags & FLAG_NEW_NOTE) {
            if (flags & FLAG_KEY_OFF) { keyOff(); flags &= ~FLAG_KEY_OFF; }
            if (flags & FLAG_UPDATE) setNote(tone, pitch, volume);
            if (chip->mode() != 0) {
                if (index <= 7) {                            // OPM operator registers
                    const uint8_t* t = tonePtr(S, tone & 0x7F);
                    for (int i = 0; i < 4; i++) {
                        const uint8_t* c = t + 6 * i;
                        chip->write(index + 8 * i + 64, c[0]);
                        int tl = c[1];
                        if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - volume); if (tl > 0x7F) tl = 127; }
                        chip->write(index + 8 * i + 96, tl);
                        chip->write(index + 8 * i + 0x80, c[2]);
                        chip->write((index + 8 * i - 96) & 0xFF, c[3]);
                        chip->write((index + 8 * i - 64) & 0xFF, c[4]);
                        chip->write((index + 8 * i - 32) & 0xFF, c[5]);
                    }
                    chip->write(index + 32, (pan << 6) | t[24]);
                    chip->write(index + 56, t[25]);
                    keyOn();
                }
            } else if (index < 3) {                          // OPN operator registers
                const uint8_t* t = tonePtr(S, tone & 0x7F);
                for (int j = 0; j < 4; j++) {
                    const uint8_t* c = t + 6 * j;
                    chip->write(index + 4 * j + 48, c[0]);
                    int tl = c[1];
                    if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - volume); if (tl > 0x7F) tl = 127; }
                    chip->write(index + 4 * j + 64, tl);
                    chip->write(index + 4 * j + 80, c[2]);
                    chip->write(index + 4 * j + 96, c[3]);
                    chip->write(index + 4 * j + 112, c[4]);
                    chip->write(index + 4 * j + 0x80, c[5]);
                }
                chip->write((index - 80) & 0xFF, t[24]);
                chip->write((index - 76) & 0xFF, t[25]);
                keyOn();
            }
            owns = true;
        }
    } else {
        owns = false;
    }
    return running;
}

// ---------------------------------------------------------------- Sequencer

Sequencer::Sequencer(IChip* c) : chip(c) {
    for (int i = 0; i < 11; i++) { ch[i].seq = this; ch[i].chip = c; ch[i].index = i; ch[i].reset(); }
}

void Sequencer::load(const Song* s) { song = s; }

void Sequencer::setTempo(int bpm) {                          // sub_1001BE50
    if (bpm <= 0) return;                                    // original would divide by zero
    tempo = bpm;
    timerStep = 49152 * bpm / 60 / 1000;
}

void Sequencer::setFade(int v) {                             // sub_1001BD90
    if (v < 0) v = 0; else if (v > 255) v = 255;
    fadeStep = 256 - v;
}

void Sequencer::start() {                                    // sub_1001C620(0)
    fadeStep = 0; fadeAcc = 0;
    for (int i = 0; i < 11; i++) {
        ch[i].reset();
        const std::vector<uint8_t>& code = song->ch[i].code;
        if (code.empty()) ch[i].setData(nullptr, 0, 0);
        else              ch[i].setData(code.data(), code.size(), 0);
    }
    counter = 0; fadeStep = 0; fadeAcc = 0; masterAtten = 0; timerAcc = 0; lastMask = 0;
    setTempo(120);
    for (int i = 0; i < 11; i++) if (!song->ch[i].code.empty()) ch[i].state = 4;   // sub_1001C050
}

void Sequencer::tick(unsigned& mask) {                       // sub_10022780
    unsigned v7 = 0;
    if ((++counter & 0xF) == 0) {
        if (fadeAcc > 3825) { for (int i = 0; i < 11; i++) if (ch[i].active()) ch[i].stop(); }
        else fadeAcc += fadeStep;
    }
    timerAcc += timerStep;
    int steps = timerAcc >> 10;
    timerAcc &= 0x3FF;
    if (steps != 0) {
        for (int j = 0; j < 11; j++)
            if (ch[j].active()) v7 |= (unsigned)ch[j].tick((mask & (1u << j)) == 0) << j;
        lastMask = v7;
    } else {
        v7 = lastMask;
    }
    mask |= v7;
}

void step1ms(Sequencer** seqs, int n, IChip* chip, unsigned muteMask) {   // body of sub_10022E80's per-ms loop
    unsigned v8 = 0;
    for (int j = 0; j < n; j++) if (seqs[j]) seqs[j]->tick(v8);
    int mode = chip->mode();
    for (int m = 0; m < 11; m++) {
        if ((v8 & (1u << m)) == 0 || (muteMask & (1u << m))) {
            if (mode != 0 && mode != 5) {
                if (m >= 8) chip->writeOpn(m, 0);
                else        chip->write(8, m);
            } else if (m >= 3) chip->write(m + 5, 0);
            else               chip->write(0x28, m);
        }
    }
}

}  // namespace opnmml
