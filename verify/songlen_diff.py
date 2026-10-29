#!/usr/bin/env python3
"""
songlen_diff.py - checks the song-length measurement (songlen.cpp, shown by mmldump and mmlplay) against the ORIGINAL DLL.

The DLL's own mixer step function (sub_10022E80, no audio is produced) is run tick by tick while the channel objects are watched:
  * the tick at which a channel first has a timed note             -> "used"
  * the tick at which its loop offset first becomes non-zero       -> loopStart   (the L command was passed)
  * the tick at which its terminator count (passes) first is >= 1  -> end of the first pass, or the first wrap of a loop
  * the tick at which it first is >= 2                             -> second wrap = length of one trip round the loop
  * a channel stopped from outside (fade-out)                      -> fadeEnded
The same events, in milliseconds, are what lenscript (the port's measurement) prints; they must agree EXACTLY, tick for tick.

  songlen_diff.py                 all example songs and the built-in special cases
  songlen_diff.py --cases         only the built-in special cases
  songlen_diff.py FILE.mus ...    only these files (device chosen from the file name like the game does)
"""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import DllMixer

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, '..')
STEP = 0x10022E80                       # sub_10022E80(this, frames): advance the sequencers by frames/44.1 ms steps


def build():
    exe = os.path.join(HERE, 'lenscript')
    srcs = [os.path.join(HERE, 'lenscript.cpp')] + [os.path.join(ROOT, f) for f in ('songlen.cpp', 'seq.cpp', 'mmlcomp.cpp')]
    deps = srcs + [os.path.join(ROOT, f) for f in ('songlen.hpp', 'seq.hpp', 'mmlcomp.hpp', 'tables.inc')]
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(d) for d in deps):
        subprocess.check_call(['g++', '-std=c++11', '-O2', '-I' + ROOT, '-o', exe] + srcs)
    return exe


def port_timing(path):
    out = subprocess.run([build(), path], capture_output=True, text=True).stdout.split('\n')
    if out[0] in ('ERR', 'EMPTY'): return out[0], None
    ch, song = {}, None
    for l in out:
        f = l.split()
        if not f: continue
        if f[0] == 'ch':
            c = int(f[1]); used, loops, end, ls, lm, fade = (int(x) for x in f[2:8])
            if used: ch[c] = dict(loops=bool(loops), end=end, loopStart=ls if loops else None, loopMs=lm if loops else None, fade=bool(fade))
        elif f[0] == 'song': song = [int(x) for x in f[1:]]
    return 'OK', (ch, song)


def dll_timing(text, device, predicted):
    """Step the DLL and record the same events.  `predicted` (from the port) only decides where to step finely."""
    m = DllMixer(); m.set_device(device)
    err = m.compile(3, 0, text)
    if err: return None
    m.play(3, 0)
    e = m.emu; sa = m.seq_addr(3)
    chp = [e.read_u32(sa + 4 * (4163 + c)) for c in range(11)]
    active = [e.read_u32(chp[c] + 4) != 0 and (e.read(chp[c] + 10, 2) != b'\0\0') for c in range(11)]
    ev = {c: dict(saw=False, loopStart=None, end=None, loops=False, wrap2=None, fade=False) for c in range(11)}
    todo = {c for c in range(11) if active[c]}
    want = sorted(t for c, d in predicted.items() for t in (d['end'], d['loopStart'], (d['end'] + d['loopMs']) if d['loops'] else None) if t)
    limit = (want[-1] if want else 1000) + 400
    cnt = 0
    while todo and cnt < limit:
        nxt = next((t for t in want if t > cnt), cnt + 200)
        gap = nxt - cnt
        e.call(STEP, int((gap - 60) * 44.1) if gap > 90 else 44, this=m.mixer)
        now = e.read_u32(sa + 4 * 4176)
        if now == cnt: continue
        cnt = now
        for c in list(todo):
            raw = e.read(chp[c], 0x68)
            state = int.from_bytes(raw[10:12], 'little', signed=True); ln = int.from_bytes(raw[12:14], 'little')
            loopOff = int.from_bytes(raw[20:24], 'little', signed=True); passes = raw[91]; d = ev[c]
            if ln: d['saw'] = True
            if d['loopStart'] is None and loopOff != 0: d['loopStart'] = cnt
            if d['end'] is None:
                if passes >= 1 and state != 0: d['end'] = cnt; d['loops'] = True
                elif passes >= 1:              d['end'] = cnt; todo.discard(c)
                elif state == 0:               d['end'] = cnt; d['fade'] = True; todo.discard(c)
            elif d['loops'] and passes >= 2:   d['wrap2'] = cnt; todo.discard(c)
    res = {}
    for c, d in ev.items():
        if active[c] and d['saw']:
            res[c] = dict(loops=d['loops'], end=d['end'], loopStart=d['loopStart'] if d['loops'] else None,
                          loopMs=(d['wrap2'] - d['end']) if d['loops'] and d['wrap2'] else None, fade=d['fade'])
    return res


