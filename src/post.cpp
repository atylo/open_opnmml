#include "post.hpp"
#include <cmath>

namespace opnmml {
namespace {

const double PI = 3.14159265358979323846;

// Standard "audio EQ cookbook" sections (Robert Bristow-Johnson), normalised so that a0 = 1.
enum Kind { LOW_SHELF, PEAKING, HIGH_SHELF };
void design(PostChain* /*unused*/, double rate, Kind kind, double f0, double gainDb, double q, double& b0, double& b1, double& b2, double& a1, double& a2) {
    double A = std::pow(10.0, gainDb / 40.0), w = 2 * PI * f0 / rate, c = std::cos(w), s = std::sin(w), al = s / (2 * q), sq = 2 * std::sqrt(A) * al;
    double a0;
    switch (kind) {
    case LOW_SHELF:
        b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq);
        a0 = (A + 1) + (A - 1) * c + sq;       a1 = -2 * ((A - 1) + (A + 1) * c);    a2 = (A + 1) + (A - 1) * c - sq; break;
    case HIGH_SHELF:
        b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq);
        a0 = (A + 1) - (A - 1) * c + sq;       a1 = 2 * ((A - 1) - (A + 1) * c);      a2 = (A + 1) - (A - 1) * c - sq; break;
    default:
        b0 = 1 + al * A; b1 = -2 * c; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * c; a2 = 1 - al / A; break;
    }
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
}

// The measured fade of the release (the gain of the release divided by the gain of our own render):  x = time since the start of the fade /
// 8 s, g = level.
const double kFade[][2] = {
    {0.0000, 1.000}, {0.0300, 0.945}, {0.0925, 0.830}, {0.1550, 0.730}, {0.2175, 0.634}, {0.2800, 0.554}, {0.3425, 0.445}, {0.4050, 0.360},
    {0.4675, 0.288}, {0.5300, 0.225}, {0.5925, 0.158}, {0.6550, 0.107}, {0.7175, 0.065}, {0.7800, 0.033}, {0.8425, 0.011}, {0.9050, 0.004},
    {0.9675, 0.001}, {1.0000, 0.000}};

double fadeGain(double x) {                              // piecewise linear through the table
    const int n = (int)(sizeof kFade / sizeof kFade[0]);
    if (x <= 0) return 1.0;
    for (int i = 1; i < n; i++)
        if (x <= kFade[i][0]) return kFade[i - 1][1] + (kFade[i][1] - kFade[i - 1][1]) * (x - kFade[i - 1][0]) / (kFade[i][0] - kFade[i - 1][0]);
    return 0.0;
}

}  // namespace

PostChain::PostChain(int rate, bool dcBlock, bool eqRelease) : dc(dcBlock), eq(eqRelease) {
    if (dc) {                                            // first-order Butterworth high-pass at 15 Hz (bilinear transform)
        double k = std::tan(PI * 15.0 / rate);
        hp.b0 = 1 / (1 + k); hp.b1 = -1 / (1 + k); hp.b2 = 0; hp.a1 = (k - 1) / (k + 1); hp.a2 = 0;
    }
    if (eq) {                                            // fitted to the measured curve of the release (rms error 0.12 dB, worst 0.31 dB)
        design(nullptr, rate, LOW_SHELF,  223.0,  1.42, 0.7,  sec[0].b0, sec[0].b1, sec[0].b2, sec[0].a1, sec[0].a2);
        design(nullptr, rate, PEAKING,   5515.0, -0.56, 0.9,  sec[1].b0, sec[1].b1, sec[1].b2, sec[1].a1, sec[1].a2);
        design(nullptr, rate, HIGH_SHELF, 11225.0, 1.74, 0.7, sec[2].b0, sec[2].b1, sec[2].b2, sec[2].a1, sec[2].a2);
    }
}

void PostChain::process(int16_t* io, int frames) {
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            double x = io[2 * i + ch];
            if (dc) x = hp.run(ch, x);
            if (eq) for (int s = 0; s < NSEC; s++) x = sec[s].run(ch, x);
            double a = x < 0 ? -x : x; if (a > peakPre) peakPre = a;
            x *= gain;
            if (fadeLen > 0) {
                long p = pos + i;
                if (p >= fadeStart + fadeLen) x = 0;
                else if (p >= fadeStart) x *= fadeGain((double)(p - fadeStart) / (double)fadeLen);
            }
            long v = std::lround(x);
            if (v > 32767) { v = 32767; ++clipped; } else if (v < -32768) { v = -32768; ++clipped; }
            io[2 * i + ch] = (int16_t)v;
            int m = (int)(v < 0 ? -v : v); if (m > peakPost) peakPost = m;
        }
    }
    pos += frames;
}

}  // namespace opnmml
