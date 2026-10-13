// Port of wuopn.dll "COpx4".  Function names in comments are the Hex-Rays names from wuopn.cpp.
#include "opx4.hpp"
#include "chiptables.inc"
#include <cstring>

namespace opnmml {
namespace {

// ---- noise / LFO random table (word_10035A60), sub_1001A490 ----
// The DLL fills it with the C runtime's rand().  MSVC's rand() with the default seed 1 is this LCG.
// If anything in the host process called rand()/srand() first, the real table differs; use setNoiseSeed().
int16_t  g_noise[16384];
bool     g_noiseInit = false;
uint32_t g_seed = 1;
int msvcRand() { g_seed = g_seed * 214013u + 2531011u; return (int)((g_seed >> 16) & 0x7FFF); }
void initNoise() {
    if (g_noiseInit) return;
    g_noiseInit = true;
    for (int i = 0; i < 0x4000; i++) g_noise[i] = (int16_t)((msvcRand() & 0xFFF) - 2048);
}

// rate-table block: the decompile addresses it as dword_1002FA30[x] / FB80 / FC30 / FC80 / FCC0
inline int FA30(int i) { return kR2FB80[i]; }
inline int FB80(int i) { return kR2FB80[84 + i]; }
inline int FC30(int i) { return kR2FB80[128 + i]; }
inline int FC80(int i) { return kR2FB80[148 + i]; }
inline int FCC0(int i) { return kR2FB80[164 + i]; }

inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

}  // namespace

void Opx4::setNoiseSeed(uint32_t seed) { g_seed = seed; g_noiseInit = false; }

// ------------------------------------------------------------------ construction / reset

Opx4::Opx4(int outRate, int clock) : d(1400, 0) {            // sub_1001BC80
    D(3) = clock;
    D(4) = 1024;
    D(6) = outRate;
    D(7) = 0;
    D(8) = 0x4000;
    initNoise();                                              // sub_1001A490
    D(1) = 0;
    setMode(D(1));
}

void Opx4::setMode(int m) {                                   // sub_1001BB20
    D(1) = m;
    switch (m) {
    case 0: case 5: D(5) = (int32_t)((uint32_t)D(3) / 0x48u); D(2) = 4095; break;
    case 1: case 4: D(5) = (int32_t)((uint32_t)D(3) >> 6);    D(2) = -1;   break;
    case 2:         D(5) = (int32_t)((uint32_t)D(3) / 0x48u); D(2) = 16715760; break;
    case 3:         D(5) = (int32_t)((uint32_t)D(3) / 0x48u); D(2) = 0xFFFFFF; break;
    default: break;
    }
    D(8) = (int32_t)(((uint32_t)D(6) << 14) / (uint32_t)D(5));                       // sub_1001A450
    D(9) = (int32_t)((((uint32_t)D(3) << 9) / (uint32_t)D(6)) << 12);                // sub_1001A6A0
    reset();
    B(5000) = 0;                                              // sub_1001A430(this, 0)
}

void Opx4::reset() {                                          // sub_1001B7F0
    B(41) = 0; B(40) = 0;
    memset(bytes() + 42, 0, 0x200);
    memset(bytes() + 588, 0, 0xE80);
    memset(bytes() + 4300, 0, 0x240);
    for (int i = 0; i < 32; i++) {
        int o = 147 + 29 * i;
        D(o + 21) = DUMMY;
        D(o + 22) = DUMMY;
        D(o + 18) = 4190208;
        D(o + 20) = 8;
        D(o + 27) = rateCell(0);
        D(o + 28) = 1075 + 18 * (i >> 2);
        if ((i & 3) == 0) D(o + 21) = 1085 + 18 * (i >> 2);
    }
    for (int j = 0; j < 8; j++) {
        int c = 1075 + 18 * j;
        D(c + 6) = -1; D(c + 7) = -1; D(c + 0) = -1; D(c + 8) = 1;
        D(c + 14) = DUMMY; D(c + 15) = DUMMY; D(c + 16) = DUMMY; D(c + 17) = DUMMY;
    }
    D(140) = 0; D(139) = 0; D(143) = 0; D(142) = 0;
    D(1219) = 0; D(1220) = 0; D(1221) = 0;
    D(1224) = 2; D(1223) = 127; D(1222) = 127;
    D(144) = 0;
    r33D0((int8_t)255);
    r33D0(127);
    D(1224) = 2 & 3;                                          // sub_100039F0(this, 2)
    for (int k = 0; k < 13; k++) write(k, 0);
    write(7, 56);
    D(1235) = 424936722;
    D(1236) = 0;
    D(1253) = 0; D(1251) = 0; D(1252) = 0;
    for (int i = 0; i < 32; i++) D(147 + 29 * i + 13) = 4;    // sub_1001A3F0
}

int Opx4::read(int reg) { return (reg >= 0 && reg < 0x20) ? B(42 + reg) : 0; }

void Opx4::write(int reg, int val) {                          // sub_1001B7A0
    if (D(1) == 1 || D(1) == 4) opmWrite(reg, val);
    else                        writeOpn(reg, val);
}

// ------------------------------------------------------------------ register helpers

void Opx4::r33D0(int8_t v) { if (v < 0) D(1222) = v & 0x7F; else D(1223) = v & 0x7F; }       // sub_100033D0

void Opx4::r3410(int ch, int v) {                                                             // sub_10003410
    int v5 = (int16_t)v & 0x3FFF;
    int v4 = (v5 & 0x7FF) << (v5 >> 11);
    D(1077 + 18 * ch) = 4 * kFnumBlk[v5 >> 7];
    D(1078 + 18 * ch) = v4;
    D(158 + 116 * ch) = v4;
    D(1083 + 18 * ch) = 1;
}
void Opx4::r34B0(int op, int v) {                                                             // sub_100034B0
    int r = ((int16_t)v & 0x7FF) << (((int16_t)v & 0x3FFF) >> 11);
    D(158 + 29 * op) = r;
    D(1083 + 18 * (op >> 2)) = 1;
}
void Opx4::r3520(int ch, int v) {                                                             // sub_10003520
    D(1079 + 18 * ch) = kTab_ABB0[(v >> 4) & 3];
    D(1080 + 18 * ch) = kTab_AB70[v & 7];
    r3740(ch, v >> 6);
}
void Opx4::r3580(int ch, int v) {                                                             // sub_10003580
    connect(ch, (D(1075 + 18 * ch) & 0x18) | (v & 7));
    int fb = (v >> 3) & 7;
    D(1076 + 18 * ch) = fb ? 9 - fb : 23;
}
void Opx4::r35F0(int a3) {                                                                    // sub_100035F0
    int v4 = a3 >> 6;
    if (v4 != D(139)) { D(139) = v4; D(1119) = 1; D(1173) = 1; D(1120) = v4; D(1174) = v4; }
}
void Opx4::r3660(int ch, int keys) {                                                          // sub_10003660
    int ob = 147 + 116 * ch;
    for (int i = 0; i < 4; i++) {
        int op = ob + 29 * FCC0(i);
        if (keys & (1 << i)) {
            if ((uint32_t)D(op + 13) >= 3u) { D(op + 13) = 0; D(op + 12) = 0; D(op + 20) = 0; }
        } else if ((uint32_t)D(op + 13) < 3u) {
            D(op + 20) = 0; D(op + 13) = 3;
        }
    }
}
void Opx4::r3740(int ch, int v) { D(1081 + 18 * ch) = -(v & 1); D(1082 + 18 * ch) = -((v >> 1) & 1); }   // sub_10003740
void Opx4::r3790(int op, int v) {                                                             // sub_10003790
    D(154 + 29 * op) = (v & 0xF) ? 2 * (v & 0xF) : 1;
    D(157 + 29 * op) = kDtTab[(v >> 4) & 7];
    D(1083 + 18 * (op >> 2)) = 1;
}
void Opx4::r3810(int op, int v) { D(152 + 29 * op) = 8 * (v & 0x7F); }                        // sub_10003810
void Opx4::r3840(int op, int v) {                                                             // sub_10003840
    D(147 + 29 * op) = 2 * (v & 0x1F);
    D(153 + 29 * op) = (v >> 6) & 3;
    D(1083 + 18 * (op >> 2)) = 1;
}
void Opx4::r38A0(int op, int v) {                                                             // sub_100038A0
    D(148 + 29 * op) = 2 * (v & 0x1F);
    D(156 + 29 * op) = -((v >> 7) != 0);
    D(1083 + 18 * (op >> 2)) = 1;
}
void Opx4::r3900(int op, int v) {                                                             // sub_10003900
    D(149 + 29 * op) = 2 * (v & 0x1F);
    D(155 + 29 * op) = kLfoShift[(v >> 6) & 3];
    D(1083 + 18 * (op >> 2)) = 1;
}
void Opx4::r3960(int op, int v) {                                                             // sub_10003960
    D(150 + 29 * op) = 4 * (v & 0xF) + 2;
    D(151 + 29 * op) = ((v >> 4) & 0xF) << 17;
    D(1083 + 18 * (op >> 2)) = 1;
}
void Opx4::r32D0(int ch, int v) {                                                             // sub_100032D0
    D(1079 + 18 * ch) = (v & 3) ? (255 << ((v & 3) - 1)) : 0;
    D(1080 + 18 * ch) = kTab_AB90[(v >> 4) & 7];
}
void Opx4::r3340(int ch, int8_t kc, int kf) {                                                 // sub_10003340
    D(1077 + 18 * ch) = kc & 0x7F;
    D(1078 + 18 * ch) = ((D(1077 + 18 * ch) >> 4) << 16) + (kf >> 2) + (kTab_AEE0[D(1077 + 18 * ch) & 0xF] << 6);
    D(1083 + 18 * ch) = 1;
}

// ------------------------------------------------------------------ OPN / PSG register write (sub_1001B220)

void Opx4::writeOpn(int reg, int val) {
    int a4 = reg & 0xFF, a5 = val & 0xFF;
    B(a4 + 42) = (uint8_t)a5;
    if (a4 >= 0x30) {
        if (a4 < 0xA0) {
            if ((a4 & 3) != 3) {
                int v9 = ((a4 >> 2) & 3) + 4 * (a4 & 3);
                switch (a4 & 0xF0) {
                case 0x30: r3790(v9, a5); break;
                case 0x40: r3810(v9, a5); break;
                case 0x50: r3840(v9, a5); break;
                case 0x60: r38A0(v9, a5); break;
                case 0x70: r3900(v9, a5 & 0x1F); break;
                case 0x80: r3960(v9, a5); break;
                default: break;
                }
            }
        } else if ((a4 & 3) != 3) {
            int v10 = a4 & 3;
            switch (a4 & 0xFC) {
            case 0xA0: r3410(v10, a5 | ((B(a4 + 46) & 0x3F) << 8)); break;
            case 0xA8: r34B0(FA30(a4) + 4, a5 | ((B(a4 + 46) & 0x3F) << 8)); break;
            case 0xB0: r3580(v10, a5); break;
            case 0xB4: if (D(1) == 0) a5 |= 0xC0; r3520(v10, a5); break;
            default: break;
            }
        }
        return;
    }
    switch (a4) {
    case 0: case 2: case 4: {
        int v14 = a4 >> 1;
        uint32_t v15 = (uint32_t)((a5 + (B(a4 + 43) << 8)) & 0xFFF);
        if (v15 == 0) v15 = 1;
        D(1230 + v14) = (int32_t)((uint32_t)D(9) / v15);
        if ((uint32_t)D(1230 + v14) > 0x2000000u) D(1230 + v14) = 0;
        break;
    }
    case 1: case 3: case 5: {
        int v12 = a4 >> 1;
        uint32_t v13 = (uint32_t)((B(a4 + 41) + (a5 << 8)) & 0xFFF);
        if (v13 == 0) v13 = 1;
        D(1230 + v12) = (int32_t)((uint32_t)D(9) / v13);
        if ((uint32_t)D(1230 + v12) > 0x2000000u) D(1230 + v12) = 0;
        break;
    }
    case 6: {
        uint32_t v11 = (uint32_t)(a5 & 0x1F);
        if (v11 == 0) v11 = 1;
        D(1233) = (int32_t)(((uint32_t)D(9) / v11) >> 1);
        break;
    }
    case 8: case 9: case 10:
        D(1230 + a4) = kTab_D620[2 * (a5 & 0xF)];
        D(1234 + a4) = (a5 & 0x10) ? 1241 : (1230 + a4);
        break;
    case 0xB: case 0xC: {
        D(1234) = (int32_t)(uint16_t)(B(53) | (B(54) << 8));
        if (D(1234) == 0) D(1234) = 1;
        D(1234) = (int32_t)(((uint32_t)(D(9) / D(1234))) >> 1);
        break;
    }
    case 0xD:
        D(1229) = 0;
        D(1237) = a5 & 0xF;
        break;
    case 0x22:
        if (a5 & 8) D(1221) = kTab_AEC0[a5 & 7];
        else        { D(1221) = 0; D(1220) = 0; D(1219) = 0; }       // sub_100039D0(0); sub_10003A10()
        break;
    case 0x27: r35F0(a5 >> 6); break;
    case 0x28: if ((a5 & 3) != 3) r3660(a5 & 3, a5 >> 4); break;
    default: break;
    }
}

// ------------------------------------------------------------------ OPM register write (sub_1001AE40)

void Opx4::opmWrite(int reg, int val) {
    int a4 = reg & 0xFF, a5 = val & 0xFF;
    B(a4 + 298) = (uint8_t)a5;
    switch (a4) {
    case 1: if (a5 & 2) D(1220) = 0; return;
    case 8: r3660(a5 & 7, a5 >> 3); return;
    case 0xF: D(140) = (a5 & 0x80) ? (int32_t)(0x400000 / (32 * (32 - (a5 & 0x1F)))) : 0; return;
    case 0x18: D(1221) = ((a5 & 0xF) + 16) << 13 >> (15 - (a5 >> 4)); return;
    case 0x19: r33D0((int8_t)a5); return;
    case 0x1B: D(1224) = a5 & 3; return;
    default: break;
    }
    if (a4 < 0x20) return;                                      // all other registers below 0x20 are ignored
    if (a4 >= 0x40) {
        int v9 = ((a4 >> 3) & 3) + 4 * (a4 & 7);
        switch (a4 & 0xE0) {
        case 0x40: r3790(v9, a5); break;
        case 0x60: r3810(v9, a5); break;
        case 0x80: r3840(v9, a5); break;
        case 0xA0: r38A0(v9, a5); break;
        case 0xC0: r3900(v9, a5); break;
        case 0xE0: r3960(v9, a5); break;
        default: break;
        }
    } else {
        int v10 = a4 & 7;
        switch (a4 & 0xF8) {
        case 0x20: r3580(v10, a5); r3740(v10, a5 >> 6); break;
        case 0x28: case 0x30:
            r3340(v10, (int8_t)B(v10 + 338), B(v10 + 346));
            break;
        case 0x38: r32D0(v10, a5); break;
        default: break;
        }
    }
}

// ------------------------------------------------------------------ operator routing (sub_10019F30)

void Opx4::connect(int a2, int a3) {
    int c = 1075 + 18 * a2;
    if (D(c) == a3) return;
    D(c) = a3;
    int b = 116 * a2;                       // operator-block offset of this channel in dwords
    int o0 = b + 163, o1 = b + 192, o2 = b + 221, o3 = b + 250;     // operator output cells
    const int X = DUMMY;
    int p226, p197, p198, p255, p256, q89, q90, q91, q92;
    switch (a3 & 7) {
    case 0: p226 = o0; p197 = o2; p198 = X;  p255 = o1; p256 = X;  q89 = o3; q90 = X;  q91 = X;  q92 = X;  break;
    case 1: p226 = X;  p197 = o0; p198 = o2; p255 = o1; p256 = X;  q89 = o3; q90 = X;  q91 = X;  q92 = X;  break;
    case 2: p226 = X;  p197 = o2; p198 = X;  p255 = o1; p256 = o0; q89 = o3; q90 = X;  q91 = X;  q92 = X;  break;
    case 3: p226 = o0; p197 = X;  p198 = X;  p255 = o1; p256 = o2; q89 = o3; q90 = X;  q91 = X;  q92 = X;  break;
    case 4: p226 = o0; p197 = X;  p198 = X;  p255 = o1; p256 = X;  q89 = o2; q90 = o3; q91 = X;  q92 = X;  break;
    case 5: p226 = o0; p197 = o0; p198 = X;  p255 = o0; p256 = X;  q89 = o1; q90 = o2; q91 = o3; q92 = X;  break;
    case 6: p226 = o0; p197 = X;  p198 = X;  p255 = X;  p256 = X;  q89 = o1; q90 = o2; q91 = o3; q92 = X;  break;
    default:p226 = X;  p197 = X;  p198 = X;  p255 = X;  p256 = X;  q89 = o0; q90 = o1; q91 = o2; q92 = o3; break;
    }
    D(b + 226) = p226; D(b + 197) = p197; D(b + 198) = p198; D(b + 255) = p255; D(b + 256) = p256;
    D(18 * a2 + 1089) = q89; D(18 * a2 + 1090) = q90; D(18 * a2 + 1091) = q91; D(18 * a2 + 1092) = q92;
}

// ------------------------------------------------------------------ rates and phase increments

int Opx4::rateCell(int a1) {                                    // sub_1001A510
    if (a1 > 63) a1 = 63;
    if ((a1 >> 2) >= 12) return FB80(a1) | (FC80(a1 >> 2) << 24);
    if (a1 <= 2) return 251658240;
    return FC30(a1 & 3) | (FC80(a1 >> 2) << 24);
}

void Opx4::calcPhaseInc(int opb, int chb) {                     // sub_10019E00 (OPN) / sub_10019E90 (OPM)
    int m = D(1);
    for (int i = 0; i < 4; i++) {
        int o = opb + 29 * i;
        if (m == 1 || m == 4) {
            uint32_t f = (uint32_t)D(chb + 3);
            int e = kExpTab[D(o + 8) + (uint16_t)f] << ((f >> 16) & 0xFF);
            uint32_t r = (uint32_t)mul32(D(o + 7), add32(kLinTab[D(o + 10) + (D(chb + 2) >> 2)], e));
            D(o + 14) = (int32_t)(r >> 1);
        } else {
            int v3 = (D(chb + 9) != 0) ? D(o + 11) : D(chb + 3);
            D(o + 14) = mul32(D(o + 7), add32(kLinTab[D(o + 10) + (D(chb + 2) >> 2)], v3));
        }
    }
}

void Opx4::updateChannel(int opb, int chb) {                    // sub_1001A580
    calcPhaseInc(opb, chb);
    for (int i = 0; i < 4; i++) {
        int o = opb + 29 * i;
        int v4 = D(chb + 2) >> (5 - (D(o + 6) & 0xFF));
        int v5 = D(o + 0);  if (v5 != 0) v5 += v4;
        D(o + 23) = rateCell(v5) - 0x4000000;
        int v6 = D(o + 1);  if (v6 != 0) v6 += v4;
        D(o + 24) = rateCell(v6);
        int v7 = D(o + 2);  if (v7 != 0) v7 += v4;
        D(o + 25) = rateCell(v7);
        int v8 = D(o + 3);  if (v8 != 0) v8 += v4;
        D(o + 26) = rateCell(v8);
    }
}

// ------------------------------------------------------------------ LFO (sub_10003A40)

void Opx4::lfoUpdate() {
    int v5 = 0, v4 = 0;
    if (D(1221) != 0) {
        D(1220) = add32(D(1220), 8 * D(1221));
        int v3 = D(1220) >> 12;
        switch (D(1224)) {
        case 0:
            if ((D(1220) & 0x8000000) != 0) {
                v4 = (((D(1220) >> 12) & 0x7FFF) >> 1) - 0x4000;
                v5 = (int)(uint16_t)v3 >> 1;
            } else {
                v4 = ((D(1220) >> 12) & 0x7FFF) >> 1;
                v5 = (int)(uint16_t)v3 >> 1;
            }
            break;
        case 1:
            if ((D(1220) & 0x8000000) != 0) { v4 = -16384; v5 = 0x7FFF; }
            else                            { v4 = 0x3FFF;  v5 = 0; }
            break;
        case 2:
            switch ((D(1220) >> 26) & 3) {
            case 0: v4 = (D(1220) >> 12) & 0x3FFF;          v5 = (D(1220) >> 12) & 0x7FFF; break;
            case 1: v4 = 0x4000 - ((D(1220) >> 12) & 0x3FFF); v5 = (D(1220) >> 12) & 0x7FFF; break;
            case 2: v4 = -((D(1220) >> 12) & 0x3FFF);       v5 = 0x8000 - ((D(1220) >> 12) & 0x7FFF); break;
            default:v4 = ((D(1220) >> 12) & 0x3FFF) - 0x4000; v5 = 0x8000 - ((D(1220) >> 12) & 0x7FFF); break;
            }
            break;
        case 3:
            v4 = 8 * g_noise[(v3 >> 9) & 0x3FFF];
            v5 = v4 + 0x4000;
            break;
        default: break;
        }
        D(143) = (D(1222) * v4) >> 14;
        D(142) = (D(1223) * v5) >> 14;
    }
}

// ------------------------------------------------------------------ PSG (sub_10019990)

void Opx4::psg(int32_t* a2, int a3) {
    int v10 = (D(1) == 5) ? -2 : 1;
    do {
        int v6 = (int)(((uint32_t)D(1229) >> 25) & 0x1F);
        if ((D(1237) & 8) != 0) {
            int v5 = D(1237) & 3;
            if (v5 == 1)      { if ((uint32_t)D(1229) >> 30 != 0) v6 = 31; }
            else if (v5 == 2) { if ((D(1229) & 0x40000000) != 0) v6 = 31 - v6; }
            else if (v5 == 3) { if ((uint32_t)D(1229) >> 30 != 0) v6 = 0; }
            if ((D(1237) & 4) == 0) v6 = 31 - v6;
        } else if ((D(1237) & 4) != 0) {
            v6 &= -(((uint32_t)D(1229) >> 30) == 0);
        } else {
            v6 = ((uint32_t)D(1229) >> 30 != 0) ? 0 : 31 - v6;
        }
        D(1241) = kTab_D620[v6];
        if ((uint32_t)D(1229) >> 30 != 0) {
            if ((D(1237) & 1) == 0 && (D(1237) & 8) != 0) D(1229) = (int32_t)((uint32_t)D(1229) & ~0x80000000u);
            else                                          D(1229) = 0x40000000;
        }
        int v8 = D(1236) | B(49);
        int v9 = v8 & (v8 >> 3);
        int p0 = d[D(1244)], p1 = d[D(1243)], p2 = d[D(1242)];     // **(this+4976), **(this+4972), **(this+4968)
        int v7 = (p0 & -((v9 >> 2) & 1)) + (p1 & -((v9 >> 1) & 1)) + (p2 & -(v9 & 1)) - v10 * ((p0 + p1 + p2) >> 1);
        D(1225) = add32(D(1225), D(1230));
        D(1226) = add32(D(1226), D(1231));
        D(1227) = add32(D(1227), D(1232));
        D(1228) = add32(D(1228), D(1233));
        D(1229) = add32(D(1229), D(1234));
        D(1236) ^= (D(1225) & 0x2000000) != 0;
        D(1236) ^= 2 * ((D(1226) & 0x2000000) != 0);
        D(1236) ^= 4 * ((D(1227) & 0x2000000) != 0);
        D(1225) &= 0x1FFFFFF; D(1226) &= 0x1FFFFFF; D(1227) &= 0x1FFFFFF;
        if ((D(1228) & 0x2000000) != 0) {
            D(1228) &= 0x1FFFFFF;
            if ((D(1235) & 1) != 0) D(1235) ^= 0x8302;
            D(1235) = (int32_t)((uint32_t)D(1235) >> 1);
            D(1236) = (-(D(1235) & 1) & 0x38) | (D(1236) & 7);
        }
        a2[0] = add32(a2[0], v7);
        a2[1] = add32(a2[1], v7);
        a2 += 2;
        --a3;
    } while (a3 != 0);
}

// ------------------------------------------------------------------ sample generator (sub_1001A6E0)

void Opx4::generate(int32_t* a2, int a3) {
    if (a3 <= 0) return;
    // B(5000) != 0 selects a fast-forward path (sub_10019760) that normal playback never enables.
    for (int i = 0; i < 8; i++) {
        if (D(1083 + 18 * i) != 0) {
            updateChannel(147 + 116 * i, 1075 + 18 * i);
            D(1083 + 18 * i) = 0;
        }
    }
    switch (D(1)) {
    case 0: case 2: case 4: case 5: psg(a2, a3); break;
    default: break;
    }
    while (D(1253) != 0) {
        --D(1253);
        --a3;
        a2[0] = add32(a2[0], D(1251));
        a2[1] = add32(a2[1], D(1252));
        a2 += 2;
    }
    if (a3 == 0) return;

    int v23 = 0, v19 = 0, v21 = 0;
    for (;;) {
        do {
            if ((D(1219) & 7) == 0) {
                lfoUpdate();
                for (int j = 0; j < 8; j++) {
                    int c = 1075 + 18 * j;
                    int v12 = 0, v13 = 0;
                    if (D(1) == 1) {
                        if (D(c + 5) != 0) v13 = kEnvRateOPM[(mul32(D(c + 5), D(143))) >> 7];
                        if (D(c + 4) != 0) v12 = (D(c + 4) * D(142)) >> 8;
                    } else {
                        v13 = ((D(c + 5) * D(143)) >> 7) + 2048;
                        v12 = (D(c + 4) * D(142)) >> 8;
                    }
                    D(c + 12) = v13;
                    D(c + 13) = v12;
                }
            }
            uint32_t v15 = (uint32_t)D(2);
            for (int op = 0; op < 32; op++, v15 >>= 1) {
                int o = 147 + 29 * op;
                int chb = D(o + 28);
                if (v15 & 1) {
                    if (D(o + 13) == 4) {
                        D(o + 16) = 0;
                    } else {
                        if (--D(o + 20) <= 0) {
                            int v9 = D(o + D(o + 13) + 23) & 0xFFFFF;
                            switch (D(o + 13)) {
                            case 0:
                                D(o + 18) >>= 10;
                                D(o + 18) += mul32(v9, -D(o + 18)) >> 20;
                                D(o + 18) = (int32_t)((uint32_t)D(o + 18) << 10);
                                if (D(o + 18) <= 0) {
                                    D(o + 13) = 1;
                                    D(o + 18) = 0;
                                    if (D(o + 4) == 0) D(o + 13) = 2;
                                }
                                break;
                            case 1:
                                D(o + 18) = add32(D(o + 18), v9);
                                if (D(o + 18) >= D(o + 4)) { D(o + 13) = 2; D(o + 18) = D(o + 4); }
                                break;
                            case 2: case 3:
                                D(o + 18) = add32(D(o + 18), v9);
                                break;
                            case 4:
                                D(o + 18) = 4190208;
                                break;
                            default: break;
                            }
                            if (D(o + 18) > 4190208) { D(o + 13) = 4; D(o + 18) = 4190208; }
                            D(o + 15) = D(o + 18) / 4096;
                            D(o + 20) = 1 << (((uint32_t)D(o + D(o + 13) + 23) >> 24) & 0xFF);
                        }
                        int v11 = (D(o + 9) & D(chb + 13)) + D(o + 5) + D(o + 15);
                        if (v11 > 1023) v11 = 1023;
                        D(o + 12) = add32(D(o + 12), (int32_t)((uint32_t)mul32(D(chb + 12), D(o + 14)) >> 10));
                        int idx = (W(D(o + 22)) + W(D(o + 21)) + (uint16_t)((uint32_t)D(o + 12) >> 13)) & 0x3FF;
                        D(o + 16) = (kSine[idx] * ((v11 & 0x3F) ^ 0x7F)) >> ((v11 >> 6) + 5);
                    }
                } else {
                    D(o + 16) = 0;
                }
            }
            if (D(140) != 0) {
                int v8 = 1023 - (D(1051) + D(1061));
                if (v8 < 0) v8 = 0;
                D(1062) = (v8 * g_noise[(uint32_t)D(141) >> 16 & 0x3FFF]) >> 10;
                D(141) = add32(D(141), D(140));
            }
            int v7 = 147;
            for (int k = 1075; k < 1219; k += 18) {
                D(k + 10) = (D(v7 + 16) + D(k + 11)) >> D(k + 1);
                D(k + 11) = D(v7 + 16);
                int v5 = d[D(k + 17)] + d[D(k + 16)] + d[D(k + 15)] + d[D(k + 14)];
                v21 += v5 & D(k + 6);
                v19 += v5 & D(k + 7);
                v7 += 116;
            }
            ++v23;
            ++D(1219);
            D(7) = add32(D(7), D(8));
        } while ((D(7) >> 14) == 0);

        int v4 = D(7) >> 14;
        D(7) &= 0x3FFF;
        int v22 = (v21 / v23 * D(4)) >> 10;
        int v20 = (v19 / v23 * D(4)) >> 10;
        bool restart = false;
        for (;;) {
            a2[0] = add32(a2[0], v22);
            a2[1] = add32(a2[1], v20);
            a2 += 2;
            if (--a3 <= 0) break;
            if (--v4 == 0) { v23 = 0; v19 = 0; v21 = 0; restart = true; break; }
        }
        if (!restart) {
            D(1253) = v4 - 1;
            D(1251) = v22;
            D(1252) = v20;
            return;
        }
    }
}

}  // namespace opnmml
