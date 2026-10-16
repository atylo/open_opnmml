// Faithful port of the wuopn.dll MML compiler stage.
// Function names in comments are the IDA/Hex-Rays names from wuopn.cpp.
#include "mmlcomp.hpp"
#include <cstdio>
#include <cstring>

namespace opnmml {
namespace {

const int NONE = 987654;   // "number not present" sentinel used throughout the original

// ---- messages (original text is Shift-JIS Japanese; English glosses here) ----
const char* const E_LOOP_NEST   = "Loop nesting too deep.";
const char* const E_LOOP_CMD    = "Loop command error.";
const char* const E_REPEAT      = "Bad repeat count.";
const char* const E_LOOP_ZERO   = "- Warning - loop body length is zero.";
const char* const E_ATQ         = "Bad quantize (@q).";
const char* const E_TONE_NO     = "Bad tone number.";
const char* const E_DETUNE      = "Bad detune.";
const char* const E_VOLUME      = "Bad volume.";
const char* const E_RELVOL      = "Bad relative volume.";
const char* const E_R           = "Bad R command. (step,volume)";
const char* const E_LFO         = "LFO command error";
const char* const E_ENVEL       = "Bad volume envelope. (no,step)";
const char* const E_FADE        = "Bad fade-out.";
const char* const E_PAN         = "Bad pan.";
const char* const E_TEMPO       = "Bad tempo.";
const char* const E_PORTA       = "Bad portamento.";
const char* const E_INFLOOP_IN  = "Infinite-loop command not allowed inside a loop.";
const char* const E_Y           = "Bad Y command.";
const char* const E_TRANSPOSE   = "Transpose out of range.";
const char* const E_QUANT       = "Bad quantize.";
const char* const E_LEN         = "Bad note length.";
const char* const E_SYNTAX      = "Syntax Error.";
const char* const E_PITCH       = "Bad pitch.";
const char* const E_BEND        = "Bad bend.";
const char* const E_BENDW       = "Bend width too large.";
const char* const E_OCTAVE      = "Octave out of range (valid octaves are 0-7; the original message said 1-8).";

// ---- tone parameter table (rdata 0x1002F2E0: name,min,max,shift) ----
struct PDef { const char* name; int mn, mx, sh; };
const PDef PT[15] = {
    {"AR", 0, 31, 0}, {"DR", 0, 31, 0}, {"SR", 0, 31, 0}, {"RR", 0, 15, 0},
    {"SL", 0, 15, 4}, {"TL", 0, 127, 0}, {"KS", 0, 3, 6}, {"MUL", 0, 15, 0},
    {"DT", -3, 7, 4}, {"AMS", 0, 1, 7}, {"DT2", 0, 3, 6},
    {"AL", 0, 7, 0}, {"FB", 0, 7, 3}, {"PMD", 0, 7, 0}, {"AMD", 0, 3, 4}};

// dword_1002FB70[32]: carrier flag per (algorithm, operator)
const int CARRIER[32] = {0,0,0,128, 0,0,0,128, 0,0,0,128, 0,0,0,128,
                         0,128,0,128, 0,128,128,128, 0,128,128,128, 128,128,128,128};
const int OPOFS[4] = {0, 12, 6, 18};   // dword_1002FBF0

// ---- lexical helpers (sub_1001C0D0 / C130 / C890 / C9B0 / D620 / F8B0) ----
void skipWs(const char*& p, int& col) {          // space, tab, LF, CR
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { ++p; ++col; }
}
void skipSp(const char*& p, int& col) {          // space, tab
    while (*p == ' ' || *p == '\t') { ++p; ++col; }
}
int parseInt(const char*& p, int& col, int def) {
    bool neg = false, got = false;
    int v = 0;
    skipSp(p, col);
    const char* start = p;
    if (*p == '-') { neg = true; ++p; ++col; }
    if (*p == '+') { neg = false; ++p; ++col; }
    skipSp(p, col);
    // The DLL does this in 32-bit registers, so very long numbers wrap around.  Use unsigned arithmetic so the wrap-around
    // is defined behaviour in C++ too (signed overflow is not).
    while (*p >= '0' && *p <= '9') { v = (int)(10u * (unsigned)v + (unsigned)(*p++ - '0')); ++col; got = true; }
    skipSp(p, col);
    if (neg) v = (int)(0u - (unsigned)v);
    if (!got) { v = def; p = start; }
    return v;
}
bool comma(const char*& p, int& col) {
    skipSp(p, col);
    if (*p != ',') return false;
    ++p; ++col;
    skipSp(p, col);
    return true;
}
bool prefixI(const char* name, const char* s) {  // sub_1001BD10: case-insensitive prefix test
    while (*name) {
        char a = *name++, b = *s++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
int lenOne(const char*& p, int& acc, int& col, int def) {   // sub_1001D620
    bool pct = false;
    skipSp(p, col);
    if (*p == '%') { ++p; ++col; pct = true; skipSp(p, col); }
    int v6 = parseInt(p, col, NONE);
    if (v6 == NONE && pct) return 0;
    if (v6 < 1) return 0;
    if (!pct) v6 = (v6 == NONE) ? def : 192 / v6;
    int v5 = v6;
    skipSp(p, col);
    while (*p == '.') { ++p; ++col; v5 /= 2; v6 += v5; skipSp(p, col); }
    if (*p == '^')      { ++p; int r = lenOne(p, acc, col, def); acc += r; }
    else if (*p == '~') { ++p; int r = lenOne(p, acc, col, def); acc -= r; }
    return v6 >= 1 ? v6 : 0;
}
int parseLen(const char*& p, int& col, int def) {            // sub_1001F8B0
    int acc = 0;
    int r = lenOne(p, acc, col, def);
    return acc + r;
}

// ---- compiler object ----
struct Comp {
    Song& S;
    int cur = 0;                 // this+18456 : current channel
    explicit Comp(Song& s) : S(s) {}

    Channel& C() { return S.ch[cur]; }
    void put(int b)            { C().code.push_back((uint8_t)b); }
    void put2(int a, int b)    { put(a); put(b); }
    void put3(int a, int b, int c) { put(a); put(b); put(c); }
    void put16(int v)          { put((v >> 8) & 0xFF); put(v & 0xFF); }

    void fail(const char* msg, int line) {
        char buf[512];
        snprintf(buf, sizeof buf, "line %d: %s", line, msg);
        S.error = buf;
    }

    // sub_1001C170 : $SHARP{...} / $FLAT{...}
    bool keySig(const char*& p, int line, int& col) {
        (void)line;
        int accidental = 0;
        if (strncmp(p, "$SHARP{", 6) == 0)      { p += 7; col += 7; accidental = 1; }
        else if (strncmp(p, "$FLAT{", 5) == 0)  { p += 6; col += 6; accidental = -1; }
        else { S.error = "$SHARP/$FLAT"; return false; }
        static const int nat[7] = {0, 2, 4, 5, 7, 9, 11};
        for (int i = 0; i < 7; i++) C().keysig[i] = nat[i];
        skipSp(p, col);
        while (*p != 0 && *p != '}') {
            int idx = -1;
            switch (*p) {
                case 'c': idx = 0; break; case 'd': idx = 1; break; case 'e': idx = 2; break;
                case 'f': idx = 3; break; case 'g': idx = 4; break; case 'a': idx = 5; break;
                case 'b': idx = 6; break;
            }
            if (idx < 0) break;
            C().keysig[idx] = nat[idx] + accidental;
            ++p; ++col;
            skipSp(p, col);
        }
        if (*p == '}') { ++p; ++col; return true; }
        S.error = "$SHARP/$FLAT";
        return false;
    }

    // sub_1001FA30 : one MML command.  Returns false on error (S.error set).
    // NOTE on signedness: in the original several argument variables are `unsigned int` (relative volume, EA number, LFO type
    // and delay, P, R step, v, y), so their range checks also reject NEGATIVE values.  Found by differential fuzzing.
    bool command(const char*& p, int line, int& col) {
        bool ok = true;
        const char* Str = "";
        int note = -1;
        Channel& c = C();
        skipSp(p, col);
        switch (*p) {
        case '$':
            ok = keySig(p, line, col);
            break;
        case '&':
            ++p; ++col; put(131);
            break;
        case '(': case ')': {
            bool open = (*p == '(');
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v == NONE) v = 1;
            if ((unsigned)v > 10u) Str = E_RELVOL;                       // unsigned compare in the original: negative = error
            // '(' : op 150 when octDir<=0 else 151 ; ')' is the opposite
            bool a = (c.octDir <= 0) ? open : !open;
            put2(a ? 150 : 151, v);
            break;
        }
        case '/': {
            ++p; ++col;
            if (c.depth <= 0) { fail(E_LOOP_CMD, line); return false; }
            put3(193, 0, 0);
            LoopFrame& f = c.loops[c.depth - 1];
            f.brk = (int)c.code.size();
            f.brkTicks = f.ticks;
            break;
        }
        case '<': ++p; ++col; c.octave -= c.octDir; break;
        case '>': ++p; ++col; c.octave += c.octDir; break;
        case '@': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v == NONE && *p == 'q') {
                ++p; ++col;
                v = parseInt(p, col, NONE);
                if (v < 1 || v > 127) Str = E_ATQ;
                c.quant = (int16_t)-v;
            } else {
                if (v <= 0 || v > 127) Str = E_TONE_NO;
                put2(130, v);
            }
            break;
        }
        case 'D': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v < -8192 || v > 0x2000) Str = E_DETUNE;
            put(129); put16(v);
            break;
        }
        case 'E': {
            ++p; ++col;
            if (strncmp(p, "AOF", 3) == 0) {
                p += 3; col += 3;
                put3(165, 0, 1);
            } else if (*p == 'A') {
                ++p; ++col;
                int n = parseInt(p, col, NONE);
                comma(p, col);
                int m = parseInt(p, col, NONE);
                if ((unsigned)n > 0x40u || m < 1 || m > 100) Str = E_ENVEL;
                put3(165, n, m);
            }
            break;
        }
        case 'F': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v < 1 || v > 255) Str = E_FADE;
            put2(149, v);
            break;
        }
        case 'L':
            ++p; ++col;
            put(207);
            c.loopPoint = c.totalTicks;
            if (c.depth != 0) Str = E_INFLOOP_IN;
            break;
        case 'M': {
            ++p; ++col;
            if (strncmp(p, "AOF", 3) == 0) {
                p += 3; col += 3;
                put3(161, 0, 1); put2(0, 0);
            } else if (strncmp(p, "POF", 3) == 0) {
                p += 3; col += 3;
                put3(160, 0, 1); put2(0, 0);
            } else {
                int op = 0, v30 = 0;
                bool chain = true;
                if (*p == 'A') op = 161; else if (*p == 'P') op = 160;
                ++p; ++col;
                if (op == 0) Str = E_LFO;
                int v31 = parseInt(p, col, NONE);
                chain = comma(p, col) && chain;
                int v29 = parseInt(p, col, NONE);
                chain = comma(p, col) && chain;
                int v32 = parseInt(p, col, NONE);
                skipSp(p, col);
                if (chain && *p == ',') { comma(p, col); v30 = parseInt(p, col, NONE); }
                if (v32 < -128 || v32 > 127) { op += 2; v32 >>= 2; }
                if ((unsigned)v31 > 4u || v29 < 1 || v29 > 100 || v32 < -128 || v32 > 127 || (unsigned)v30 > 100u) Str = E_LFO;
                put3(op, v31, v29);
                put2(v32, v30);
            }
            break;
        }
        case 'P': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if ((unsigned)v >= 8u) Str = E_PORTA;
            put2(136, v);
            break;
        }
        case 'R': {
            ++p; ++col;
            bool hasComma = true;
            int step, vol;
            if (strncmp(p, "OF", 2) == 0) { p += 2; col += 2; step = 0; vol = 1; }
            else {
                step = parseInt(p, col, NONE);
                hasComma = comma(p, col);
                vol = parseInt(p, col, NONE);
            }
            if ((unsigned)step >= 0x80u || vol < 1 || vol > 15 || !hasComma) Str = E_R;
            put3(148, step, vol);
            break;
        }
        case 'T': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v < -64 || v > 64) Str = E_TRANSPOSE;
            c.transpose = v;
            break;
        }
        case '[': {
            ++p; ++col;
            if (c.depth >= 5) { fail(E_LOOP_NEST, line); return false; }
            put2(192, 0);
            LoopFrame& f = c.loops[c.depth++];
            f.start = (int)c.code.size();
            f.brk = 0; f.ticks = 0; f.brkTicks = 0;
            break;
        }
        case ']': {
            ++p; ++col;
            int n = parseInt(p, col, NONE);
            if (n == NONE) n = 2;
            if (c.depth <= 0) { fail(E_LOOP_CMD, line); return false; }
            if (n < 1 || n > 127) Str = E_REPEAT;
            --c.depth;
            LoopFrame& f = c.loops[c.depth];
            int dist = (int)c.code.size() + 3 - f.start;
            put(194); put16(dist);
            c.code[f.start - 1] = (uint8_t)n;                 // patch repeat count of [192,n]
            int total;
            if (f.ticks == 0) Str = E_LOOP_ZERO;
            if (f.brk != 0) {
                int d = (int)c.code.size() - f.brk;
                c.code[f.brk - 2] = (uint8_t)(d >> 8);
                c.code[f.brk - 1] = (uint8_t)d;
                total = f.brkTicks + f.ticks * (n - 1);
            } else {
                total = f.ticks * n;
            }
            if (c.depth != 0) c.loops[c.depth - 1].ticks += total;
            else              c.totalTicks += total;
            break;
        }
        case 'a': ++p; ++col; note = c.keysig[5]; break;
        case 'b': ++p; ++col; note = c.keysig[6]; break;
        case 'c': ++p; ++col; note = c.keysig[0]; break;
        case 'd': ++p; ++col; note = c.keysig[1]; break;
        case 'e': ++p; ++col; note = c.keysig[2]; break;
        case 'f': ++p; ++col; note = c.keysig[3]; break;
        case 'g': ++p; ++col; note = c.keysig[4]; break;
        case 'l': {
            ++p; ++col;
            c.defLen = (int16_t)parseLen(p, col, 48);
            if (c.defLen <= 0 || c.defLen > 16000) Str = E_LEN;   // original message 0x1002F98C
            break;
        }
        case 'o':
            ++p; ++col;
            c.octave = (int16_t)parseInt(p, col, NONE);
            break;
        case 'p': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v >= 1 && v <= 3) put(v - 105);
            else Str = E_PAN;
            break;
        }
        case 'q': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v < 1 || v > 8) Str = E_QUANT;
            c.quant = (int16_t)v;
            break;
        }
        case 'r': ++p; ++col; note = 127; break;
        case 't': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if (v < 1 || v > 255) Str = E_TEMPO;
            put2(135, v);
            break;
        }
        case 'v': {
            ++p; ++col;
            int v = parseInt(p, col, NONE);
            if ((unsigned)v >= 0x10u) Str = E_VOLUME;
            put2(128, v);
            break;
        }
        case 'y': {
            ++p; ++col;
            int a = parseInt(p, col, NONE);
            bool cm = comma(p, col);
            int b = parseInt(p, col, NONE);
            if ((unsigned)a >= 0x100u || (unsigned)b >= 0x100u || !cm) Str = E_Y;
            put3(143, a, b);
            break;
        }
        default:
            Str = E_SYNTAX;
            break;
        }

        if (note >= 0) {
            int v16 = note;
            if (note < 127) note += c.transpose + 12 * c.octave;
            skipSp(p, col);
            while (*p == '+' || *p == '-') {
                if (*p == '-') --note; else ++note;
                ++p; ++col;
                skipSp(p, col);
            }
            if (note != 127 && (note < 1 || note >= 96)) Str = E_PITCH;
            int len = parseLen(p, col, c.defLen);
            int rest = 0;
            skipSp(p, col);
            if (len <= 0 || len > 16000) Str = E_LEN;
            if (*p == '_') {                                   // pitch bend
                if (note == 127) Str = E_BEND;
                ++p; ++col;
                skipSp(p, col);
                const char* q = p;
                int cc = 0, v10 = 0;
                bool scan = true;
                while (scan) {
                    switch (*q) {
                        case '<': v10 -= 12 * c.octDir; break;
                        case '>': v10 += 12 * c.octDir; break;
                        case 'a': v10 += c.keysig[5]; scan = false; break;
                        case 'b': v10 += c.keysig[6]; scan = false; break;
                        case 'c': v10 += c.keysig[0]; scan = false; break;
                        case 'd': v10 += c.keysig[1]; scan = false; break;
                        case 'e': v10 += c.keysig[2]; scan = false; break;
                        case 'f': v10 += c.keysig[3]; scan = false; break;
                        case 'g': v10 += c.keysig[4]; scan = false; break;
                        default: break;
                    }
                    if (*q == 0) { Str = E_BEND; break; }      // guard (original would run off the buffer)
                    ++q;
                    skipSp(q, cc);
                }
                while (*q == '+' || *q == '-') {
                    if (*q == '-') --v10; else ++v10;
                    ++q;
                    skipSp(q, cc);
                }
                int amount = (int16_t)(16 * (v16 - v10));
                put(144); put16(amount);
                // The original divides here even when the length check above failed (len <= 0), which crashes the whole
                // plugin with a division by zero.  The file is rejected either way (E_LEN), so skip the division.
                int rate = (len != 0) ? (amount << 6) / len : 0;
                if (rate > 0x7FFF || rate < -32767) Str = E_BENDW;
                put16((int16_t)rate);
                put16((int16_t)len);
            } else {
                if (c.depth <= 0) c.totalTicks += len;
                else              c.loops[c.depth - 1].ticks += len;
                if (note != 127) {
                    rest = len;
                    if (*p != '&') {
                        if (c.quant <= 0) len += c.quant;
                        else              len = len * c.quant / 8;
                    }
                    if (len <= 0) len = 1;
                    rest -= len;
                }
                for (int i = 0; i < 2; i++) {
                    int val = (i == 0) ? len : rest;
                    if (val != 0) {
                        put(i == 0 ? note : 127);
                        if (val <= 127) put(val);
                        else { put((val >> 7) | 0x80); put(val & 0x7F); }
                    }
                }
            }
        }

        if ((unsigned)(int16_t)C().octave >= 8) Str = E_OCTAVE;
        if (*Str) { fail(Str, line); return false; }
        return ok;
    }

    // sub_10021CA0 : one channel line.  Returns true on ERROR (matches the original).
    bool channelLine(const char* a2, int line) {
        const char* p = a2;
        int col = 0;
        int nch = S.x1Mode ? 11 : 6;
        char list[16]; int n = 0;
        while ((char)*p >= 'A' && (char)*p < 'A' + nch) { list[n++] = *p - 'A'; ++p; ++col; }
        if (*p == ' ' || *p == '\t') {
            skipSp(p, col);
            const char* body = p;
            int i;
            for (i = 0; i < n; i++) {
                cur = list[i];
                p = body;
                const char* prev = nullptr;
                bool ok = true;
                while (prev != p && *a2 != 0 && ok && *p != '\n' && *p != '\r') {
                    prev = p;
                    ok = command(p, line, col);
                    skipSp(p, col);
                }
                if (!ok) break;
            }
            return i != n;
        }
        char buf[128];
        snprintf(buf, sizeof buf, "Bad channel header [A-%c] %d", nch + 'A' - 1, line);
        S.error = buf;
        return true;
    }

    // sub_1001CA10 : "@n={...}" / "@en={...}".  Returns true on ERROR.
    bool defBlock(const char* a2, int line) {
        const char* p = a2;
        int col = 0;
        int values[256]; memset(values, 0, sizeof values);
        int count = 0, loopIndex = -1;
        bool env = false;
        char msg[256];
        skipWs(p, col);
        if (*p == 'e' || *p == 'E') { env = true; ++p; skipWs(p, col); }
        int num = parseInt(p, col, NONE);
        if (env) {
            if (num < 1 || num >= 64) { snprintf(msg, sizeof msg, "Envelope number out of range (1-63), near line %d", line); S.error = msg; return true; }
        } else if ((unsigned)num >= 0x80) {
            snprintf(msg, sizeof msg, "Tone number out of range (0-127), near line %d", line); S.error = msg; return true;
        }
        skipWs(p, col);
        if (env && comma(p, col)) { loopIndex = parseInt(p, col, NONE); skipWs(p, col); }
        if (*p != '=') {
            snprintf(msg, sizeof msg, env ? "Bad envelope definition (1-63), near line %d" : "Bad tone number definition (0-127), near line %d", line);
            S.error = msg; return true;
        }
        ++p; skipWs(p, col);
        if (*p != '{') {
            snprintf(msg, sizeof msg, env ? "Bad envelope definition (1-63), near line %d" : "Bad tone number definition (0-127), near line %d", line);
            S.error = msg; return true;
        }
        ++p; skipWs(p, col);
        values[count] = parseInt(p, col, NONE);
        while (values[count] != NONE) {
            if (++count >= 65) break;
            skipWs(p, col);
            if (!comma(p, col)) break;
            skipWs(p, col);
            values[count] = parseInt(p, col, NONE);
        }
        if (env) {
            if (count < 1 || count >= 64) { snprintf(msg, sizeof msg, "Envelope entry count out of range (1-63), near line %d", line); S.error = msg; return true; }
            Envelope& e = S.envs[num];
            e.count = (int16_t)count;
            if (loopIndex < 0) loopIndex = count - 1;
            e.loop = (int16_t)loopIndex;
            for (int i = 0; i < count; i++) e.v[i] = (int16_t)values[i];
            return false;
        }
        int valuesPerOp = S.x1Mode ? 11 : 10;
        int carrierOffset[32];
        for (int j = 0; j < 32; j++) carrierOffset[j] = (CARRIER[j] ? S.totalOffset : 0) + CARRIER[j];
        if (count < 4 * valuesPerOp + 2 || count > 4 * valuesPerOp + 4) {
            snprintf(msg, sizeof msg, "Tone parameter count out of range, near line %d", line); S.error = msg; return true;
        }
        static const int x1ParamOrder[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 9};
        for (int k = 0; k < 4; k++) {
            for (int mm = 0; mm < valuesPerOp; mm++) {
                int ti = S.x1Mode ? x1ParamOrder[mm] : mm;
                int& v = values[mm + valuesPerOp * k];
                if (v < PT[ti].mn || v > PT[ti].mx) {
                    snprintf(msg, sizeof msg, "Tone parameter [%s] out of range (%d..%d), near line %d", PT[ti].name, PT[ti].mn, PT[ti].mx, line);
                    S.error = msg; return true;
                }
                if (strcmp(PT[ti].name, "DT") == 0 && v < 0) { v = -v; v ^= 4; }
                v <<= PT[ti].sh;
            }
        }
        for (int k = 0; k < 4; k++) {
            int& v = values[4 * valuesPerOp + k];
            const PDef& d = PT[11 + k];
            if (v < d.mn || v > d.mx) {
                snprintf(msg, sizeof msg, "Tone parameter [%s] out of range (%d..%d), near line %d", d.name, d.mn, d.mx, line);
                S.error = msg; return true;
            }
            v <<= d.sh;
        }
        uint8_t* t = S.tones[num].b;
        S.toneDefined[num] = true;
        for (int i = 0; i < 4; i++) t[OPOFS[i] + 0] = (uint8_t)(values[i * valuesPerOp + 8] | values[i * valuesPerOp + 7]);
        for (int j = 0; j < 4; j++) {
            uint8_t r = (uint8_t)(carrierOffset[4 * values[4 * valuesPerOp] + j] + values[j * valuesPerOp + 5]);
            t[OPOFS[j] + 1] = r;
            if (((carrierOffset[4 * values[4 * valuesPerOp] + j] ^ r) & 0x80) != 0) t[OPOFS[j] + 1] = 0xFF;
        }
        for (int k = 0; k < 4; k++) t[OPOFS[k] + 2] = (uint8_t)(values[k * valuesPerOp + 6] | values[k * valuesPerOp]);
        for (int k = 0; k < 4; k++) t[OPOFS[k] + 3] = (uint8_t)(values[k * valuesPerOp + 9] | values[k * valuesPerOp + 1]);
        for (int k = 0; k < 4; k++) t[OPOFS[k] + 4] = (uint8_t)values[k * valuesPerOp + 2];
        for (int k = 0; k < 4; k++) t[OPOFS[k] + 5] = (uint8_t)(values[k * valuesPerOp + 4] | values[k * valuesPerOp + 3]);
        t[24] = (uint8_t)(values[4 * valuesPerOp + 1] | values[4 * valuesPerOp]);
        t[25] = (uint8_t)(values[4 * valuesPerOp + 3] | values[4 * valuesPerOp + 2]);
        return false;
    }

    // sub_10022690 : end-of-song checks (+ terminating 0 byte on every channel)
    bool finish() {
        bool ok = true;
        for (int i = 0; i < 11; i++) S.ch[i].code.push_back(0);
        for (int j = 0; j < 11; j++) {
            char m[128];
            if (S.ch[j].depth != 0) {
                snprintf(m, sizeof m, "Channel %c: loop is not closed.", j + 'A');
                S.error = m; ok = false;
            }
            if (S.ch[j].totalTicks == S.ch[j].loopPoint) {
                snprintf(m, sizeof m, "Channel %c: infinite loop count is zero.", j + 'A');
                S.error = m; ok = false;
            }
        }
        return ok;
    }

    // sub_10021E90 : top-level scanner (state machine, ported verbatim)
    bool run(const char* text) {
        const char* cur = text;
        std::string buf;
        int st = 0, prevSt = 0, line = 1, col = 1;
        bool stop = false;
        static const struct { const char* name; int state; } DIR[] = {
            {"octave", 6}, {"title", 7}, {"machine", 8}, {"total_offset", 9}, {nullptr, 0}};
        while (*cur != 0 && !stop) {
            char c = *cur++;
            ++col;
            if (st != 2 && st != 3 && c == '/') {
                if (*cur == '*' || *cur == '/') {
                    prevSt = st;
                    st = (*cur == '*') ? 3 : 2;
                    ++cur; ++col;
                    if (*cur == 0) break;
                    c = *cur++; ++col;
                }
            }
            bool skipTail = false;
            switch (st) {
            case 0:
                if (c == '#') {
                    skipSp(cur, col);
                    for (int i = 0; DIR[i].name; i++) {
                        if (prefixI(DIR[i].name, cur)) {
                            size_t n = strlen(DIR[i].name);
                            st = DIR[i].state;
                            cur += n; col += (int)n;
                            skipSp(cur, col);
                            skipTail = true;           // goto LABEL_94
                            break;
                        }
                    }
                } else if (c == '@') {
                    skipSp(cur, col);
                    if (*cur == 'e' || *cur == 'E') { buf.assign(1, *cur); st = 5; ++cur; ++col; }
                    else if (*cur >= '0' && *cur <= '9') { buf.assign(1, *cur); st = 4; ++cur; ++col; }
                } else if (col == 2 && c >= 'A' && c <= 'K') {
                    buf.assign(1, c);
                    st = 1;
                }
                break;
            case 1:
                buf.push_back(c);
                if (c == '\n') { stop = channelLine(buf.c_str(), line); st = 0; }
                break;
            case 2:
                if (c == '\n') st = prevSt;
                break;
            case 3:
                if (c == '*' && *cur == '/') { st = prevSt; ++cur; ++col; }
                break;
            case 4: case 5:
                buf.push_back(c);
                if (c == '}') { stop = defBlock(buf.c_str(), line); st = 0; }
                break;
            case 6:
                if (prefixI("x", cur - 1)) { for (auto& c : S.ch) c.octDir = -1; }
                else if (prefixI("n", cur - 1)) { for (auto& c : S.ch) c.octDir = 1; }
                else { char m[64]; snprintf(m, sizeof m, "Pseudo-command error, line %d", line); S.error = m; stop = true; }
                while (*cur != 0 && *cur != '\n') { ++cur; ++col; }
                c = *cur++; ++col; st = 0;
                break;
            case 7:
                while (*cur != 0 && *cur != '\n' && (c != '"' || *cur != '"')) { S.title.push_back(*cur++); ++col; }
                c = *cur++; ++col; st = 0;
                break;
            case 8:
                if (prefixI("x1", cur - 1))        { S.x1Mode = true;  S.machine = M_X1; }
                else if (prefixI("pc88", cur - 1)) { S.x1Mode = false; S.machine = M_PC88; }
                else if (prefixI("mz", cur - 1))   { S.x1Mode = false; S.machine = M_MZ; }
                else { char m[64]; snprintf(m, sizeof m, "Pseudo-command error, line %d", line); S.error = m; stop = true; }
                while (*cur != 0 && *cur != '\n') { ++cur; ++col; }
                c = *cur++; ++col; st = 0;
                break;
            case 9: {
                --cur;
                int offset = parseInt(cur, col, NONE);
                if (offset > 100 || offset < -8) {
                    char m[96]; snprintf(m, sizeof m, "Pseudo-command error, line %d: total_offset must be -8..100.", line);
                    S.error = m; stop = true;
                }
                S.totalOffset = offset + 8;
                while (*cur != 0 && *cur != '\n') { ++cur; ++col; }
                c = *cur++; ++col; st = 0;
                break;
            }
            default: break;
            }
            (void)skipTail;
            if (c == '\n') { ++line; col = 1; }
        }
        if (stop) return false;
        return finish();
    }
};

}  // namespace

int Song::defaultTotalOffset = 8;

bool compile(const char* text, Song& out) {
    out = Song();
    Comp c(out);
    return c.run(text);
}

}  // namespace opnmml
