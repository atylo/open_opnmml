#!/usr/bin/env python3
"""
compare_detail.py - the detailed comparison of a recording of calib_fm3.mus with the port's render (calib_ours_pc88.wav), when the frequency
response is already known to be flat (see compare_recording.py).  It answers: is the recording the same SOUND as our render?

    python compare_detail.py RECORDING.wav [REFERENCE.wav]

  (a) tuning: the frequency of the high FM sines in the recording divided by ours = the ratio of the two audio clocks (0 ppm = same clock)
  (b) the time from the first to the last tone in both files (the music follows the frame count, so this is the same ratio again)
  (c) waveform: after the best sample lag and scaling, how much of the recording is not explained by our render.  Square waves (PSG)
      are exact when the result is far below -100 dB.  For sines the residual grows with frequency because the lag is a whole number
      of samples while the real timing offset is a fraction of one (it is the alignment that limits it, not the sound)
  (d) the level and DC offset of the silence between the tones
  (e) the PSG noise: is our noise sequence found in the recording?  ~0.05 = unrelated noise, 0.8-1.0 = the same sequence (possibly at
      another position: the starting point of the game's noise depends on how long the plugin has been running)
"""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_recording import load, find_grid, GRID

HERE = os.path.dirname(os.path.abspath(__file__))

def xc_peak(a, b):
    """highest normalised cross-correlation of a inside b, and where"""
    n = len(a); L = len(b) + n
    cc = np.fft.irfft(np.fft.rfft(b, L) * np.conj(np.fft.rfft(a, L)), L)[:len(b) - n + 1]
    r = cc / np.sqrt(np.dot(a, a) * np.convolve(b ** 2, np.ones(n), 'valid') + 1e-9)
    i = int(np.argmax(r)); return r[i], i

def peak_freq(seg, sr):
    n = len(seg); X = np.abs(np.fft.rfft(seg * np.hanning(n))); i = int(np.argmax(X)); y = np.log(X[i - 1:i + 2] + 1e-12)
    return (i + 0.5 * (y[0] - y[2]) / (y[0] - 2 * y[1] + y[2])) * sr / n

def main():
    if len(sys.argv) < 2: print(__doc__); return 2
    ev = json.load(open(os.path.join(HERE, 'calib_events.json')))
    refp = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'calib_ours_pc88.wav')
    rec, sr = load(sys.argv[1]); ref, _ = load(refp)
    tc, tr = find_grid(rec, sr, len(ev)), find_grid(ref, sr, len(ev))
    # (a)
    est = []
    for k, d in enumerate(ev):
        if d['kind'] != 'fm' or d['f'] < 6000: continue
        f = [peak_freq(x[int((t + k * GRID + 0.3) * sr):int((t + k * GRID + 0.9) * sr), 0], sr) for x, t in ((ref, tr), (rec, tc))]
        est.append(f[1] / f[0] - 1)
    print("(a) tuning, recording / ours, from the FM sines above 6 kHz: %+.1f ppm" % (np.mean(est) * 1e6))
    # (b)
    def onset(x, t, k):
        a = int((t + k * GRID - 0.1) * sr); b = int((t + k * GRID + 0.4) * sr)
        thr = 0.5 * np.abs(x[int((t + k * GRID + 0.4) * sr):int((t + k * GRID + 0.9) * sr)]).max()
        return (a + int(np.argmax(np.abs(x[a:b]).max(axis=1) > thr))) / sr
    dr = onset(rec, tc, len(ev) - 1) - onset(rec, tc, 0); do = onset(ref, tr, len(ev) - 1) - onset(ref, tr, 0)
    print("(b) first to last tone: recording %.4f s, ours %.4f s (%+.1f ppm)" % (dr, do, (dr / do - 1) * 1e6))
    # (c)
    res = {}
    for k, d in enumerate(ev):
        if d['kind'] == 'noise': continue
        a = int((tr + k * GRID + 0.5) * sr); ra = ref[a:a + 2205, 0]
        c0 = int((tc + k * GRID + 0.5) * sr) - 1200; rb = rec[c0:c0 + 2205 + 2400, 0]
        r, i = xc_peak(ra, rb); seg = rb[i:i + 2205]; g = np.dot(ra, seg) / np.dot(ra, ra)
        res.setdefault(d['kind'], []).append((d['f'], 20 * np.log10(np.std(seg - g * ra) / np.std(seg) + 1e-12)))
    print("(c) what of the recording is not explained by our render (dB below the recording):")
    for kind, v in res.items():
        v.sort(); print("    %-3s: %s" % (kind, "  ".join("%d Hz:%.0f" % (round(f), r) for f, r in v)))
    # (d)
    def gaps(x, t):
        return np.array([(x[int((t + k * GRID + 1.3) * sr):int((t + k * GRID + 1.45) * sr), 0].std(), x[int((t + k * GRID + 1.3) * sr):int((t + k * GRID + 1.45) * sr), 0].mean()) for k in range(len(ev))])
    gr, gc = gaps(ref, tr), gaps(rec, tc)
    print("(d) between the tones: rms ours %.1f / recording %.1f, DC ours %.2f / recording %.2f" % (gr[:, 0].mean(), gc[:, 0].mean(), gr[:, 1].mean(), gc[:, 1].mean()))
    # (e)
    print("(e) PSG noise, correlation with our noise and the shift at which it is found:")
    for k, d in enumerate(ev):
        if d['kind'] != 'noise': continue
        a = int((tr + k * GRID + 0.30) * sr); ra = ref[a:a + int(0.10 * sr), 0]
        c0 = int((tc + k * GRID + 0.30) * sr) - int(0.6 * sr); rb = rec[c0:c0 + int(1.4 * sr), 0]
        r, i = xc_peak(ra, rb); print("    %-16s correlation %.3f at %+.1f ms" % (d['note'], r, (i - int(0.6 * sr)) / 44.1))

if __name__ == '__main__':
    sys.exit(main())
