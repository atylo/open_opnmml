#!/usr/bin/env python3
"""
chip_diff.py - compare the ported chip (Opx4) with the ORIGINAL DLL's chip code, sample by sample.

The same script of register writes and render calls is run on both sides:
  * port : verify/chipscript (C++, links ../opx4.cpp)
  * DLL  : the real functions of the original DLL executed in a CPU emulator (emu.py), called by their addresses.

usage:  chip_diff.py random [--steps N] [--seed S] [--modes 0,1,4]      random register writes in the given chip modes
        chip_diff.py song FILE.mus [--ms N]                            the register stream of a real song
"""
import argparse, os, random, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import DllEmu

HERE = os.path.dirname(os.path.abspath(__file__))
DLL = os.environ.get('ORIGINAL_DLL', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'original.dll'))

# Addresses in the original DLL (the sub_XXXXXXXX names)
CHIP_CTOR, CHIP_SETMODE, CHIP_WRITE, CHIP_WRITE_OPN, CHIP_GENERATE, CHIP_SET_GAIN = (
    0x1001BC80, 0x1001BB20, 0x1001B7A0, 0x1001B220, 0x1001A6E0, 0x100195B0)
CRT_GETPTD = 0x100093D2           # C runtime: pointer to the per-thread data block (holds the rand() seed at +0x14)
CHIP_BYTES = 0x1398               # size of the C++ object (operator new size in the DLL)


def build_port():
    exe = os.path.join(HERE, 'chipscript')
    srcs = [os.path.join(HERE, 'chipscript.cpp'), os.path.join(HERE, '..', 'opx4.cpp')]
    newest = max(os.path.getmtime(s) for s in srcs + [os.path.join(HERE, '..', 'opx4.hpp'), os.path.join(HERE, '..', 'chiptables.inc')])
    if not os.path.exists(exe) or os.path.getmtime(exe) < newest:
        subprocess.check_call(['g++', '-std=c++11', '-O2', '-I' + os.path.join(HERE, '..'), '-o', exe] + srcs)
    return exe


def run_port(script_lines):
    exe = build_port()
    with tempfile.TemporaryDirectory() as td:
        sp, op = os.path.join(td, 's.txt'), os.path.join(td, 'o.bin')
        open(sp, 'w').write('\n'.join(script_lines) + '\n')
        subprocess.check_call([exe, sp, op])
        return open(op, 'rb').read()


def run_dll(script_lines, heap_fill=0x00, progress=True):
    emu = DllEmu(DLL, heap_fill=heap_fill)
    # The DLL's rand() is its own code; only its thread-data lookup is replaced by a block with the default seed 1.
    ptd = emu.alloc(0x100, 0)
    emu.write_u32(ptd + 0x14, 1)
    emu.add_hook(CRT_GETPTD, lambda e: e.cdecl_return(ptd))
    chip = None
    out = bytearray()
    n_gen = sum(1 for l in script_lines if l.startswith('gen'))
    done = 0
    for line in script_lines:
        p = line.split()
        c = p[0]
        if c == 'new':
            chip = emu.alloc(CHIP_BYTES)
            emu.call(CHIP_CTOR, int(p[1]), int(p[2]), this=chip)
        elif c == 'mode':
            emu.call(CHIP_SETMODE, int(p[1]), this=chip)
        elif c == 'gain':
            emu.call(CHIP_SET_GAIN, int(p[1]), this=chip)
        elif c == 'w':
            emu.call(CHIP_WRITE, 0, int(p[1]), int(p[2]), this=chip)
        elif c == 'wo':
            emu.call(CHIP_WRITE_OPN, 0, int(p[1]), int(p[2]), this=chip)
        elif c == 'dump':
            out += emu.read(chip, CHIP_BYTES) + b'\0' * (1400 * 4 - CHIP_BYTES)
        elif c == 'gen':
            n = int(p[1])
            buf = emu.alloc(8 * n, 0)
            emu.call(CHIP_GENERATE, buf, n, this=chip)
            out += emu.read(buf, 8 * n)
            done += 1
            if progress and done % 25 == 0:
                print('\r  DLL side: %d/%d render calls' % (done, n_gen), end='', file=sys.stderr, flush=True)
        else:
            raise SystemExit('bad script line: ' + line)
    if progress:
        print('', file=sys.stderr)
    return bytes(out)


