#!/usr/bin/env python3
"""
song_fuzz.py - the whole engine against the whole engine, on random songs.

Each iteration generates a random, mostly-valid MML program (every command: bends, portamento, LFOs, release, loops, raw
register writes ...), picks a random device (PC88 / X1 / MZ - including dialect/device combinations the game never uses),
plays it in the ORIGINAL DLL (emulated) and in the port, and compares the 16-bit output sample for sample.

usage: song_fuzz.py [--count N] [--seed S] [--seconds T]
"""
import argparse, array, os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import DllMixer
from emu import EmuError
from compile_fuzz import Gen
from mixer_diff import build_port

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--count', type=int, default=10); ap.add_argument('--seed', type=int, default=1); ap.add_argument('--seconds', type=float, default=0.8)
    ap.add_argument('--keep', default=os.path.join(HERE, 'fuzz_failures'))
    a = ap.parse_args()
    exe = build_port(); rnd = random.Random(a.seed); gen = Gen(rnd, valid=True)
    os.makedirs(a.keep, exist_ok=True)
    frames = int(a.seconds * 44100); block = 1024
    stats = dict(identical=0, mismatch=0, dll_rejected=0, dll_crash=0, silent=0)
    for it in range(a.count):
        dev = rnd.choice([4, 4, 5, 5, 6])
        vol = rnd.choice([32, 32, 0, 20, 60])
        m = DllMixer(volume=vol, device=dev)
        try:
            for attempt in range(60):                        # keep generating until the original compiler accepts a program
                text = gen.program().encode('latin-1')
                m.set_device(dev)
                err = m.compile(3, 0, text)
                if not err: break
                stats['dll_rejected'] += 1
            else:
                continue
            m.play(3, 0)
            out = bytearray(); done = 0
            while done < frames:
                k = min(block, frames - done); out += m.render(k); done += k
        except EmuError as e:
            stats['dll_crash'] += 1
            open(os.path.join(a.keep, 'songcrash_%d_%d.mus' % (a.seed, it)), 'wb').write(text); print('DLL crash in song %d: %s' % (it, str(e)[:80])); continue
        with tempfile.TemporaryDirectory() as td:
            sp, op = os.path.join(td, 's.mus'), os.path.join(td, 'o.bin'); open(sp, 'wb').write(text)
            r = subprocess.run([exe, sp, op, str(dev), str(vol), '0', str(frames), str(block)], capture_output=True)
            port = open(op, 'rb').read() if r.returncode == 0 and os.path.exists(op) else None
        A = array.array('h'); A.frombytes(bytes(out))
        if port is None:
            stats['mismatch'] += 1; open(os.path.join(a.keep, 'songfail_%d_%d.mus' % (a.seed, it)), 'wb').write(text)
            print('MISMATCH song %d: DLL accepted but the port failed (exit %d)' % (it, r.returncode)); continue
        B = array.array('h'); B.frombytes(port)
        if A == B:
            stats['identical'] += 1
            if not any(A): stats['silent'] += 1
        else:
            bad = [i for i in range(len(A)) if A[i] != B[i]]
            stats['mismatch'] += 1
            path = os.path.join(a.keep, 'songfail_%d_%d.mus' % (a.seed, it)); open(path, 'wb').write(text)
            print('MISMATCH song %d (device %d, volume %d): %d of %d samples differ, first at %.3f s; saved %s' % (it, dev, vol, len(bad), len(A), bad[0] / 2 / 44100, path))
    print('songs: %d | identical audio: %d (of which silent: %d) | DLL rejected the program: %d | DLL crashed: %d | MISMATCHES: %d'
          % (a.count, stats['identical'], stats['silent'], stats['dll_rejected'], stats['dll_crash'], stats['mismatch']))
    sys.exit(1 if stats['mismatch'] else 0)


if __name__ == '__main__':
    main()
