#!/usr/bin/env python3
"""
mixer_diff.py - the whole engine against the whole engine.

Runs the ORIGINAL DLL's mixer (compiler + sequencer + chip + mixer, in the CPU emulator) and the port on the same
.mus file, with the same call sequence the plugin itself uses, and compares the 16-bit samples:

    sub_10023150(rate)     create the mixer                      sub_10022B70(32)   master volume
    sub_10022D80(device)   select the chip (4 PC88 / 5 X1 / 6 MZ) sub_10023060(slot, text, 0, num)   compile
    sub_10022D30(slot, num) play                                  sub_100233F0(out, frames)          render

usage: mixer_diff.py FILE.mus [--device 4|5|6] [--seconds S] [--volume V] [--mute MASK] [--block N]
"""
import argparse, array, os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import DllEmu

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.environ.get('ORIGINAL_DLL', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'original.dll'))
OP_NEW, OP_DELETE, OP_DELETE_ARR, CRT_GETPTD = 0x10006AB5, 0x100066CA, 0x10006BF4, 0x100093D2
MIXER_CTOR, MIXER_VOLUME, MIXER_DEVICE, MIXER_COMPILE, MIXER_PLAY, MIXER_RENDER, MIXER_MUTE = (
    0x10023150, 0x10022B70, 0x10022D80, 0x10023060, 0x10022D30, 0x100233F0, 0x10022BC0)


def build_port():
    exe = os.path.join(HERE, 'mixerscript')
    srcs = [os.path.join(HERE, 'mixerscript.cpp')] + [os.path.join(HERE, '..', f) for f in ('mixer.cpp', 'opx4.cpp', 'seq.cpp', 'mmlcomp.cpp')]
    deps = srcs + [os.path.join(HERE, '..', f) for f in ('mixer.hpp', 'opx4.hpp', 'seq.hpp', 'mmlcomp.hpp', 'chiptables.inc', 'tables.inc')]
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(d) for d in deps):
        subprocess.check_call(['g++', '-std=c++11', '-O2', '-Wno-unused-result', '-I' + os.path.join(HERE, '..'), '-o', exe] + srcs)
    return exe


def run_port(path, device, seconds, volume, mute, block):
    exe = build_port()
    with tempfile.TemporaryDirectory() as td:
        o = os.path.join(td, 'o.bin')
        subprocess.check_call([exe, path, o, str(device), str(volume), str(mute), str(int(seconds * 44100)), str(block)])
        return open(o, 'rb').read()


def run_dll(path, device, seconds, volume, mute, block, heap_fill=0x00):
    emu = DllEmu(DLL, heap_fill=heap_fill)
    ptd = emu.alloc(0x100, 0); emu.write_u32(ptd + 0x14, 1)           # per-thread data: rand() seed = 1
    emu.add_hook(CRT_GETPTD, lambda e: e.cdecl_return(ptd))
    emu.add_hook(OP_NEW, lambda e: e.cdecl_return(e.alloc(e.arg(0))))  # operator new(size)
    emu.add_hook(OP_DELETE, lambda e: e.cdecl_return(0))               # operator delete / delete[]: never reuse memory
    emu.add_hook(OP_DELETE_ARR, lambda e: e.cdecl_return(0))
    mixer = emu.alloc(0x5C)
    emu.call(MIXER_CTOR, 44100, this=mixer)
    emu.call(MIXER_VOLUME, volume, this=mixer)
    emu.call(MIXER_DEVICE, device, this=mixer)
    emu.call(MIXER_MUTE, mute, this=mixer)
    text = emu.cstr(open(path, 'rb').read())
    err = emu.call(MIXER_COMPILE, 3, text, 0, 0, this=mixer)
    if err and emu.read(err, 1) != b'\0':
        raise SystemExit('the DLL reports a compile error: %r' % emu.read(err, 80).split(b'\0')[0])
    emu.call(MIXER_PLAY, 3, 0, this=mixer)
    frames = int(seconds * 44100)
    out = bytearray(); done = 0; calls = 0
    buf = emu.alloc(4 * block, 0)
    total_calls = (frames + block - 1) // block
    while done < frames:
        k = min(block, frames - done)
        emu.write(buf, b'\0' * (4 * k))
        emu.call(MIXER_RENDER, buf, k, this=mixer)
        out += emu.read(buf, 4 * k)
        done += k; calls += 1
        if calls % 20 == 0: print('\r  DLL side: %d/%d render calls' % (calls, total_calls), end='', file=sys.stderr, flush=True)
    print('', file=sys.stderr)
    return bytes(out)


def compare(port, dll, label):
    A, B = array.array('h'), array.array('h'); A.frombytes(port); B.frombytes(dll)
    if len(A) != len(B):
        print('%s: LENGTH DIFFERS port=%d dll=%d' % (label, len(A), len(B))); return False
    bad = [i for i in range(len(A)) if A[i] != B[i]]
    peak = max(max(A), -min(A)) if A else 0
    if not bad:
        print('%s: IDENTICAL  (%d samples = %.1f s, peak %d)' % (label, len(A), len(A) / 2 / 44100, peak)); return True
    i = bad[0]
    print('%s: DIFFERENT  %d of %d samples differ; first at frame %d (%.3f s): port=%d dll=%d' % (label, len(bad), len(A), i // 2, i / 2 / 44100, A[i], B[i]))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file'); ap.add_argument('--device', type=int, default=4, choices=(4, 5, 6))
    ap.add_argument('--seconds', type=float, default=5.0); ap.add_argument('--volume', type=int, default=32)
    ap.add_argument('--mute', type=lambda x: int(x, 0), default=0); ap.add_argument('--block', type=int, default=1024)
    ap.add_argument('--heap-fill', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()
    label = '%s device %d vol %d mute 0x%x block %d' % (os.path.basename(a.file), a.device, a.volume, a.mute, a.block)
    ok = compare(run_port(a.file, a.device, a.seconds, a.volume, a.mute, a.block), run_dll(a.file, a.device, a.seconds, a.volume, a.mute, a.block, a.heap_fill), label)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