def check(name, path, device):
    text = open(path, 'rb').read()
    status, port = port_timing(path)
    if status != 'OK': return '%-34s port: %s' % (name, status), False
    pch, psong = port
    dch = dll_timing(text, device, pch)
    if dch is None: return '%-34s the original DLL rejected the file' % name, False
    problems = []
    for c in sorted(set(pch) | set(dch)):
        if pch.get(c) != dch.get(c): problems.append('channel %s: port %s | DLL %s' % ('ABCDEFGHIJK'[c], pch.get(c), dch.get(c)))
    if problems: return '%-34s MISMATCH\n    ' % name + '\n    '.join(problems), False
    return '%-34s identical: %d channels, once %.3f s, intro %.3f s, loop %.3f s%s%s' % (
        name, len(pch), psong[0] / 1000, psong[1] / 1000, psong[2] / 1000, ', loops' if psong[3] else ', no loop', ', ends by fade' if psong[5] else ''), True


T88 = "@1={31,5,2,7,2,20,1,1,0,0, 31,5,2,7,2,10,1,1,0,0, 31,5,2,7,2,10,1,1,0,0, 31,5,2,7,2,10,1,1,0,0, 4,5}\n"
CASES = {
    'no loop, ends by itself':        "A\tt150 @1 o4 l4 c d e f\nD\t@64 t150 o4 l8 c d e f g a b >c\n",
    'tempo changes, loops':           T88 + "A\t@1 t60 o4 l8 L c d e f t200 g a b >c t90 c1\nD\t@64 o4 l8 L c d e f t200 g a b >c t90 c1\n",
    'fade-out (F) ends the song':     "D\t@64 t120 o4 l4 c d e f F100 g1 g1 g1 g1 g1 g1\n",
    'L is the very first byte (still loops)': "D\tL @64 t120 o4 l8 c d e f\n",
    'L after settings (loops)':       "D\t@64 t120 L o4 l8 c d e f\n",
    'two loop lengths (drift)':       "D\t@64 t120 o4 l4 L c d e f\nE\t@64 t120 o4 l4 L c d e f g\n",
    'only one channel loops':         "D\t@64 t120 o4 l4 L c d e f\nE\t@64 t120 o4 l4 c d e f g a\n",
    'lead-in before the loop point':  "D\t@64 t120 o4 l4 c d L e f g a\nE\t@64 t120 o4 l4 L e f g a\n",
    'very slow tempo':                "D\t@64 t8 o4 l4 L c d\n",
    'very fast tempo':                "D\t@64 t255 o4 l16 L c d e f g a b >c\n",
    'rests only (nothing to hear)':   "D\tt120 r1 r1\n",
    'long note (2 s) at the end':     "D\t@64 t120 o4 l4 L c d e f1^1\n",
}


def main():
    files = [a for a in sys.argv[1:] if not a.startswith('--')]
    only_cases = '--cases' in sys.argv
    bad = 0
    if not files:
        import glob
        files = [] if only_cases else sorted(glob.glob(os.path.join(ROOT, 'bgm', '*.mus')) + glob.glob(os.path.join(ROOT, 'bgm', 'chama', '*.mus')))
        tmp = os.path.join(HERE, '_len_case.mus')
        for name, text in CASES.items():
            open(tmp, 'wb').write(text.encode('latin-1'))
            msg, ok = check(name, tmp, 4); print(('  ' if ok else '! ') + msg, flush=True); bad += not ok
        os.unlink(tmp)
    for f in files:
        dev = 5 if f.endswith('_opm.mus') else 6 if f.endswith('_pwm.mus') else 4
        msg, ok = check(os.path.basename(f), f, dev); print(('  ' if ok else '! ') + msg, flush=True); bad += not ok
    print('\nsong length: %s' % ('every measurement identical to the original DLL, tick for tick' if not bad else '%d MISMATCH(ES)' % bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