def random_script(seed, steps, modes):
    rnd = random.Random(seed)
    lines = ['new 44100 4000000']
    for mode in modes:
        lines.append('mode %d' % mode)
        opm = mode in (1, 4)
        for _ in range(steps):
            for _ in range(1 + rnd.randrange(6)):
                kind, val = rnd.randrange(10), rnd.randrange(256)
                if kind < 4:   reg = 0x30 + rnd.randrange(0x70)
                elif kind < 6: reg = 0xA0 + rnd.randrange(0x18)
                elif kind < 7: reg = rnd.randrange(0x10)
                elif kind < 8: reg = rnd.choice([0x22, 0x27, 0x28, 0x18, 0x19, 0x1B, 0x0F, 0x08, 0x01])
                else:          reg = rnd.randrange(256)
                if reg in (0x28, 0x08): val = (val | 0xF0) if rnd.randrange(2) else (val & 0x0F)
                if opm and reg < 0x10 and rnd.randrange(3) == 0: cmd = 'wo'
                elif (not opm) and rnd.randrange(7) == 0:        cmd = 'wo'
                else:                                            cmd = 'w'
                lines.append('%s %d %d' % (cmd, reg, val))
            lines.append('gen %d' % (1 + rnd.randrange(63)))
    return lines


def song_script(path, ms):
    """Register stream of a real song (from the ported sequencer, via mmltrace --all), one ~1 ms render per step."""
    trace = os.path.join(HERE, '..', 'mmltrace')
    if not os.path.exists(trace):
        subprocess.check_call(['g++', '-std=c++11', '-O2', '-o', trace] + [os.path.join(HERE, '..', f) for f in ('mmltrace.cpp', 'seq.cpp', 'mmlcomp.cpp')])
    rows = subprocess.run([trace, path, '-t', str(ms), '--all'], capture_output=True, text=True, check=True).stdout.splitlines()
    first = rows[0].split()[1] if rows else 'OPN'
    x1 = any(r.split()[1] == 'OPM' for r in rows)
    mode = 4 if x1 else 0
    lines = ['new 44100 4000000', 'gain 2048', 'mode %d' % mode]
    t_prev = 0
    by_t = {}
    for r in rows:
        t, kind, reg, _, val = r.split()
        by_t.setdefault(int(t), []).append((kind, int(reg, 16), int(val, 16)))
    for t in range(ms):
        for kind, reg, val in by_t.get(t, []):
            lines.append('%s %d %d' % ('wo' if (x1 and kind == 'OPN') else 'w', reg, val))
        lines.append('gen 44')            # ~1 ms of audio
    return lines, mode


# dwords that only hold code/table addresses in the DLL (vtable, envelope-table and function pointers)
IGNORED = {0, 145, 146}


def field_names():
    """Names of the state-image dwords, parsed from opx4.hpp (G_*, OP_*, CH_*)."""
    import re
    src = open(os.path.join(HERE, '..', 'opx4.hpp')).read()
    g, o, c = {}, {}, {}
    for m in re.finditer(r'\b(G_\w+|OP_\w+|CH_\w+)\s*=\s*(\d+)', src):
        n, v = m.group(1), int(m.group(2))
        if n in ('OP_BASE', 'CH_BASE', 'OP_STRIDE', 'CH_STRIDE'): continue
        (o if n.startswith('OP_') else c if n.startswith('CH_') else g)[v] = n
    return g, o, c


def name_of(i, names):
    g, o, c = names
    if 147 <= i < 147 + 32 * 29:
        op, f = divmod(i - 147, 29); return 'op%d.%s' % (op, o.get(f, 'field%d' % f))
    if 1075 <= i < 1075 + 8 * 18:
        ch, f = divmod(i - 1075, 18)
        if f >= 14: return 'ch%d.OUTPUT_PTR+%d' % (ch, f - 14)
        return 'ch%d.%s' % (ch, c.get(f, 'field%d' % f))
    if 1238 <= i <= 1244 and i in g: return g[i]
    for base in (1225, 1230, 1238, 1242):
        pass
    return g.get(i, 'dword%d (byte %d)' % (i, 4 * i))


def normalise_dll_state(raw, chip_addr, dummy_addr=0x1003DA74):
    """The DLL stores pointers; the port stores dword indices.  Convert the known pointer cells."""
    import array
    w = array.array('I'); w.frombytes(raw)
    cells = []
    for op in range(32): cells += [147 + 29 * op + 21, 147 + 29 * op + 22, 147 + 29 * op + 28]
    for ch in range(8): cells += [1075 + 18 * ch + k for k in range(14, 18)]
    cells += [1242, 1243, 1244]
    for i in cells:
        v = w[i]
        if v == dummy_addr: w[i] = 1300
        elif chip_addr <= v < chip_addr + CHIP_BYTES: w[i] = (v - chip_addr) // 4
    return w


