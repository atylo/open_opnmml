// Faithful port of the wuopn.dll sequencer. IDA names are given next to each function.
#include "seq.hpp"
#include "tables.inc"
#include <cstring>

namespace opnmml {

static const uint8_t* tonePtr(const Song* s, int n) { return s->tones[n & 0x7F].b; }   // sub_1001BDF0

// ---------------------------------------------------------------- SeqCh

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

// sub_1001E350 : program pitch + volume (and PSG mixer / noise) for the channel
void SeqCh::setNote(int a2, int a3, int a4) {
    const Song* S = seq->song;
    int m = chip->mode();
    if (m != 0 && m != 5) {
        if (index > 7) {                                    // X1: PSG voices (OPN register file)
            int per = kPsgPeriod[a3 <= 192 ? 0 : a3 - 192];
            int n = index - 8;
            if ((a2 >> 5) != 0) {
                int v10 = ((a2 & 0x40) != 0) | ((a2 & 0x20) != 0 ? 8 : 0);
                int cur = chip->read(7);
                chip->writeOpn(7, ~(v10 << n) & ((9 << n) | cur));
                chip->writeOpn(6, a2 & 0x1F);
            } else {
                int cur = chip->read(7);
                chip->writeOpn(7, ~(1 << n) & ((9 << n) | cur));
            }
            chip->writeOpn(2 * n, per);
            chip->writeOpn(2 * n + 1, per >> 8);
            if (state == 0) a4 = 0;
            chip->writeOpn(n + 8, a4);
        } else {                                            // X1: OPM FM voices
            int v32 = a3 - 47; if (v32 < 0) v32 = 0;
            int oct = v32 / 192, semi = (v32 % 192) >> 4;
            static const int kc[12] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14};
            const uint8_t* t = tonePtr(S, a2 & 0x7F);
            for (int i = 0; i < 4; i++) {
                int tl = t[6 * i + 1];
                if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - a4); if (tl > 0x7F) tl = 127; }
                chip->write(index + 8 * i + 96, tl);
            }
            chip->write(index + 32, (pan << 6) | t[24]);
            chip->write(index + 40, kc[semi] | (16 * oct));
            chip->write(index + 48, 16 * (v32 & 0xF));
        }
    } else if (index >= 3) {                                // PC-88: PSG voices
        int per = kPsgPeriod[a3 <= 192 ? 0 : a3 - 192];
        int n = index - 3;
        if ((a2 >> 5) != 0) {
            int v21 = ((a2 & 0x40) != 0) | ((a2 & 0x20) != 0 ? 8 : 0);
            int cur = chip->read(7);
            chip->write(7, ~(v21 << n) & ((9 << n) | cur));
            chip->write(6, a2 & 0x1F);
        } else {
            int cur = chip->read(7);
            chip->write(7, ~(1 << n) & ((9 << n) | cur));
        }
        chip->write(2 * n, per);
        chip->write(2 * n + 1, per >> 8);
        if (state == 0) a4 = 0;
        chip->write(n + 8, a4);
    } else {                                                // PC-88: OPN FM voices
        int oct = a3 / 192, fr = a3 % 192;
        int fn = kFnum[fr] | (oct << 11);
        const uint8_t* t = tonePtr(S, a2 & 0x7F);
        for (int j = 0; j < 4; j++) {
            int tl = t[6 * j + 1];
            if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - a4); if (tl > 0x7F) tl = 127; }
            chip->write(index + 4 * j + 64, tl);
        }
        chip->write((index - 92) & 0xFF, fn >> 8);
        chip->write((index - 96) & 0xFF, fn);
    }
}

