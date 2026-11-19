#!/usr/bin/env python3
"""
make_calib.py - writes the calibration files used to find out where the tilt between the game's recordings and the port comes from.

  calib_fm3.mus     a test song for the game (PC-88 dialect, plays on the PC-88 chip, no loop, about 70 s):
                      1. FM sine tones, 13 notes from about 65 Hz to 3.9 kHz, then the same notes doubled and quadrupled in frequency (up to 16 kHz)
                      2. PSG square waves on the same 13 notes
                      3. PSG noise bursts (4 different noise periods)
                    every tone lasts about 1.2 s and is followed by 0.3 s of silence
  control_tones.wav the same kind of signal as a plain WAV file (exact sines at the same frequencies), to be played in an ordinary player
                    and recorded with the same method: it shows what the recording chain does on its own
"""
import numpy as np, wave, struct, json, os
HERE = os.path.dirname(os.path.abspath(__file__))
NOTE_MS = 1200; GAP_MS = 300
TICK_MS = 1024 / 98.0                                  # the engine's real tick length at the default tempo 120 (see MML_REFERENCE.md)
NOTE_TICKS = round(NOTE_MS / TICK_MS); GAP_TICKS = round(GAP_MS / TICK_MS)
NOTES = [('c', 2), ('g', 2), ('c', 3), ('g', 3), ('c', 4), ('g', 4), ('c', 5), ('g', 5), ('c', 6), ('g', 6), ('c', 7), ('g', 7), ('b', 7)]
SEMI = {'c': 0, 'd': 2, 'e': 4, 'f': 5, 'g': 7, 'a': 9, 'b': 11}
def freq(n, o): return 440.0 * 2 ** ((12 * o + SEMI[n] - 57) / 12.0)

def tone(n_ml):   # one audible operator (a sine), the other three silent, algorithm 7 (all carriers), no feedback
    op = lambda tl, ml: "31,0,0,15,0,%d,0,%d,0,0" % (tl, ml)
    return "{" + ", ".join([op(0, n_ml), op(127, 1), op(127, 1), op(127, 1)]) + ", 7,0}"

events = []          # (channel, kind, note, octave, multiple, start_tick)
a, d = [], []        # music text of channel A (FM) and D (PSG)
pos = 0
def play(ch_text, other_text, n, o, tonenum, kind, mul):
    global pos
    ch_text.append("@%d %s%d%s r%%%d" % (tonenum, n, 0, "", GAP_TICKS))   # placeholder replaced below
for kind, plan in (("fm", [(1, 1), (2, 2), (3, 4)]), ("psg", [(64, 1)])):
    pass

lines_a, lines_d, order = [], [], []
def add(channel, tonenum, n, o, kind, mul):
    global pos
    note = "o%d %s%%%d r%%%d" % (o, n, NOTE_TICKS, GAP_TICKS)
    if channel == 'A':
        lines_a.append("@%d %s" % (tonenum, note)); lines_d.append("r%%%d" % (NOTE_TICKS + GAP_TICKS))
    else:
        lines_d.append("@%d %s" % (tonenum, note)); lines_a.append("r%%%d" % (NOTE_TICKS + GAP_TICKS))
    order.append(dict(kind=kind, f=freq(n, o) * mul, note="%s%d" % (n, o), mul=mul))
def rest(channel_only=None):
    lines_a.append("r%%%d" % (NOTE_TICKS + GAP_TICKS)); lines_d.append("r%%%d" % (NOTE_TICKS + GAP_TICKS))

rest()                                             # 1.5 s of silence first: easy to find the start
for mul, tn in ((1, 1), (2, 2), (4, 3)):           # FM sines: tone 1 = x1, tone 2 = x2, tone 3 = x4 frequency multiple
    for n, o in (NOTES if mul == 1 else NOTES[-3:]):
        add('A', tn, n, o, 'fm', mul)
for n, o in NOTES:                                 # PSG squares
    add('D', 64, n, o, 'psg', 1)
for period in (2, 8, 16, 31):                      # PSG noise: @32 = noise only, low 5 bits = noise period
    add('D', 32 + period, 'c', 4, 'noise', 0); order[-1]['note'] = 'noise period %d' % period
rest()

mus = ["#title \"Calibration signal for the port\"", "",
       "@1=" + tone(1), "@2=" + tone(2), "@3=" + tone(4), "",
       "AD\tt120 q8 v15", "A\t" + " ".join(lines_a), "D\t" + " ".join(lines_d), ""]
open(os.path.join(HERE, 'calib_fm3.mus'), 'w', newline='\r\n').write("\n".join(mus))
json.dump(order, open(os.path.join(HERE, 'calib_events.json'), 'w'), indent=1)

# the plain-WAV control: exact sines (and noise) with the same timing, level 0.3 of full scale, 44.1 kHz 16-bit stereo
rate = 44100; sig = [np.zeros(int(1.5 * rate))]
rng = np.random.RandomState(1)
for ev in order:
    n = int(NOTE_MS / 1000.0 * rate); t = np.arange(n) / rate
    if ev['kind'] == 'noise': x = rng.uniform(-1, 1, n)
    elif ev['kind'] == 'psg': x = np.sign(np.sin(2 * np.pi * ev['f'] * t))
    else: x = np.sin(2 * np.pi * ev['f'] * t)
    x = x * 0.3 * np.minimum(1, np.minimum(t, t[::-1]) / 0.005)           # 5 ms edges, no clicks
    sig += [x, np.zeros(int(GAP_MS / 1000.0 * rate))]
sig.append(np.zeros(int(1.5 * rate)))
y = np.concatenate(sig); pcm = (np.clip(y, -1, 1) * 32767).astype('<i2')
w = wave.open(os.path.join(HERE, 'control_tones.wav'), 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate)
w.writeframes(np.repeat(pcm[:, None], 2, axis=1).tobytes()); w.close()
print("events:", len(order), "| note %d ticks, gap %d ticks | files: calib_fm3.mus, control_tones.wav, calib_events.json" % (NOTE_TICKS, GAP_TICKS))
