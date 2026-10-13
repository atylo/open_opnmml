// ================================================================================================================
// MML compiler - port of the compiler inside wuopn.dll (sub_10021E90 and the functions it calls).
//
// Input : the text of a .mus file (Shift-JIS or ASCII).   Output: a Song = FM tone definitions, volume envelopes and
// one bytecode stream per channel, ready for the sequencer (seq.hpp).  Depends only on the C++ standard library.
//
// FILE LAYOUT  (a line starting with // is a comment; /* ... */ also works)
//   #title "name"            song title
//   #machine pc88|x1|mz      sound hardware: pc88 = 6 channels A-F (default), x1 = 11 channels A-K, mz = PWM
//   #octave n|x              n: '<' lowers and '>' raises the octave (default);  x: reversed
//   #total_offset n          shift the loudness of all FM tones, -8..100 (default 0)
//   @N={ ... }               FM tone definition, N = 0..127:  4 operators x (AR,DR,SR,RR,SL,TL,KS,ML,DT[,DT2,AMS])
//                            followed by AL,FB[,PMD,AMD].   The X1 form has 11 values per operator, PC-88 has 10.
//   @eN={ v1,v2,... }        software volume envelope N = 1..63 (up to 63 entries; "@eN,L={...}" loops back to entry L)
//   ABC... commands         a channel line: one or more channel letters at the start of the line, then commands.
//                            The same commands are compiled for every listed channel.
//
// COMMANDS (channel lines)       lengths: 4 = quarter note, 192/n ticks; %n = n ticks; '.' dots; ^ ties; ~ subtracts
//   c d e f g a b r   note / rest, optional + or -, optional length:  c4  d8.  e%12  f4^8  g+16
//   o4  < >           octave   |  l8 default length  |  t120 tempo  |  v12 volume 0-15  |  ( n  ) n  relative volume
//   @3                select tone  |  q8 gate 1-8, @q20 absolute gate  |  & tie to next note  |  p1 p2 p3 pan
//   D-12              detune  |  T-3 transpose  |  P4 portamento 0-7  |  F20 fade out  |  y reg,val raw register write
//   MP t,period,depth,delay   pitch LFO     MA t,period,depth,delay   volume LFO   (MPOF / MAOF switch them off)
//   EA n,speed        volume envelope n  (EAOF off)  |  R step,vol  release fade  (ROF off)
//   [ ... ]n          repeat n times (default 2), nesting up to 5 deep;  '/' inside = leave on the last pass
//   L                 song loop point (the song restarts here when the channel ends)
//   $SHARP{cdf}  $FLAT{b}    key signature
//   c_g               pitch bend from c to g
// ================================================================================================================
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
    // Internal value of the carrier total-level offset when the file has no #total_offset line.  The DLL stores
    // n + 8 (so '#total_offset 0' = 8), and starts at 8.  Changing this only makes sense for experiments.
    static int defaultTotalOffset;
    int  totalOffset = defaultTotalOffset;       // this+16736 : "#total_offset n" stores n+8
    Tone     tones[128];
    bool     toneDefined[128] = {false};
    Envelope envs[64];
    Channel  ch[11];
    std::string error;          // set when compile() fails: "line N: message"
};

// Returns true on success.  On failure Song::error describes the problem, e.g. "line 12: Bad volume."
bool compile(const char* text, Song& out);

}  // namespace opnmml