// sub_1001DAF0 : execute one control opcode.  p enters pointing at the opcode.
void SeqCh::dispatch(uint8_t op, const uint8_t*& p) {
    uint8_t v6 = *++p;
    ++p;
    auto rd = [&p]() -> uint8_t { return *p++; };
    switch (op) {
    case 0x80: vol = v6; break;
    case 0x81: detune = (int16_t)(rd() | (v6 << 8)); break;
    case 0x82: tone = v6; break;
    case 0x83: tie = 1; --p; break;
    case 0x87: seq->setTempo(v6); break;
    case 0x88: porta = (int8_t)v6; break;
    case 0x8F: { int v5 = rd(); chip->write(v6, v5); break; }
    case 0x90: {
        int16_t a = (int16_t)(rd() | (v6 << 8));
        bend = (int32_t)a << 6;
        uint8_t h = rd();
        bendRate = (int16_t)(rd() | (h << 8));
        uint8_t h2 = rd();
        bendLen = (uint16_t)(rd() | (h2 << 8));
        break;
    }
    case 0x94: rStep = v6; rDec = rd(); rCnt = 0; break;
    case 0x95: seq->setFade(v6); break;
    case 0x96: vol += v6; if (vol > 15) vol = 15; break;
    case 0x97: vol -= v6; if (vol & 0x80) vol = 0; break;
    case 0x98: pan = 1; --p; break;
    case 0x99: pan = 2; --p; break;
    case 0x9A: pan = 3; --p; break;
    case 0x9B: pan = 4; --p; break;
    case 0x9C: pan = 5; --p; break;
    case 0x9F: case 0xA6: case 0xF0: case 0xF1: case 0xF2: case 0xF3:
        break;                                              // two-byte no-ops
    case 0xA0: case 0xA2:
        lfoPType = v6;
        lfoPPeriod = rd();
        { int8_t d = (int8_t)rd(); lfoPDepth = (op == 0xA2) ? (int16_t)(4 * d) : (int16_t)d; }
        lfoPDelay = rd();
        lfoPDcnt = lfoPDelay; lfoPCnt = lfoPPeriod; lfoPPhase = 0;
        break;
    case 0xA1: case 0xA3:
        lfoAType = v6;
        lfoAPeriod = rd();
        { int8_t d = (int8_t)rd(); lfoADepth = (op == 0xA3) ? (int16_t)(4 * d) : (int16_t)d; }
        lfoADelay = rd();
        lfoADcnt = lfoADelay; lfoACnt = lfoAPeriod; lfoAPhase = 0;
        break;
    case 0xA5:
        envNo = (int8_t)v6; envPeriod = rd(); envCnt = envPeriod; envIdx = 0;
        break;
    case 0xC0:
        if (depth < 5) { cnt[depth] = v6; ++depth; }
        break;
    case 0xC1: {
        uint16_t v23 = (uint16_t)(rd() | (v6 << 8));
        if (depth != 0 && cnt[depth - 1] == 1) { p += v23; --depth; }
        break;
    }
    case 0xC2: {
        uint16_t v24 = (uint16_t)(*p | (v6 << 8));
        ++p;
        if (depth != 0) {
            if (--cnt[depth - 1] != 0) p -= v24;
            else --depth;
        }
        break;
    }
    case 0xCF: loopOff = (int32_t)((--p) - base); break;
    default: --p; break;                                     // unknown: one-byte skip
    }
}