def state_compare(script_lines, label, heap_fill=0x00, stop_at_first=True):
    """Run both sides, dumping the full state after EVERY command; report the first state divergence."""
    import array
    sc = []
    for l in script_lines:
        sc.append(l)
        if not l.startswith(('new', 'mode', 'gain')): sc.append('dump')
        else: sc.append('dump') if l.startswith(('mode', 'gain')) else None
    port = run_port(sc)
    # DLL side needs the chip address for pointer normalisation: replay manually
    emu = DllEmu(DLL, heap_fill=heap_fill)
    ptd = emu.alloc(0x100, 0); emu.write_u32(ptd + 0x14, 1)
    emu.add_hook(CRT_GETPTD, lambda e: e.cdecl_return(ptd))
    chip = None; dumps = []
    for line in sc:
        p = line.split(); c = p[0]
        if c == 'new': chip = emu.alloc(CHIP_BYTES); emu.call(CHIP_CTOR, int(p[1]), int(p[2]), this=chip)
        elif c == 'mode': emu.call(CHIP_SETMODE, int(p[1]), this=chip)
        elif c == 'gain': emu.call(CHIP_SET_GAIN, int(p[1]), this=chip)
        elif c == 'w': emu.call(CHIP_WRITE, 0, int(p[1]), int(p[2]), this=chip)
        elif c == 'wo': emu.call(CHIP_WRITE_OPN, 0, int(p[1]), int(p[2]), this=chip)
        elif c == 'gen':
            n = int(p[1]); buf = emu.alloc(8 * n, 0); emu.call(CHIP_GENERATE, buf, n, this=chip)
        elif c == 'dump':
            dumps.append(normalise_dll_state(emu.read(chip, CHIP_BYTES) + b'\0' * (1400 * 4 - CHIP_BYTES), chip))
    # the port side contains, in order: [dump after each command] and [samples for each gen]; walk in script order
    A = array.array('i'); pos = 0; k = 0; names = field_names()
    # reconstruct: port output = for each script line either samples (gen) or state (dump)
    raw = memoryview(port); cmdline = None
    for line in sc:
        p = line.split(); c = p[0]
        if c == 'gen':
            pos += 8 * int(p[1]); cmdline = line
        elif c == 'dump':
            st = array.array('i'); st.frombytes(bytes(raw[pos:pos + 1400 * 4])); pos += 1400 * 4
            ref = dumps[k]; k += 1
            diffs = [i for i in range(1400) if i not in IGNORED and (st[i] & 0xFFFFFFFF) != ref[i]]
            if diffs:
                print('%s: STATE DIFFERS after "%s" (dump #%d); %d fields differ. First few:' % (label, cmdline, k, len(diffs)))
                for i in diffs[:12]:
                    print('    %-28s port=%-12d dll=%d' % (name_of(i, names), st[i], ref[i] if ref[i] < 2**31 else ref[i] - 2**32))
                return False
        else:
            cmdline = line
    print('%s: full state IDENTICAL after every command (%d checkpoints)' % (label, k))
    return True


def compare(a, b, label):
    import array
    A, B = array.array('i'), array.array('i')
    A.frombytes(a); B.frombytes(b)
    if len(A) != len(B):
        print('%s: LENGTH DIFFERS port=%d dll=%d' % (label, len(A), len(B))); return False
    bad = [i for i in range(len(A)) if A[i] != B[i]]
    peak = max(max(A), -min(A)) if A else 0
    if not bad:
        print('%s: IDENTICAL  (%d samples, peak %d)' % (label, len(A), peak)); return True
    i = bad[0]
    print('%s: DIFFERENT  %d of %d samples differ; first at sample %d (frame %d %s): port=%d dll=%d; max |diff|=%d'
          % (label, len(bad), len(A), i, i // 2, 'L' if i % 2 == 0 else 'R', A[i], B[i], max(abs(A[j] - B[j]) for j in bad)))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('random'); r.add_argument('--steps', type=int, default=60); r.add_argument('--seed', type=int, default=1)
    r.add_argument('--modes', default='0,1,2,3,4,5'); r.add_argument('--heap-fill', type=lambda x: int(x, 0), default=0)
    st = sub.add_parser('state'); st.add_argument('--steps', type=int, default=20); st.add_argument('--seed', type=int, default=1); st.add_argument('--mode', type=int, default=0)
    s = sub.add_parser('song'); s.add_argument('file'); s.add_argument('--ms', type=int, default=1500); s.add_argument('--heap-fill', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()
    if a.cmd == 'state':
        sys.exit(0 if state_compare(random_script(a.seed, a.steps, [a.mode]), 'chip mode %d' % a.mode) else 1)
    if a.cmd == 'random':
        ok = True
        for m in [int(x) for x in a.modes.split(',')]:
            sc = random_script(a.seed, a.steps, [m])
            ok &= compare(run_port(sc), run_dll(sc, a.heap_fill), 'random writes, chip mode %d' % m)
    else:
        sc, mode = song_script(a.file, a.ms)
        ok = compare(run_port(sc), run_dll(sc, a.heap_fill), '%s, first %d ms, chip mode %d' % (os.path.basename(a.file), a.ms, mode))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
