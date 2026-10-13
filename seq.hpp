// Port of the wuopn.dll sequencer: CSoundSeqCh (sub_1001EAF0 & co) and CSoundSequencer (sub_10022780).
// It consumes the bytecode produced by mmlcomp and drives a chip through register writes.
#pragma once
#include "mmlcomp.hpp"

namespace opnmml {

// What the sequencer needs from the sound chip (COpx4 in the original).
struct IChip {
    virtual ~IChip() {}
    virtual int  mode() const = 0;               // sub_1001A470: 0 PC-88 OPN, 1/4 OPM(X1), 2/3 OPN variants, 5 MZ
    virtual void write(int reg, int val) = 0;    // sub_1001B7A0 (port 0): OPM write for mode 1/4, OPN/PSG write otherwise
    virtual void writeOpn(int reg, int val) = 0; // sub_1001B220 called directly (OPN/PSG register file)
    virtual int  read(int reg) = 0;              // sub_10019DC0 (port 0): shadow of registers 0x00-0x1F
};

class Sequencer;

struct SeqCh {                                   // CSoundSeqCh, 0x68 bytes in the original
    Sequencer* seq = nullptr;                    // +96
    IChip*     chip = nullptr;                   // +100
    int        index = 0;                        // +93
    const uint8_t* base = nullptr;               // +4
    size_t     size = 0;
    bool       owns = false;                     // +8   channel has programmed the chip
    int16_t    state = 0;                        // +10  0 off, 1 mute, 2 gate, 3 release(R), 4 key-off
    uint16_t   len = 0;                          // +12  ticks left in current event
    int32_t    off = 0;                          // +16
    int32_t    loopOff = 0;                      // +20
    int16_t    pitch16 = 0;                      // +24  16 * note
    int16_t    detune = 0;                       // +26
    int16_t    curPitch = 0;                     // +28
    int32_t    vol = 0;                          // +32
    int32_t    bend = 0, bendRate = 0;           // +36 +40
    uint16_t   bendLen = 0;                      // +44
    uint8_t    tone = 1;                         // +46
    uint8_t    cnt[5] = {0};                     // +47..+51 loop counters
    int8_t     depth = 0;                        // +52
    uint8_t    pan = 3;                          // +53
    int8_t     porta = 0;                        // +54
    int16_t    lfoPPhase = 0, lfoAPhase = 0, envIdx = 0;   // +56 +58 +60
    int16_t    lfoPPeriod = 0, lfoPDelay = 0;    // +62 +64
    int16_t    lfoAPeriod = 0, lfoADelay = 0;    // +66 +68
    int16_t    lfoPCnt = 0, lfoPDcnt = 0;        // +70 +72
    int16_t    lfoACnt = 0, lfoADcnt = 0;        // +74 +76
    uint8_t    envPeriod = 0, envCnt = 0;        // +78 +79
    uint8_t    rStep = 0, rDec = 0, rCnt = 0, rAcc = 0;    // +80 +81 +82 +83
    uint8_t    lfoPType = 0, lfoAType = 0;       // +84 +85
    int16_t    lfoPDepth = 0, lfoADepth = 0;     // +86 +88
    int8_t     envNo = 0;                        // +90
    uint8_t    passes = 0;                       // +91
    uint8_t    tie = 0;                          // +92

    void reset();                                // sub_1001BF40
    void setData(const uint8_t* p, size_t n, int offset) { base = p; size = n; off = offset; }   // sub_1001BF20
    bool active() const { return state != 0; }   // sub_1001C030
    void stop();                                 // sub_1001F890
    bool tick(bool a2);                          // sub_1001EAF0, returns "still running"

    void dispatch(uint8_t op, const uint8_t*& p);// sub_1001DAF0
    void setNote(int toneNo, int pitch, int volume);   // sub_1001E350
    void keyOn();                                // sub_1001EA10
    void keyOff();                               // sub_1001EA80
};

class Sequencer {                                // CSoundSequencer
public:
    Sequencer(IChip* chip);
    void load(const Song* song);                 // song data (tones, envelopes, bytecode)
    void start();                                // sub_1001C620(0) : start song 0 on all channels
    void tick(unsigned& mask);                   // sub_10022780 : one 1 kHz step
    void setTempo(int bpm);                      // sub_1001BE50
    void setFade(int v);                         // sub_1001BD90
    int  fadeAtten() const { return fadeAcc >> 8; }      // sub_1001BED0
    int  masterAtten = 0;                        // +4182 (sub_1001BEF0)
    const Song* song = nullptr;
    SeqCh ch[11];
    int  tempo = 120;
private:
    IChip* chip;
    int  counter = 0, fadeAcc = 0, fadeStep = 0;  // +4176 +4180 +4181
    int  timerAcc = 0, timerStep = 0;             // +4177 +4178
    unsigned lastMask = 0;                        // +4183
};

// sub_10022E80 : advance by one millisecond (sequencer step + key-off housekeeping for idle channels)
void step1ms(Sequencer** seqs, int n, IChip* chip, unsigned muteMask);

}  // namespace opnmml
