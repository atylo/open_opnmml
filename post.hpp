// ================================================================================================================
// post - optional processing of the rendered audio (NOT in the original plugin).
//
// The engine's output is left exactly as the original makes it.  These steps are for people who want a file that sounds like an official
// release or a mastered recording; they are applied to the finished 16-bit samples, in this order:
//   1. DC filter   the PSG output has a DC offset (the signal sits above or below zero); a 15 Hz high-pass removes it (inaudible)
//   2. equaliser   "release": the gentle curve that the public release of the soundtrack has compared with the plain engine output
//                  (+1.4 dB below 220 Hz, -0.6 dB around 5.5 kHz, +1.7 dB above 11 kHz; measured, see calib/match_release.py)
//   3. gain        a fixed factor, or whatever makes the loudest sample reach full scale (the caller finds that factor with a first pass)
//   4. fade-out    optional: the level falls from 1 to 0 over a given stretch of the output, silence after it.  The shape is the one measured on
//                  the fade of the public release: close to (1-x)^2 at first, steeper towards the end (-39 dB after 85 % of the time)
// ================================================================================================================
#pragma once
#include <cstdint>

namespace opnmml {

class PostChain {
public:
    PostChain(int rate, bool dcBlock, bool eqRelease);
    void   setGain(double linear) { gain = linear; }
    // Fade out: full level before output frame `start`, then the measured curve with x going 0..1 over `length` frames, then silence.
    void   setFade(long start, long length) { fadeStart = start; fadeLen = length; }
    bool   active() const { return dc || eq || gain != 1.0 || fadeLen > 0; }
    // Process `frames` stereo frames (interleaved) in place.  Notes the peak before the gain and counts clipped samples.
    void   process(int16_t* io, int frames);
    double peakBeforeGain() const { return peakPre; }   // largest |sample| after the DC filter and equaliser, before the gain
    int    peakOut() const { return peakPost; }         // largest |sample| written
    long   clippedSamples() const { return clipped; }

private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1[2] = {0, 0}, x2[2] = {0, 0}, y1[2] = {0, 0}, y2[2] = {0, 0};
                    double run(int ch, double x) { double y = b0 * x + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
                                                   x2[ch] = x1[ch]; x1[ch] = x; y2[ch] = y1[ch]; y1[ch] = y; return y; } };
    static const int NSEC = 3;
    bool    dc, eq;
    double  gain = 1.0;
    long    fadeStart = 0, fadeLen = 0, pos = 0;               // pos = number of frames processed so far
    Biquad  hp, sec[NSEC];
    double  peakPre = 0;
    int     peakPost = 0;
    long    clipped = 0;
};

}  // namespace opnmml
