#!/usr/bin/env python3
"""
match_release.py - how much of the difference between an official recording/release of a song and the port's render is just level and tone?

    python match_release.py RELEASE.flac OURS.wav MATCHED.wav [--offset SECONDS] [--train A:B] [--test A:B]

Both files can be anything ffmpeg reads.  OURS.wav should be a render of the same song (a plain one: mmlplay SONG.mus OURS.wav -t 80 on the right device).
  1. finds where the song starts in the release (or use --offset)
  2. measures, on the training part of the song (default 8..40 s), the level ratio release / ours in 1/3-octave bands
  3. builds a gain + equaliser from it and removes the DC offset (a 15 Hz high-pass), applies them to ours and writes MATCHED.wav
  4. shows on a DIFFERENT part of the song (default 40..70 s) how well the two now agree in level and tone
What remains after this (phase, tiny ripples, a few % of peak height) is not a difference in the music.
The player can do the same without this script: mmlplay SONG.mus OUT.wav -d -c -e release -G 11.7  (the equaliser there is a three-section fit of
the curve this script measures).
"""
import sys, wave
import numpy as np
from scipy import signal
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from compare_music import load, xlag

sr = 44100
def psd(x): return signal.welch(x, fs=sr, nperseg=16384)
def span(name, d):
    if name in sys.argv:
        a, b = sys.argv[sys.argv.index(name) + 1].split(':'); return int(float(a) * sr), int(float(b) * sr)
    return int(d[0] * sr), int(d[1] * sr)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--') and not a[:1].isdigit() and ':' not in a]
    if len(args) < 3: print(__doc__); return 2
    rel, ours = load(args[0]), load(args[1]); mr, mo = rel.mean(axis=1), ours.mean(axis=1)
    if '--offset' in sys.argv: off = int(float(sys.argv[sys.argv.index('--offset') + 1]) * sr)
    else:
        sos = signal.butter(4, 1500 / (sr / 2), output='sos'); dec = 44
        env = lambda x: np.convolve(np.abs(signal.sosfiltfilt(sos, x)), np.ones(dec) / dec, 'same')[::dec]
        seg = env(mo)[5 * sr // dec:35 * sr // dec]; lag, c = xlag(seg - seg.mean(), env(mr) - env(mr).mean()); off = (lag - 5 * sr // dec) * dec
        l, c2 = xlag(mo[10 * sr:11 * sr], mr[10 * sr + off - 3000:11 * sr + off + 3000]); off += l - 3000
    print("the song starts %.3f s into the release" % (off / sr))
    t0, t1 = span('--train', (8, 40)); v0, v1 = span('--test', (40, 70))
    print("DC offset: release %.1f, ours %.1f" % (mr[off + t0:off + t1].mean(), mo[t0:t1].mean()))
    f, po = psd(mo[t0:t1]); pb = psd(mr[off + t0:off + t1])[1]
    cent = [60 * 2 ** (i / 3.0) for i in range(25)]; vals = []
    for c in cent:
        m = (f >= c / 2 ** (1 / 6)) & (f < c * 2 ** (1 / 6)); vals.append(np.sqrt(pb[m].sum() / po[m].sum()))
    vals = np.array(vals); cs = np.array(cent)
    gain = np.exp(np.mean(np.log(vals[(cs > 200) & (cs < 8000)])))
    print("gain %.2f = %+.1f dB;  equaliser relative to that (dB): " % (gain, 20 * np.log10(gain)) + " ".join("%d:%+.1f" % (c, 20 * np.log10(v / gain)) for c, v in zip(cs[::2], vals[::2])))
    nfft = 8192; fr = np.fft.rfftfreq(nfft, 1 / sr)
    H = np.interp(np.log(np.maximum(fr, 1)), np.log(cs), vals); H[fr < 40] = H[np.argmin(abs(fr - 60))]
    h = np.roll(np.fft.irfft(H), nfft // 2) * np.hanning(nfft); hp = signal.butter(1, 15 / (sr / 2), 'high', output='sos')
    mat = np.stack([signal.sosfilt(hp, signal.fftconvolve(ours[:, c], h, mode='same')) for c in (0, 1)], axis=1)
    a = mat.mean(axis=1)[v0:v1]; b = mr[off + v0:off + v1]; d = mo[v0:v1]
    bands = [(50, 300), (300, 1000), (1000, 3000), (3000, 8000), (8000, 12000), (12000, 16000), (16000, 19000), (19000, 21500)]
    def rat(x, y):
        fa, px = psd(x); fb, py = psd(y); return np.array([np.sqrt(py[(fa >= lo) & (fa < hi)].sum() / px[(fa >= lo) & (fa < hi)].sum()) for lo, hi in bands])
    r0, r1 = rat(d, b), rat(a, b)
    print("\nlevel ratio release / ours on the test part, bands " + " ".join("%d-%d" % x for x in bands))
    print("   plain render       " + " ".join("%5.2f" % v for v in r0) + "   spread %.1f dB" % (20 * np.log10(r0.max() / r0.min())))
    print("   gain+EQ+DC-block   " + " ".join("%5.2f" % v for v in r1) + "   spread %.1f dB" % (20 * np.log10(r1.max() / r1.min())))
    print("   rms release %.0f  matched %.0f | peak release %.0f  matched %.0f" % (b.std(), a.std(), np.abs(b).max(), np.abs(a).max()))
    out = np.clip(np.round(mat), -32768, 32767).astype('<i2')
    w = wave.open(args[2], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes(out.tobytes()); w.close()
    print("\nwritten %s" % args[2])

if __name__ == '__main__':
    sys.exit(main())
