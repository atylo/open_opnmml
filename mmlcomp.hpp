// Port of the wuopn.dll MML compiler stage (decompiled: sub_10021E90 and callees).
// Input : MML text (Shift-JIS/ASCII bytes).  Output: tones, envelopes, 11 channel bytecode streams.
// No dependencies beyond the C++ standard library.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace opnmml {

enum Machine { M_PC88 = 0, M_X1 = 4, M_MZ = 5 };   // values passed to the chip setter (sub_1001BB20)

struct Tone {                 // 26 bytes, layout from sub_1001CA10 (this+13324+26*n)
    uint8_t b[26];
    Tone() { for (int i = 0; i < 26; i++) b[i] = 0; }
};

struct Envelope {             // this+4876+132*n : count, loop index, up to 63 words
    int16_t count = 0;
    int16_t loop  = 0;
    int16_t v[64] = {0};
};

struct LoopFrame { int start = 0, brk = 0, ticks = 0, brkTicks = 0; };

struct Channel {              // compile state (156-byte record at this+16740+156*ch) + output
    int16_t octave = 4, octDir = 1, defLen = 48, quant = 8;
    int     keysig[7] = {0, 2, 4, 5, 7, 9, 11};   // c d e f g a b
    int     depth = 0;
    int     totalTicks = 0;
    int     loopPoint = -1;
    int     transpose = 0;
    LoopFrame loops[5];
    std::vector<uint8_t> code;
};

struct Song {
    std::string title;
    int  machine     = M_PC88;
    bool x1Mode      = false;   // this+18468 : 11 channels A-K, 11-value operators
    int  totalOffset = 8;       // this+16736 : "#total_offset n" stores n+8
    Tone     tones[128];
    bool     toneDefined[128] = {false};
    Envelope envs[64];
    Channel  ch[11];
    std::string error;          // "message line"
};

// Returns true on success.  On failure Song::error holds "<message> <line>".
bool compile(const char* text, Song& out);

}  // namespace opnmml
