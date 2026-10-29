// songlen - see songlen.hpp.
#include "songlen.hpp"
#include "seq.hpp"
#include <cstdlib>

namespace opnmml {
namespace {

// A chip that does nothing; it only keeps the register shadow because the sequencer reads PSG register 7.
struct NullChip : IChip {
    int shadow[32] = {0};
    int  mode() const override { return 0; }
    void write(int reg, int val) override { if (reg >= 0 && reg < 32) shadow[reg] = val; }
    void writeOpn(int reg, int val) override { write(reg, val); }
    int  read(int reg) override { return (reg >= 0 && reg < 32) ? shadow[reg] : 0; }
};

}  // namespace

bool measureSong(const Song& song, int num, SongTiming& out, long maxMs) {
    out = SongTiming();
    NullChip chip;
    Sequencer seq(&chip);
    seq.load(&song);
    seq.start(num);
    Sequencer* slots[1] = {&seq};

    bool active[11], done[11], sawNote[11] = {false};
    int  active_count = 0;
    for (int c = 0; c < 11; c++) {
        active[c] = seq.ch[c].base != nullptr && seq.ch[c].state != 0;
        done[c] = !active[c];
        if (active[c]) active_count++;
    }
    if (active_count == 0) return false;

    int remaining = active_count;
    for (long k = 1; k <= maxMs && remaining > 0; k++) {
        step1ms(slots, 1, &chip, 0);                      // k steps of 1 ms have been completed
        for (int c = 0; c < 11; c++) {
            if (done[c]) continue;
            const SeqCh& h = seq.ch[c];
            ChannelTiming& t = out.ch[c];
            if (h.len != 0) sawNote[c] = true;                 // a note or rest is being timed
            if (t.loopStartMs < 0 && h.loopOff != 0) t.loopStartMs = k;
            if (t.endMs < 0) {
                if (h.passes >= 1 && h.state != 0) { t.endMs = k; t.loops = true; }                      // jumped back to the loop point
                else if (h.passes >= 1)            { t.endMs = k; done[c] = true; remaining--; }         // reached the end and stopped
                else if (h.state == 0)             { t.endMs = k; t.fadeEnded = true; done[c] = true; remaining--; }   // cut by a fade-out
            } else if (t.loops && h.passes >= 2) {
                t.loopMs = k - t.endMs;
                done[c] = true; remaining--;
            }
        }
    }
    out.truncated = remaining > 0;

    // ---- the song as a whole ----
    bool first = true;
    long loopRef = 0; bool loopsRef = false;
    for (int c = 0; c < 11; c++) {
        ChannelTiming& t = out.ch[c];
        if (!active[c] || t.endMs < 0) continue;
        t.used = sawNote[c];                              // a channel without notes reaches its terminator at the first tick
        if (!t.used) continue;
        if (t.endMs > out.onceMs) out.onceMs = t.endMs;
        if (t.fadeEnded) out.fadeEnd = true;
        if (t.loops) {
            out.loops = true;
            if (t.loopStartMs > out.introMs) out.introMs = t.loopStartMs;
            if (t.loopMs > out.loopMs) out.loopMs = t.loopMs;
        }
        if (first) { loopRef = t.loopMs; loopsRef = t.loops; first = false; }
        else if (t.loops != loopsRef || (t.loops && labs(t.loopMs - loopRef) > 2)) out.uniform = false;   // the lead-ins may differ; the loops must not
    }
    return !first;
}

}  // namespace opnmml