// sub_1001EAF0 : one musical tick for this channel
bool SeqCh::tick(bool a2) {
    const Song* S = seq->song;
    bool v32 = true;
    int v29 = 0;
    int16_t v30 = 0;
    int v23 = 0;
    const uint8_t* p = nullptr;
    if (base == nullptr || state == 0) v32 = false;
    if (v32) {
        p = base + off;
        tie = 0;
        while (len == 0) {
            uint8_t op = *p;
            if (op == 0) {
                ++passes;
                if (loopOff == 0) {
                    passes = 0xFF; state = 0;
                    off = (int32_t)(p - base);
                    v29 |= 2; v32 = false;
                    if (owns && a2) { setNote(tone, curPitch, 15); keyOff(); }
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
                        else { state = 4; v29 |= 2; }
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
                    v29 |= 3;
                }
            }
        }
        off = (int32_t)(p - base);
    }
    if (v32) {
        v29 |= 4;
        --len;
        v30 = (int16_t)(detune + pitch16);
        if (lfoPType != 0) {
            if (lfoPDcnt != 0) --lfoPDcnt;
            else if (lfoPCnt != 0) --lfoPCnt;
            else { lfoPCnt = lfoPPeriod; ++lfoPPhase; }
            switch (lfoPType) {
            case 1: if (lfoPPhase & 1) v30 += lfoPDepth; break;
            case 2: {
                int v2;
                if (lfoPPhase & 1) v2 = lfoPCnt * lfoPDepth / lfoPPeriod;
                else               v2 = (lfoPPeriod - lfoPCnt) * lfoPDepth / lfoPPeriod;
                v30 += v2;
                break;
            }
            case 4: {
                int ph = lfoPPhase & 3;
                if (ph == 1) v30 += lfoPDepth; else if (ph == 3) v30 -= lfoPDepth;
                break;
            }
            default: break;
            }
        }
        if (porta != 0 && ((porta * curPitch + (8 - porta) * v30) >> 3) != curPitch)
            v30 = (int16_t)((porta * curPitch + (8 - porta) * v30) >> 3);
        if (bendLen != 0) {
            --bendLen;
            bend -= bendRate;
            v30 += (int16_t)(bend >> 6);
        }
        if (v30 >= 0) { if (v30 > 1536) v30 = 1536; } else v30 = 0;
        curPitch = v30;

        int16_t v24 = (int16_t)vol;
        if (state == 4) {
            if (index >= 3) v24 = 0;
        } else {
            if (state == 3) {
                if (rCnt != 0) --rCnt;
                else { rCnt = rStep; rAcc += rDec; }
                v24 -= (int8_t)rAcc;
                if (v24 < 0) { state = 4; v29 |= 2; }
            }
            if (lfoAType != 0) {
                if (lfoADelay != 0) --lfoADelay;             // the original decrements the stored delay itself
                else if (lfoACnt != 0) --lfoACnt;
                else { lfoACnt = lfoAPeriod; ++lfoAPhase; }
                switch (lfoAType) {
                case 1: if (lfoAPhase & 1) v24 += lfoADepth; break;
                case 2: {
                    int v3;
                    if (lfoAPhase & 1) v3 = lfoACnt * lfoADepth / lfoAPeriod;
                    else               v3 = (lfoAPeriod - lfoACnt) * lfoADepth / lfoAPeriod;
                    v24 += v3;
                    break;
                }
                case 4: {
                    int ph = lfoAPhase & 3;
                    if (ph == 1) v24 += lfoADepth; else if (ph == 3) v24 -= lfoADepth;
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
                v24 += e.v[envIdx];
            }
            if (state == 1) v24 = 0;
        }
        int16_t v25 = (int16_t)(v24 - seq->fadeAtten());
        v23 = v25 - seq->masterAtten;
        if (v23 >= 0) { if (v23 > 15) v23 = 15; } else v23 = 0;
    }
    if (a2) {
        if (owns) {
            if (v29 & 2) { keyOff(); v29 &= ~2; }
            if (v29 & 4) { setNote(tone, v30, v23); v29 &= ~4; }
        }
        if (v29 & 1) {
            if (v29 & 2) { keyOff(); v29 &= ~2; }
            if (v29 & 4) setNote(tone, v30, v23);
            if (chip->mode() != 0) {
                if (index <= 7) {                            // OPM operator registers
                    const uint8_t* t = tonePtr(S, tone & 0x7F);
                    for (int i = 0; i < 4; i++) {
                        const uint8_t* c = t + 6 * i;
                        chip->write(index + 8 * i + 64, c[0]);
                        int tl = c[1];
                        if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - v23); if (tl > 0x7F) tl = 127; }
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
                    if (tl & 0x80) { tl = (tl & 0x7F) + 4 * (15 - v23); if (tl > 0x7F) tl = 127; }
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
    return v32;
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
