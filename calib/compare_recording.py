#!/usr/bin/env python3
"""
compare_recording.py - compares a recording of the calibration signal with the reference (our render of calib_fm3.mus, or control_tones.wav).

    python compare_recording.py RECORDING.wav REFERENCE.wav [--json calib_events.json]

The 36 test events sit on a fixed grid (one every 1.5 s).  The tool finds the position of the grid in each file, so it does not matter how long
the recording starts before the sound or how much the two clocks drift.  For every event it measures the level over the middle of the tone
(0.3 s to 0.9 s after its start) and prints recording / reference:
  * FM sines and PSG squares: the amplitude of the fundamental (the exact frequency response of the chain at that frequency)
  * PSG noise: the RMS in octave bands
  * every event: the total RMS
A ratio of 1.00 means the recording has the level of the reference.  The ratios of the sines, read from low to high frequency, ARE the
frequency response of everything between the plugin's output and the recording.
"""
import json, os, sys, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
GRID = 1.5; NOTE = 1.2

def load(path):
    w = wave.open(path); n = w.getnframes(); ch = w.getnchannels(); sr = w.getframerate()
    x = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch).astype(np.float64)
    if ch == 1: x = np.repeat(x, 2, axis=1)
    return x, sr

def envelope(x, sr, win=0.02):
    m = np.sqrt(np.convolve(x.mean(axis=1) ** 2, np.ones(int(win * sr)) / int(win * sr), mode='same'))
    return m[::int(0.005 * sr)], int(0.005 * sr)          # one value per 5 ms

def find_grid(x, sr, nev):
    """start time (s) of the first event: the grid position where tones are loud and gaps are quiet."""
    e, step = envelope(x, sr); dt = step / sr
    best = (-1e18, 0.0)
    for t0 in np.arange(0.0, min(30.0, len(x) / sr - nev * GRID), 0.005):
        s = 0.0
        for k in range(nev):
            a = int((t0 + k * GRID + 0.15) / dt); b = int((t0 + k * GRID + 1.05) / dt)
            g0 = int((t0 + k * GRID + 1.27) / dt); g1 = int((t0 + k * GRID + 1.45) / dt)
            if b + 1 >= len(e): return None
            s += e[a:b].mean() - e[g0:g1].mean()
        if s > best[0]: best = (s, t0)
    # refine to 1 ms using the onset of the loudest events
    return best[1]

def sine_amplitude(seg, sr, f0):
    """amplitude of the sine near f0: the power in the main lobe (peak +- 4 bins) of a Hann-windowed FFT, which does not depend on where
    between two FFT bins the frequency falls"""
    n = len(seg); w = np.hanning(n); X = np.fft.rfft(seg * w); f = np.fft.rfftfreq(n, 1 / sr)
    m = (f > f0 * 0.97) & (f < f0 * 1.03)
    if not m.any(): return 0.0
    k = np.flatnonzero(m)[np.argmax(np.abs(X[m]))]
    lo, hi = max(0, k - 4), min(len(X), k + 5)
    return float(np.sqrt(4.0 * np.sum(np.abs(X[lo:hi]) ** 2) / (n * np.sum(w ** 2))))

def measure(x, sr, t0, ev):
    out = []
    for k, d in enumerate(ev):
        a = int((t0 + k * GRID + 0.3) * sr); b = int((t0 + k * GRID + 0.9) * sr)
        if b > len(x): out.append(None); continue
        row = {}
        for c in (0, 1):
            seg = x[a:b, c]; n = len(seg)
            row[c] = dict(rms=seg.std())
            if d['kind'] in ('fm', 'psg'):
                row[c]['fund'] = sine_amplitude(seg, sr, d['f'])
            else:
                sp = np.abs(np.fft.rfft(seg * np.hanning(n))); f = np.fft.rfftfreq(n, 1 / sr); pw = sp ** 2
                row[c]['bands'] = [np.sqrt(pw[(f >= lo) & (f < hi)].sum()) for lo, hi in ((100, 400), (400, 1600), (1600, 6400), (6400, 20000))]
        out.append(row)
    return out

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2: print(__doc__); return 2
    evp = os.path.join(HERE, 'calib_events.json')
    if '--json' in sys.argv: evp = sys.argv[sys.argv.index('--json') + 1]
    ev = json.load(open(evp)); nev = len(ev)
    rec, sr1 = load(args[0]); ref, sr2 = load(args[1])
    if sr1 != sr2: print("warning: sample rates differ (%d vs %d); the result is only meaningful if both are 44100" % (sr1, sr2))
    t_rec = find_grid(rec, sr1, nev); t_ref = find_grid(ref, sr2, nev)
    if t_rec is None or t_ref is None: print("could not find the test signal (recording too short?)"); return 1
    print("test signal starts at %.3f s in the recording and %.3f s in the reference (offset %.3f s)" % (t_rec, t_ref, t_rec - t_ref))
    mr = measure(rec, sr1, t_rec, ev); mf = measure(ref, sr2, t_ref, ev)
    print("\n kind   frequency      reference level   recording/reference  (left, right)   total RMS ratio")
    sines = []
    for k, d in enumerate(ev):
        if mr[k] is None or mf[k] is None: continue
        rat = lambda key, c: mr[k][c][key] / mf[k][c][key] if mf[k][c][key] > 0 else float('nan')
        rms = (rat('rms', 0) + rat('rms', 1)) / 2
        if d['kind'] in ('fm', 'psg'):
            l, r = rat('fund', 0), rat('fund', 1)
            print(" %-5s %8.0f Hz   %12.0f       %8.3f  %8.3f           %6.3f" % (d['kind'], d['f'], mf[k][0]['fund'], l, r, rms))
            if d['kind'] == 'fm': sines.append((d['f'], (l + r) / 2))
        else:
            b = [(mr[k][0]['bands'][i] / mf[k][0]['bands'][i] + mr[k][1]['bands'][i] / mf[k][1]['bands'][i]) / 2 if mf[k][0]['bands'][i] > 0 else float('nan') for i in range(4)]
            print(" noise %-28s bands 100-400/400-1600/1600-6400/6400-20k Hz: %s     %6.3f" % (d['note'], " ".join("%6.3f" % v for v in b), rms))
    if sines:
        sines.sort()
        print("\nFM sines, sorted by frequency (this is the frequency response of the chain):")
        print("   " + "  ".join("%dHz:%.3f" % (round(f), r) for f, r in sines))

if __name__ == '__main__':
    sys.exit(main())
