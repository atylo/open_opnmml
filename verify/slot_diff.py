#!/usr/bin/env python3
"""
slot_diff.py - the multi-song slot engine against the ORIGINAL DLL (run in the CPU emulator).

Two kinds of test, both drive the DLL's own functions (compile 0x10023060, play 0x10022D30, fade-stop 0x10022CE0,
OPNSetDevice = 0x10022DF0 + 0x10022D80, tempo 0x10022A50/0x10022A90, title 0x10022AF0) and the port's Mixer (verify/slotscript.cpp):

  slot_diff.py --scenarios           hand-written scenarios (tone inheritance, unloaded numbers, clamping, device switch ...)
  slot_diff.py --count N --seed S    random operation sequences

After EVERY operation the whole observable state of the four slots is compared: all channel code vectors, tone table, envelope
table, the 64x11 start offsets, the sequencer's runtime state (tempo, counters, fade, and for each of the 11 channels: bound,
state, length, offset, loop offset, owns, tone, volume) and, after a render, the audio sample for sample.  Titles are compared
against a model of the ORIGINAL's behaviour (see KNOWN DEVIATIONS), and the model itself is checked against the DLL.

KNOWN DEVIATIONS (deliberate, documented in DESIGN.md) - these are checked as deviations, so the test fails if they change:
 * Titles.  sub_1001D7D0 clears the title of song 11*num instead of song num (it overwrote `num` with 11*num before using it as an
   index), and the compiler APPENDS to the title.  So the DLL concatenates titles when a number is recompiled and wipes the title
   of song 11*num.  For num >= 6 the clear lands inside the envelope/tone tables (memory corruption).  The port clears the title
   of the song being compiled.  The test only uses numbers whose DLL clear target is untouched memory (all zero) or a real title.
 * A failed compile.  The DLL keeps half-compiled code and half-defined tones; the port restores the slot.  Only the verdict and the
   error line are compared, then both sides are wiped (OPNSetDevice) so the test can go on.
"""
import argparse, os, random, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import DllMixer
from emu import EmuError
from compile_fuzz import Gen, dll_error_line

HERE = os.path.dirname(os.path.abspath(__file__))


def build_slotscript():
    exe = os.path.join(HERE, 'slotscript')
    srcs = [os.path.join(HERE, 'slotscript.cpp')] + [os.path.join(HERE, '..', f) for f in ('mixer.cpp', 'opx4.cpp', 'seq.cpp', 'mmlcomp.cpp')]
    deps = srcs + [os.path.join(HERE, '..', f) for f in ('mixer.hpp', 'opx4.hpp', 'seq.hpp', 'mmlcomp.hpp', 'chiptables.inc', 'tables.inc')]
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(d) for d in deps):
        subprocess.check_call(['g++', '-std=c++11', '-O2', '-Wno-unused-result', '-I' + os.path.join(HERE, '..'), '-o', exe] + srcs)
    return exe


