#!/usr/bin/env python3
"""
check_mimic.py - how close is mmlplay's  -d -R  output to the public release of the same song?

    python check_mimic.py RELEASE.flac MIMIC.wav

MIMIC.wav is made with:  mmlplay SONG.mus MIMIC.wav -d -R      (SONG = bgm01_x1.mus, the song the settings were measured on)
Reports the length, where the first sound is, the fine alignment, the level (rms) in 4 s windows, the tone per band over the song, the last
non-zero sample (the end of the fade) and the level ratio around the fade.
"""
import sys, os
import numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_music import load, xlag
sr = 44100

def main():
    if len(sys.argv) < 3: print(__doc__); return 2
    r = load(sys.argv[1]).mean(axis=1); m = load(sys.argv[2]).mean(axis=1)
    print("length: release %.3f s (%d frames), ours %.3f s (%d frames)" % (len(r) / sr, len(r), len(m) / sr, len(m)))
    env = lambda x: np.convolve(np.abs(x), np.ones(441) / 441, 'same')
    first = lambda x: np.argmax(env(x) > 500) / sr
    print("first sound (envelope above 500): release %.3f s, ours %.3f s" % (first(r), first(m)))
    l, c = xlag(m[10 * sr:11 * sr], r[10 * sr - 3000:11 * sr + 3000]); print("fine offset around 10 s (ours -> release): %+.2f ms (correlation %.2f)" % ((l - 3000) / 44.1, c))
    print("\nrms per 4 s: release / ours / ratio")
    for t in range(0, 84, 6):
        a = r[t * sr:(t + 4) * sr].std(); b = m[t * sr:(t + 4) * sr].std(); print("  %2d-%2d s   %6.0f  %6.0f   %.3f" % (t, t + 4, a, b, b / (a + 1e-9)))
    print("last non-zero sample: release %.3f s, ours %.3f s" % (np.flatnonzero(np.abs(r) > 0)[-1] / sr, np.flatnonzero(np.abs(m) > 0)[-1] / sr))
    bands = [(50, 300), (300, 1000), (1000, 3000), (3000, 8000), (8000, 12000), (12000, 16000), (16000, 19000), (19000, 21500)]
    f, pr = signal.welch(r[sr:84 * sr], fs=sr, nperseg=16384); f, pm = signal.welch(m[sr:84 * sr], fs=sr, nperseg=16384)
    q = np.array([np.sqrt(pm[(f >= lo) & (f < hi)].sum() / pr[(f >= lo) & (f < hi)].sum()) for lo, hi in bands])
    print("\nlevel ratio ours / release per band (1 = identical): " + " ".join("%.2f" % v for v in q) + "   spread %.1f dB" % (20 * np.log10(q.max() / q.min())))
    print("fade: ratio ours/release rms per second around the fade: " + " ".join("%d:%.2f" % (t, m[t * sr:(t + 1) * sr].std() / (r[t * sr:(t + 1) * sr].std() + 1e-9)) for t in range(74, 85)))

if __name__ == '__main__':
    sys.exit(main())
