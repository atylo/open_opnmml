#!/usr/bin/env python3
"""
compare_music.py - compares a recording (or any other audio file) of a song with the port's render of the same song.

    python compare_music.py RECORDING OURS [--skip SECONDS_OF_OURS_TO_ALIGN_ON=5] [--len 40]

Both files may be WAV or anything ffmpeg can read (FLAC ...).  It reports
  1. where the song starts in the recording
  2. the SPEED ratio (recording / ours) from the way the offset grows with time (a clock difference, a different tempo or sample rate)
  3. the level ratio per frequency band (the spectral balance), pooled over the whole song
  4. how alike the waveforms are, in short windows after local alignment (1.0 = identical shape)
  5. the stereo picture (correlation between left and right, and the level difference)
"""
import os, subprocess, sys, tempfile, wave
import numpy as np
from scipy import signal

def load(path, sr=44100):
    """16-bit stereo float array at 44.1 kHz; ffmpeg converts anything else"""
    if not path.lower().endswith('.wav'):
        tmp = tempfile.mktemp(suffix='.wav'); subprocess.run(['ffmpeg', '-v', 'quiet', '-y', '-i', path, '-ar', str(sr), '-ac', '2', '-c:a', 'pcm_s16le', tmp], check=True); path = tmp
    w = wave.open(path); n = w.getnframes(); ch = w.getnchannels()
    x = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch).astype(np.float64)
    if w.getframerate() != sr: print("warning: %s has %d Hz" % (path, w.getframerate()))
    return np.repeat(x, 2, axis=1) if ch == 1 else x

def xlag(a, b):
    """lag such that b[t+lag] ~ a[t] and the normalised correlation there (FFT)"""
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    c = np.fft.irfft(np.fft.rfft(b, n) * np.conj(np.fft.rfft(a, n)), n)
    c = np.concatenate([c[-(len(a) - 1):], c[:len(b)]]); lags = np.arange(-(len(a) - 1), len(b))
    k = int(np.argmax(c)); s = max(0, lags[k]); seg = b[s:s + len(a)]
    return int(lags[k]), c[k] / np.sqrt(np.dot(a, a) * np.dot(seg, seg) + 1e-9)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2: print(__doc__); return 2
    opt = lambda name, d: float(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else d
    rec, ours = load(args[0]), load(args[1]); sr = 44100
    m_r, m_o = rec.mean(axis=1), ours.mean(axis=1)
    # 1. coarse alignment on a 40 s excerpt of ours, using a smoothed envelope so that small speed differences do not matter
    start = int(opt('--skip', 5) * sr); L = int(opt('--len', 40) * sr)
    sos = signal.butter(4, 1500 / (sr / 2), output='sos')
    env = lambda x: np.abs(signal.sosfiltfilt(sos, x))
    dec = 44
    e_r = np.convolve(env(m_r), np.ones(dec) / dec, 'same')[::dec]; e_o = np.convolve(env(m_o), np.ones(dec) / dec, 'same')[::dec]
    seg = e_o[start // dec:(start + L) // dec]; seg = seg - seg.mean(); er = e_r - e_r.mean()
    lag, c = xlag(seg, er); lag0 = lag * dec - start
    print("1. the song starts at %.3f s in the recording (envelope correlation %.2f)" % (lag0 / sr, c))
    # 2. refine at several places with the waveform itself, then fit a line: offset(t) = a + b t
    pts = []
    t0 = int(8 * sr); tmax = min(len(m_o) - 3 * sr, len(m_r) - lag0 - 3 * sr)
    for t in np.linspace(t0, tmax, 14).astype(int):
        w = m_o[t:t + int(1.0 * sr)]
        est = lag0 + t; lo = max(0, est - 3000); hi = min(len(m_r), est + len(w) + 3000)
        l, cc = xlag(w, m_r[lo:hi])
        if cc > 0.2: pts.append((t / sr, (lo + l - est) / 1.0, cc))
    ts = np.array([p[0] for p in pts]); off = np.array([p[1] for p in pts]) + lag0
    good = np.ones(len(ts), bool)
    for _ in range(3):                                       # robust line fit: drop outliers
        A = np.polyfit(ts[good], off[good], 1); resid = off - np.polyval(A, ts); good = np.abs(resid) < max(3 * np.std(resid[good]), 30)
    print("2. offset over time (samples): " + "  ".join("%.0fs:%+d" % (t, o - lag0) for t, o in zip(ts, off)))
    drift = A[0] / sr                                          # fraction: samples of offset gained per sample of time
    print("   speed of the recording relative to ours: %+.2f ppm  (%d of %d points on the line; the song starts %.3f s into the recording)" % (drift * 1e6, good.sum(), len(ts), A[1] / sr))
    # 3. spectral balance over the whole song, using the fitted alignment
    n0 = int(10 * sr); n1 = int(min(len(m_o) - sr, (len(m_r) - A[1]) / (1 + drift) - sr))
    seg_o = ours[n0:n1]; r0 = int(round(A[1] + n0 * (1 + drift))); seg_r = rec[r0:r0 + int((n1 - n0) * (1 + drift))]
    nn = min(len(seg_o), len(seg_r)); seg_o, seg_r = seg_o[:nn], seg_r[:nn]
    bands = [(30, 100), (100, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000), (16000, 21000)]
    print("3. level ratio recording / ours per band (left, right) over %.0f s:" % (nn / sr))
    f, po = signal.welch(seg_o, fs=sr, nperseg=16384, axis=0); f, pr = signal.welch(seg_r, fs=sr, nperseg=16384, axis=0)
    tot = np.sqrt(pr.sum(axis=0) / po.sum(axis=0))
    for lo, hi in bands:
        m = (f >= lo) & (f < hi); r = np.sqrt(pr[m].sum(axis=0) / po[m].sum(axis=0)); print("   %5d-%-5d Hz   %.3f  %.3f" % (lo, hi, r[0], r[1]))
    print("   whole spectrum      %.3f  %.3f" % (tot[0], tot[1]))
    # 4. waveform alikeness in 100 ms windows after local alignment (+-40 samples)
    cs = []
    for t in np.linspace(12 * sr, tmax - sr, 60).astype(int):
        w = m_o[t:t + 4410]; est = int(round(A[1] + t * (1 + drift)))
        lo = est - 40; seg = m_r[lo:lo + len(w) + 80]
        if len(seg) < len(w) + 80: continue
        l, cc = xlag(w, seg); cs.append(cc)
    print("4. waveform correlation in 100 ms windows, locally aligned: median %.3f, 10%% worst %.3f  (1.0 = same waveform)" % (np.median(cs), np.percentile(cs, 10)))
    # 5. stereo
    def stereo(x): return np.corrcoef(x[:, 0], x[:, 1])[0, 1], 20 * np.log10((x[:, 0].std() + 1e-9) / (x[:, 1].std() + 1e-9))
    print("5. stereo: left/right correlation recording %.3f ours %.3f ; left minus right level recording %+.2f dB, ours %+.2f dB" % (stereo(seg_r)[0], stereo(seg_o)[0], stereo(seg_r)[1], stereo(seg_o)[1]))

if __name__ == '__main__':
    sys.exit(main())