class Port:
    def __init__(self):
        self.p = subprocess.Popen([build_slotscript()], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    def ask(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()
        return self.p.stdout.readline().rstrip('\n')
    def state(self):
        self.p.stdin.write('state\n'); self.p.stdin.flush()
        out = {}
        while True:
            l = self.p.stdout.readline().rstrip('\n')
            if l == 'end': return out
            f = l.split(' ')
            if f[0] == 'code': out[('code', int(f[1]), int(f[2]))] = bytes.fromhex(f[3] if len(f) > 3 else '')
            elif f[0] in ('tones', 'envs', 'starts'): out[(f[0], int(f[1]))] = bytes.fromhex(f[2])
            elif f[0] == 'rt': out[('rt', int(f[1]))] = tuple(int(x) for x in f[2:])
            elif f[0] == 'rtch': out[('rtch', int(f[1]), int(f[2]))] = tuple(int(x) for x in f[3:])
    def close(self):
        try: self.p.stdin.write('quit\n'); self.p.stdin.flush(); self.p.wait(5)
        except Exception: self.p.kill()


class Mismatch(Exception): pass


class Pair:
    """DLL and port side by side; every method applies one operation to both and compares."""
    def __init__(self, device=4, volume=32):
        self.device, self.cur_volume = device, volume
        self.dll = DllMixer(volume=volume, device=device)
        self.port = Port(); self.port.ask('vol %d' % volume); self.port.ask('dev %d' % device)
        self.log = []; self.titles = {}          # model of the DLL's titles: (slot, idx) -> bytes
        self.stats = dict(ops=0, compiles=0, rejected_both=0, skipped_unsafe=0, renders=0, state_compares=0)
        self.compare('initial')

    def note(self, s): self.log.append(s); self.log = self.log[-12:]

    def fail(self, what):
        raise Mismatch('%s\n  last operations:\n    %s' % (what, '\n    '.join(self.log)))

    @staticmethod
    def _norm(st):
        """An idle channel (state 0) is never read, so what its data pointer points at is not observable.  After OPNSetDevice the DLL
        leaves such a pointer dangling into the vectors it has just emptied; the port rebinds it to null.  Ignore 'bound' there."""
        for k, v in list(st.items()):
            if k[0] == 'rtch' and v[1] == 0: st[k] = (None,) + v[1:]
        return st

    def compare(self, label):
        a, b = self._norm(self.dll.state()), self._norm(self.port.state())
        self.stats['state_compares'] += 1
        for k in sorted(a, key=str):
            if a[k] != b.get(k):
                if isinstance(a[k], bytes) and isinstance(b.get(k), bytes):
                    d = next((i for i in range(min(len(a[k]), len(b[k]))) if a[k][i] != b[k][i]), min(len(a[k]), len(b[k])))
                    self.fail('after %s: %s differs (DLL %d bytes, port %d bytes, first difference at byte %d)' % (label, k, len(a[k]), len(b[k]), d))
                self.fail('after %s: %s differs\n    DLL : %r\n    port: %r' % (label, k, a[k], b.get(k)))
        self.stats['ops'] += 1

    def check_title(self, slot, num):
        """DLL title == model of the original; port title == the title of the last file compiled into that number."""
        d = self.dll.title(slot, num)
        want = self.titles.get((slot, num), b'')
        if d != want: self.fail('DLL title(%d,%d) = %r but the model of the original says %r' % (slot, num, d, want))

    # ------------------------------------------------------------------ operations
    def set_device(self, dev):
        self.note('device %d' % dev); self.device = dev
        self.dll.set_device(dev); self.port.ask('dev %d' % dev); self.titles.clear()
        self.compare('device %d' % dev)

    def unsafe_for_dll(self, slot, num):
        """The DLL clears the title cell at dword 771+77*num.  Outside the 64-cell title array that is other memory: only OK if it is zero."""
        if num < 0 or num > 63: num = 0 if num < 0 else 63
        if 11 * num < 64: return False
        if num > 50: return True
        sa = self.dll.seq_addr(slot)
        return any(self.dll.emu.read(sa + 4 * (771 + 77 * num), 28))

    def load(self, slot, num, text, play, path):
        n = 0 if num < 0 else 63 if num > 63 else num
        if self.unsafe_for_dll(slot, num): self.stats['skipped_unsafe'] += 1; return None
        self.note('load slot %d num %d play %d  (%d bytes of MML)' % (slot, num, play, len(text)))
        open(path, 'wb').write(text)
        derr = self.dll.compile(slot, num, text)
        if not derr and play: self.dll.play(slot, num)
        r = self.port.ask('load %d %d %s %d' % (slot, num, path, 1 if play else 0))
        self.stats['compiles'] += 1
        if derr:
            if r == 'ok': self.fail('DLL rejected (%r) but the port accepted' % derr)
            dl, pl = dll_error_line(derr), int(r.split(' ')[1]) or None
            if dl is not None and pl is not None and dl != pl: self.fail('error line: DLL %s, port %s (%r / %r)' % (dl, pl, derr, r))
            self.stats['rejected_both'] += 1
            self.set_device(self.device)         # the DLL keeps half-compiled data, the port restores: wipe both
            return False
        if r != 'ok': self.fail('DLL accepted but the port rejected: %s' % r)
        # titles: the original appends, and clears cell 11*num (not num).  The port's title is the file's own title.
        ptitle = bytes.fromhex(self.port.ask('title %d %d' % (slot, n)).split(' ', 1)[1] if ' ' in self.port.ask('title %d %d' % (slot, n)) else '')
        if 11 * n < 64: self.titles[(slot, 11 * n)] = b''
        self.titles[(slot, n)] = self.titles.get((slot, n), b'') + ptitle
        self.compare('load slot %d num %d' % (slot, num))
        self.check_title(slot, n)
        return True

    def playn(self, slot, num):
        self.note('play slot %d num %d' % (slot, num))
        self.dll.play(slot, num); self.port.ask('playn %d %d' % (slot, num)); self.compare('play slot %d num %d' % (slot, num))

    def stop(self, slot):
        self.note('stop slot %d' % slot)
        self.dll.stop(slot); self.port.ask('stop %d' % slot); self.compare('stop slot %d' % slot)

    def tempo(self, slot, bpm=None):
        if bpm is not None:
            self.note('tempo slot %d = %d' % (slot, bpm)); self.dll.set_tempo(slot, bpm); self.port.ask('settempo %d %d' % (slot, bpm))
        a, b = self.dll.tempo(slot), int(self.port.ask('tempo %d' % slot).split(' ')[1])
        if a != b: self.fail('tempo(%d): DLL %d, port %d' % (slot, a, b))
        self.compare('tempo slot %d' % slot)

    def mute(self, m):
        self.note('mute 0x%x' % m); self.dll.set_mute(m); self.port.ask('mute %d' % m); self.compare('mute 0x%x' % m)

    def volume(self, v):
        self.note('volume %d' % v); self.cur_volume = v
        self.dll.set_volume(v); self.port.ask('vol %d' % v); self.compare('volume %d' % v)

    def cut(self, slot):
        """The DLL's stop+reset (sub_10022C80): the slot is cut at once, no fade."""
        self.note('cut slot %d' % slot)
        self.dll.stop_reset(slot); self.port.ask('cut %d' % slot); self.compare('cut slot %d' % slot)

    def render(self, frames, block=128):
        self.note('render %d frames (block %d)' % (frames, block))
        a = b''
        done = 0
        while done < frames:
            k = min(block, frames - done); a += self.dll.render(k); done += k
        r = self.port.ask('render %d %d' % (frames, block))
        b = bytes.fromhex(r.split(' ', 1)[1])
        self.stats['renders'] += 1
        if a != b:
            import array
            A, B = array.array('h'), array.array('h'); A.frombytes(a); B.frombytes(b)
            i = next(i for i in range(len(A)) if A[i] != B[i])
            self.fail('audio differs after render: first difference at frame %d (DLL %d, port %d)' % (i // 2, A[i], B[i]))
        self.compare('render')
        self.last_peak = max((abs(x) for x in memoryview(a).cast('h')), default=0)
        return self.last_peak > 0

    def close(self): self.port.close()


# ------------------------------------------------------------------------------------------------ scenarios
TONE = '@%d={25,2,2,0,0,35,2,1,3,0, 25,2,2,0,0,10,2,1,3,0, 25,2,2,0,0,35,2,1,3,0, 31,5,2,7,2,0,1,1,0,0, 5,6}\r\n'
def tone_line(n, tl):    # a PC-88 tone with a chosen total level so that different tones sound different
    return '@%d={31,5,2,7,2,%d,1,1,0,0, 31,5,2,7,2,%d,1,1,0,0, 31,5,2,7,2,%d,1,1,0,0, 31,5,2,7,2,%d,1,1,0,0, 4,5}\r\n' % (n, tl, tl, tl, tl)

SONG_DEF = ('#title "Song with tones"\r\n' + tone_line(1, 20) + tone_line(2, 5) + '@e1={-3,-2,-1,0}\r\n'
            'A @1 o4 l8 EA1,4 c d e f g a b >c\r\nB @2 o3 l4 c e g c\r\nD o4 l16 v12 c d e f\r\n')
SONG_USER = 'A @1 o4 l8 c e g >c <g e c r\r\nB @2 o3 l2 c g\r\nD o4 l8 v10 e g\r\n'            # defines nothing: uses tones/envelope 1 from SONG_DEF
SONG_REDEF = tone_line(1, 60) + 'A @1 o4 l4 c d e f\r\n'


def scenarios(pair):
    tmp = os.path.join(HERE, '_scn.mus')
    fl = lambda slot, num, text, play: pair.load(slot, num, text.encode('latin-1'), play, tmp)
    def run(frames=6000): return pair.render(frames)
    results = []
    # 1. tone and envelope inheritance: song 1 defines nothing and uses what song 0 of the same slot defined
    pair.set_device(4)
    fl(3, 0, SONG_DEF, False); fl(3, 1, SONG_USER, True); results.append(('inherit tones+envelope (slot 3, song 1)', run()))
    pair.playn(3, 0); results.append(('song 0 of the same slot', run()))
    # 2. the same text in a slot that has no tones: tones are all zero
    fl(0, 0, SONG_USER, True); results.append(('same text in a clean slot', run()))
    # 3. another song redefines tone 1: it changes song 0 and song 1 too (shared table)
    fl(3, 2, SONG_REDEF, True); results.append(('redefine tone 1 in song 2', run())); pair.playn(3, 0); results.append(('song 0 after the redefinition', run()))
    # 4. two slots at once, lowest slot wins a channel
    fl(1, 0, SONG_DEF, True); fl(2, 0, SONG_USER, True); pair.playn(3, 1); results.append(('slots 1,2,3 together', run()))
    # 5. a number never loaded plays the first song of the slot; numbers are clamped
    pair.playn(1, 9); results.append(('play unloaded number 9 of slot 1', run()))
    pair.playn(1, 200); results.append(('play number 200 (clamps to 63)', run()))
    pair.playn(1, -5); results.append(('play number -5 (clamps to 0)', run()))
    pair.playn(0, 5); results.append(('play unloaded number 5 of slot 0', run()))
    # 6. fade-out stop, restart, tempo
    pair.stop(3); results.append(('fade-out of slot 3 (immediately after stop)', run(4000))); results.append(('... and after the fade is over', run(20000)))
    pair.playn(3, 1); pair.tempo(3); pair.tempo(3, 200); results.append(('tempo 200 on slot 3', run())); pair.tempo(3, 90); results.append(('tempo 90', run()))
    pair.tempo(2); pair.tempo(7)
    # 7. recompiling the same number: code grows, start offset moves; the old code stays in the vector
    fl(3, 1, SONG_USER, True); results.append(('recompile song 1', run()))
    # 8. titles (and the original's title bug)
    fl(2, 3, '#title "abc"\r\nD c d e\r\n', False); fl(2, 3, '#title "abc"\r\nD c d e\r\n', False)
    d, p = pair.dll.title(2, 3), bytes.fromhex(pair.port.ask('title 2 3').split(' ', 1)[1] if ' ' in pair.port.ask('title 2 3') else '')
    results.append(('title after compiling "abc" twice into one number: DLL %r (appends), port %r (asserted)' % (d, p), d == b'abcabc' and p == b'abc', 'assert'))
    # 9. device switch wipes every slot
    pair.set_device(5); results.append(('after device switch: all four slots wiped, nothing sounds (asserted)', run(3000) is False, 'assert'))
    fl(3, 0, SONG_USER, True); results.append(('after wipe, a song on the X1 chip', run()))
    pair.set_device(6); pair.set_device(4)
    return results


# ------------------------------------------------------------------------------------------------ random sequences
def strip_defs(text, rnd):
    """Drop the tone/envelope definitions so the song has to use what earlier songs of the slot defined."""
    return re.sub(r'(?m)^@[0-9eE][^\r\n]*\r?\n', '', text) if rnd.random() < 0.5 else text


def song_pool():
    import glob
    return [open(f, 'rb').read() for f in sorted(glob.glob(os.path.join(HERE, '..', 'bgm', '*.mus')) + glob.glob(os.path.join(HERE, '..', 'bgm', 'chama', '*.mus')))]


def random_run(seed, steps, device=None):
    rnd = random.Random(seed); gen = Gen(rnd, valid=True); pool = song_pool()
    pair = Pair(device=device or rnd.choice([4, 4, 5, 6]))
    tmp = os.path.join(HERE, '_rnd_%d.mus' % seed)
    try:
        for step in range(steps):
            r = rnd.random()
            slot, num = rnd.randrange(4), rnd.choice([0, 0, 1, 2, 3, 4, 5, 6, 7, 12, 20, 33, 44, 63, 64, -1, 99])
            if r < 0.34:
                if rnd.random() < 0.35:                       # a real song, whose tones other songs of the slot may then inherit
                    text = rnd.choice(pool)
                    if rnd.random() < 0.3: text = SONG_USER.encode('latin-1')       # inherits tones 1,2 if an earlier song defined them
                else:
                    text = strip_defs(gen.program(), rnd).encode('latin-1')
                pair.load(slot, num, text, rnd.random() < 0.5, tmp)
            elif r < 0.50: pair.playn(slot, num)
            elif r < 0.56: pair.stop(slot)
            elif r < 0.62: pair.tempo(slot, rnd.choice([1, 30, 60, 120, 180, 255, 400]))
            elif r < 0.64: pair.set_device(rnd.choice([4, 5, 6]))
            elif r < 0.67: pair.mute(rnd.choice([0, 0, 1, 7, 0x38, 0x7FF, rnd.randrange(0x800), rnd.randrange(0x800)]))
            elif r < 0.71: pair.volume(rnd.choice([0, 32, 27, 54, 100, 256, 512, rnd.randrange(513), rnd.randrange(513)]))
            elif r < 0.75: pair.cut(slot)
            else: pair.render(rnd.choice([300, 1000, 2500, 5000]), rnd.choice([128, 128, 64, 1024]))
        pair.render(3000)
    finally:
        pair.close()
        if os.path.exists(tmp): os.unlink(tmp)
    return pair.stats


def fade_tempo(pair):
    """Focused check of OPNStopSE (quick fade-out) and tempo get/set against the DLL.  Returns (label, ok) pairs."""
    tmp = os.path.join(HERE, '_ft.mus'); res = []
    fl = lambda slot, num, text, play: pair.load(slot, num, text.encode('latin-1'), play, tmp)
    # ---- fade-out, slot 3 alone, audio and sequencer state compared in 500-frame steps (about 11 ms)
    pair.set_device(4); fl(3, 0, SONG_DEF, True)
    res.append(('song audible before the stop', pair.render(3000)))
    pair.stop(3)
    chunks, peaks = [], []
    for _ in range(40):                                                  # 20000 frames = 0.45 s
        chunks.append(pair.render(500)); peaks.append(pair.last_peak)
    res.append(('audible right after stop (fade has begun)', chunks[0]))
    res.append(('fade-out peak: %d right after stop -> %d at the end (the DLL leaves a constant +-%d residue too)' % (peaks[0], peaks[-1], peaks[-1]), peaks[0] > 300 and max(peaks[-12:]) <= 8, 'assert'))
    res.append(('all channels idle afterwards', all(v[1] == 0 for k, v in pair.port.state().items() if k[0] == 'rtch' and k[1] == 3), 'assert'))
    # ---- odd uses of stop
    pair.stop(0); pair.stop(0); pair.render(500)                         # idle slot, twice
    fl(3, 0, SONG_DEF, True); pair.render(2000); pair.stop(3); pair.render(1000); pair.playn(3, 0); res.append(('restart during the fade cancels it', pair.render(8000)))
    pair.stop(3); pair.render(300); fl(3, 1, SONG_USER, True); res.append(('load+play during the fade', pair.render(4000)))
    pair.stop(3); pair.stop(3); pair.render(20000)                       # stop twice in a row
    pair.set_device(5); fl(3, 0, SONG_USER, True); pair.render(2000); pair.stop(3); pair.render(20000)   # on the X1 chip too
    # ---- tempo: the DLL's own getter/setter, then the audio produced
    pair.set_device(4); fl(3, 0, SONG_DEF, True)
    res.append(('tempo is 120 when a song starts', pair.dll.tempo(3) == 120 and pair.port.ask('tempo 3') == 'tempo 120', 'assert'))
    for bpm in [1, 2, 7, 60, 119, 121, 255, 256, 1000, 43690, 43691, 50000, 100000, 1 << 20]:
        try:
            pair.tempo(3, bpm); pair.render(1500)
            res.append(('tempo %d: getter, state and audio identical' % bpm, True, 'assert'))
        except EmuError as e:
            res.append(('tempo %d: the DLL itself crashed (%s) - not compared' % (bpm, str(e)[:40]), True, 'assert')); pair.set_device(4); fl(3, 0, SONG_DEF, True)
    pair.tempo(3, 120); pair.tempo(9)                                    # bad slot: getter returns -1 on both
    # the port ignores 0 and negative values (the DLL divides by zero / runs backwards); the tempo must stay as it was
    pair.port.ask('settempo 3 0'); pair.port.ask('settempo 3 -5')
    res.append(('port ignores tempo 0 and -5 (DLL would crash): tempo unchanged', pair.port.ask('tempo 3') == 'tempo 120', 'assert'))
    if os.path.exists(tmp): os.unlink(tmp)
    return res


def small_song(rnd, loop_ok=True):
    """A short song on a random subset of the PC-88 channels, so that songs in different slots fight for the same chip voices."""
    out = [tone_line(1, rnd.choice([5, 20, 35])).replace('\r\n', '\n')]
    for c in rnd.sample('ABCDEF', rnd.randint(2, 6)):
        tone = 1 if c in 'ABC' else rnd.choice([64, 96, 32])
        notes = ' '.join(rnd.choice('cdefgab') + rnd.choice(['2', '4', '8', '16']) for _ in range(rnd.randint(2, 10)))
        loop = ' L' if loop_ok and rnd.random() < 0.3 else ''
        out.append('%s\tt%d @%d v%d o%d q%d%s %s\n' % (c, rnd.choice([90, 120, 160]), tone, rnd.randint(6, 15), rnd.randint(3, 5), rnd.randint(4, 8), loop, notes))
    return ''.join(out).encode('latin-1')


def priority_run(seed, steps=45):
    """Many slots playing at once on the same channels: the lowest slot that is still playing a channel keeps its chip voice."""
    rnd = random.Random(seed)
    pair = Pair(device=4); tmp = os.path.join(HERE, '_prio_%d.mus' % seed)
    try:
        for slot in range(4):
            for num in (0, 1):
                pair.load(slot, num, small_song(rnd), False, tmp)
        for _ in range(steps):
            r, slot, num = rnd.random(), rnd.randrange(4), rnd.randrange(2)
            if r < 0.35:   pair.playn(slot, num)
            elif r < 0.43: pair.stop(slot)
            elif r < 0.49: pair.cut(slot)
            elif r < 0.55: pair.mute(rnd.randrange(0x40))
            elif r < 0.60: pair.volume(rnd.choice([0, 20, 32, 60, 200]))
            elif r < 0.64: pair.load(slot, num, small_song(rnd), rnd.random() < 0.6, tmp)
            else:          pair.render(rnd.choice([100, 400, 1500, 3000]), rnd.choice([128, 64, 1024]))
        pair.render(4000)
    finally:
        pair.close()
        if os.path.exists(tmp): os.unlink(tmp)
    return pair.stats


def cut_test(pair):
    """Focused check of stop+reset (the DLL's sub_10022C80) on its own."""
    tmp = os.path.join(HERE, '_cut.mus'); res = []
    fl = lambda slot, num, text, play: pair.load(slot, num, text.encode('latin-1'), play, tmp)
    pair.set_device(4); fl(3, 0, SONG_DEF, True)
    res.append(('song audible before the cut', pair.render(3000)))
    pair.cut(3)
    res.append(('after the cut (notes already keyed on are not released by it)', pair.render(3000)))
    pair.cut(3); pair.cut(0); pair.render(500)                               # twice in a row; an idle slot
    fl(3, 0, SONG_DEF, True); pair.render(1500); pair.cut(3); pair.playn(3, 0); res.append(('cut, then play again at once', pair.render(4000)))
    fl(3, 0, SONG_DEF, True); pair.render(1500); pair.stop(3); pair.render(800); pair.cut(3); res.append(('cut in the middle of a fade-out', pair.render(5000)))
    for s in range(4): fl(s, 0, SONG_USER if s else SONG_DEF, True)
    pair.render(2000); pair.cut(1); res.append(('cut slot 1 while slots 0, 2, 3 keep playing', pair.render(3000)))
    pair.cut(0); pair.cut(2); pair.cut(3); res.append(('cut every slot', pair.render(3000)))
    pair.tempo(3, 200); fl(3, 1, SONG_USER, True); pair.cut(3); pair.tempo(3); res.append(('tempo is back to 120 after a cut and a restart', pair.dll.tempo(3) == 120, 'assert'))
    pair.set_device(5); fl(3, 0, SONG_USER, True); pair.render(1500); pair.cut(3); pair.render(2000)   # X1 chip
    if os.path.exists(tmp): os.unlink(tmp)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--scenarios', action='store_true'); ap.add_argument('--fade-tempo', action='store_true'); ap.add_argument('--cut', action='store_true'); ap.add_argument('--priority', type=int, default=0); ap.add_argument('--count', type=int, default=0)
    ap.add_argument('--seed', type=int, default=1); ap.add_argument('--steps', type=int, default=25)
    a = ap.parse_args(); bad = 0
    if a.scenarios:
        pair = Pair(device=4)
        try:
            for item in scenarios(pair):
                label, res = item[0], item[1]
                if len(item) == 3:
                    print('  %-95s %s' % (label[:95], 'ok' if res else 'FAIL'))
                    if not res: bad += 1
                else:
                    print('  %-95s %s' % (label[:95], 'audible' if res else 'silent'))
            print('scenarios: all state and audio identical to the DLL after every operation (%d state comparisons, %d renders)' % (pair.stats['state_compares'], pair.stats['renders']))
        except Mismatch as m:
            print('MISMATCH: %s' % m); bad += 1
        finally: pair.close()
    if a.cut:
        pair = Pair(device=4)
        try:
            for item in cut_test(pair):
                ok = item[1] if len(item) == 3 else True
                print('  %-80s %s' % (item[0][:80], ('ok' if ok else 'FAIL') if len(item) == 3 else ('audible' if item[1] else 'silent')))
                if not ok: bad += 1
            print('stop+reset: identical to the DLL after every operation (%d state comparisons, %d renders)' % (pair.stats['state_compares'], pair.stats['renders']))
        except Mismatch as m:
            print('MISMATCH: %s' % m); bad += 1
        finally: pair.close()
    for i in range(a.priority):
        try:
            st = priority_run(a.seed + i)
            print('priority seed %d: identical  (%d compares, %d renders)' % (a.seed + i, st['state_compares'], st['renders']), flush=True)
        except Mismatch as m:
            print('priority seed %d MISMATCH: %s' % (a.seed + i, m)); bad += 1
        except EmuError as e:
            print('priority seed %d: the DLL itself crashed (%s) - skipped' % (a.seed + i, str(e)[:60]))
    if a.fade_tempo:
        pair = Pair(device=4)
        try:
            for item in fade_tempo(pair):
                ok = item[1] if len(item) == 3 else True
                print('  %-80s %s' % (item[0][:80], ('ok' if ok else 'FAIL') if len(item) == 3 else ('yes' if item[1] else 'no')))
                if not ok: bad += 1
            print('fade/tempo: identical to the DLL after every operation (%d state comparisons, %d renders)' % (pair.stats['state_compares'], pair.stats['renders']))
        except Mismatch as m:
            print('MISMATCH: %s' % m); bad += 1
        finally: pair.close()
    for i in range(a.count):
        try:
            st = random_run(a.seed + i, a.steps)
            print('seed %d: identical  (%d compares, %d compiles, %d rejected by both, %d skipped as unsafe for the DLL, %d renders)' % (a.seed + i, st['state_compares'], st['compiles'], st['rejected_both'], st['skipped_unsafe'], st['renders']), flush=True)
        except Mismatch as m:
            print('seed %d MISMATCH: %s' % (a.seed + i, m)); bad += 1
        except EmuError as e:
            print('seed %d: the DLL itself crashed (%s) - run skipped' % (a.seed + i, str(e)[:80]))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
