// Port of the wuopn.dll sound chip emulator "COpx4" (FM operators + PSG).
//
// The original keeps all state in one flat struct addressed as `this + N` dwords.  This port keeps
// exactly that layout (a flat int32 image, same dword offsets as the Hex-Rays output) so every function
// can be checked line by line against the decompile.  Pointer fields of the original are stored here as
// dword *indices* into the same image; DUMMY is the always-zero cell the original calls unk_1003DA74.
//
//   D(0)  vtable   D(1) mode     D(2) active-operator bit mask   D(3) clock      D(4) output gain (1024)
//   D(5)  chip rate (clock/72 for OPN, clock/64 for OPM)         D(6) output rate
//   D(7)  sample accumulator     D(8) accumulator step (14-bit)  D(9) PSG/LFO phase scale
//   operator i : dwords 147 + 29*i      (32 operators, 116 bytes each)
//   channel  j : dwords 1075 + 18*j     (8 channels,   72 bytes each)
//   PSG state  : dwords 1225..1253
#pragma once
#include "seq.hpp"
#include <cstdint>
#include <vector>

namespace opnmml {

class Opx4 : public IChip {
public:
    Opx4(int outRate, int clock);                // sub_1001BC80
    void setMode(int mode);                      // sub_1001BB20 : 0/5 OPN, 1/4 OPM, 2/3 OPN variants (resets the chip)

    int  mode() const override { return d[1]; }  // sub_1001A470
    void write(int reg, int val) override;       // sub_1001B7A0 (port 0)
    void writeOpn(int reg, int val) override;    // sub_1001B220 (port 0)
    int  read(int reg) override;                 // sub_10019DC0 (port 0)

    // sub_1001A6E0 : add n stereo samples (interleaved int32) into buf
    void generate(int32_t* buf, int n);

    // seed of the noise/LFO table (the original fills it with the C runtime's rand(); see opx4.cpp)
    static void setNoiseSeed(uint32_t seed);

private:
    std::vector<int32_t> d;
    static const int DUMMY = 1300;

    int32_t&  D(int i) { return d[i]; }
    uint8_t*  bytes()  { return reinterpret_cast<uint8_t*>(d.data()); }
    uint8_t&  B(int off) { return bytes()[off]; }
    uint16_t  W(int idx) { return (uint16_t)d[idx]; }       // low word of a dword cell

    void reset();                                // sub_1001B7F0
    void opmWrite(int reg, int val);             // sub_1001AE40
    void connect(int ch, int alg);               // sub_10019F30
    void updateChannel(int opb, int chb);        // sub_1001A580
    void calcPhaseInc(int opb, int chb);         // sub_10019E00 / sub_10019E90
    int  rateCell(int a1);                       // sub_1001A510
    void lfoUpdate();                            // sub_10003A40
    void psg(int32_t* buf, int n);               // sub_10019990

    // register helpers (names are the original function addresses)
    void r33D0(int8_t v);  void r3410(int ch, int v); void r34B0(int op, int v);
    void r3520(int ch, int v); void r3580(int ch, int v); void r35F0(int a3);
    void r3660(int ch, int keys); void r3740(int ch, int v);
    void r3790(int op, int v); void r3810(int op, int v); void r3840(int op, int v);
    void r38A0(int op, int v); void r3900(int op, int v); void r3960(int op, int v);
    void r32D0(int ch, int v); void r3340(int ch, int8_t kc, int kf);
};

}  // namespace opnmml
