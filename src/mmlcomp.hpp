// ================================================================================================================
// MML compiler - port of the compiler inside the original plugin (sub_10021E90 and the functions it calls).
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

struct LoopFrame { int start = 0, brk = 0, ticks = 0, brkTicks = 0; int line = 0, column = 0; /* where the '[' is, for error messages */ };

struct Channel {              // compile state (156-byte record at this+16740+156*ch) + output
    int16_t octave = 4, octDir = 1, defLen = 48, quant = 8;
    int     keysig[7] = {0, 2, 4, 5, 7, 9, 11};   // c d e f g a b
    int     depth = 0;
    int     totalTicks = 0;
    int     loopPoint = -1;
    int     transpose = 0;
    LoopFrame loops[5];
    int     loopPointLine = 0, loopPointColumn = 0;   // where the 'L' (song loop point) is, for error messages
    std::vector<uint8_t> code;   // PERSISTENT: every song compiled into the slot appends here (see Song::songs)
    // What sub_1001D7D0 resets at the start of every compile (everything except `code`).
    void resetCompileState() {
        octave = 4; octDir = 1; defLen = 48; quant = 8;
        static const int nat[7] = {0, 2, 4, 5, 7, 9, 11};
        for (int i = 0; i < 7; i++) keysig[i] = nat[i];
        depth = 0; totalTicks = 0; loopPoint = -1; transpose = 0;
    }
};

// One song number (0..63) inside a slot: where each channel's code starts in Channel::code, and the title.
struct SongEntry {
    int start[11] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};   // dword 67 + 11*num + ch of the slot object (0 = never compiled)
    std::string title;
};

// A Song is the storage of ONE sequencer slot (the DLL's CSoundSequencer object): up to 64 songs, numbered 0..63,
// that share FM tone definitions, volume envelopes and one growing bytecode vector per channel.  compile() APPENDS
// a song; tones and envelopes of earlier songs stay and can be used (or overwritten) by later ones.
struct Song {
    SongEntry songs[64];
    bool loaded[64] = {false};   // song number was compiled successfully since the last clearAll()
    int  lastNum     = 0;       // song number of the most recent compile (this+18464)
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
    // Filled in when compile() fails.  line/column are 1-based; 0 = unknown.
    int         errorLine = 0, errorColumn = 0;
    std::string errorMessage;   // the English message alone
    std::string error;          // message with its position, e.g. "line 12, column 5: Volume out of range (valid 0-15)."
    // Title of song `num` (clamped to 0..63 like the DLL).
    const std::string& titleOf(int num) const { return songs[num < 0 ? 0 : num > 63 ? 63 : num].title; }
    // Forget every song, tone, envelope and all code (sub_1001DA20, what OPNSetDevice does to every slot).
    void clearAll();
    // Compiler-style one-liner:  "file:12:5: error: message"   (the column or the line is left out when unknown)
    std::string describeError(const char* file) const;
};

// Compile `text` as song `num` of the slot `slot` (what OPNPlaySE does, sub_10023060 without the stop/reset around it).
// num is clamped to 0..63.  The new code is appended to the slot's channel vectors; tones and envelopes are kept from
// earlier songs.  Returns true on success.  On failure the slot is left EXACTLY as it was before the call (the original
// leaves half-compiled code and half-defined tones behind) and slot.error / errorLine / errorColumn describe the problem.
bool compile(const char* text, Song& slot, int num);
// Convenience: a clean slot with this one song as number 0.   On failure Song::error describes the problem.
bool compile(const char* text, Song& out);

}  // namespace opnmml
