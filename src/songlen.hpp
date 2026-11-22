// ================================================================================================================
// songlen - how long is a song?
//
// A .mus file does not store its length.  The length follows from running the music, because it depends on the tempo
// commands (t) and on a fade-out (F) in the song, so this module simply runs the real sequencer against a chip that
// ignores everything (no audio is generated, a 5-minute song takes a few milliseconds) and watches every channel:
//
//   * a channel is finished when it reaches its terminator.  If it has a loop point (L) it jumps back and plays on; otherwise it
//     stops.  The moment of the first terminator is the end of its first pass.
//   * the second terminator of a looping channel gives the length of one trip round the loop.
//
// Times are in milliseconds of sequencer time (the sequencer works in 1 ms steps).  The tails of the last notes (the
// release of an FM voice) are not included: they are sound produced after the music has ended.
// ================================================================================================================
#pragma once
#include "mmlcomp.hpp"

namespace opnmml {

struct ChannelTiming {
    bool used        = false;   // the channel has music (it takes some time to play)
    bool loops       = false;   // it reaches a loop point (L) and plays on after the first pass
    bool fadeEnded   = false;   // it was stopped by a fade-out (F or OPNStopSE), not by reaching its end
    long endMs       = -1;      // end of the first pass (the terminator is reached)
    long loopStartMs = -1;      // loops only: when the loop point (L) is passed in the first pass
    long loopMs      = -1;      // loops only: length of one trip round the loop (-1 = not measured)
};

struct SongTiming {
    ChannelTiming ch[11];
    bool truncated = false;     // the time limit was reached before everything was measured
    bool loops     = false;     // at least one channel loops
    bool uniform   = true;      // all channels with music loop by the same length (or none loops): the parts stay together.
                                // Their lead-ins (loopStartMs) may differ: a channel can have a few notes before its L.
    bool fadeEnd   = false;     // the song ends by a fade-out
    long onceMs    = 0;         // playing the song once: until the longest channel has finished its first pass.  (A channel with a
                                // shorter lead-in has already started its loop again by then; with loops that line up this is
                                // the same point as introMs + loopMs.)
    long introMs   = 0;         // loops only: time before the loop starts
    long loopMs    = 0;         // loops only: length of the loop (of the longest looping channel)
};

// Measure song number `num` of `song` (a compiled slot).  maxMs limits the simulated time.  Returns false when there is no
// music at all.
bool measureSong(const Song& song, int num, SongTiming& out, long maxMs = 3600000);

}  // namespace opnmml
