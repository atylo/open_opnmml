#!/usr/bin/env python3
"""
compile_fuzz.py - differential fuzzing of the MML compiler: the DLL's own compiler (in the emulator) against the port.

Random MML programs (every command, header directives, tone/envelope definitions, comments, plus random corruption) are
compiled by both; the verdict (accepted / rejected) and, when accepted, the complete compiled result must be identical.

usage: compile_fuzz.py [--count N] [--seed S] [--corrupt P] [--keep DIR]
"""
import argparse, os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import DllMixer
from emu import EmuError

HERE = os.path.dirname(os.path.abspath(__file__))


def build_compdump():
    exe = os.path.join(HERE, 'compdump')
    deps = [os.path.join(HERE, 'compdump.cpp')] + [os.path.join(HERE, '..', f) for f in ('mmlcomp.cpp', 'mmlcomp.hpp')]
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(d) for d in deps):
        subprocess.check_call(['g++', '-std=c++11', '-O2', '-Wno-unused-result', '-I' + os.path.join(HERE, '..'), '-o', exe, deps[0], deps[1]])
    return exe


# ------------------------------------------------------------------------------------------------ MML generator
class Gen:
    def __init__(self, rnd, valid=False):
        self.r = rnd
        self.valid = valid          # valid=True: structurally legal programs, so most of them are accepted

    def num(self, lo, hi, p_bad=0.10):
        if self.valid: return self.r.randint(lo, hi)
        if self.r.random() < p_bad: return self.r.choice([lo - 1, hi + 1, -1, -1, -2, -9, 0, 999999, hi * 3, -999999])
        return self.r.randint(lo, hi)

    def length(self):
        r = self.r.random()
        if r < 0.35: return ''
        if r < 0.80: s = str(self.r.choice([1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 192, self.r.randint(1, 250)]))
        elif r < 0.90: s = '%' + str(self.r.randint(1, 400))
        else: s = str(self.r.choice([4, 8, 16]))
        s += '.' * self.r.choice([0, 0, 0, 1, 1, 2])
        if self.r.random() < 0.15: s += self.r.choice(['^', '~']) + str(self.r.choice([8, 16, 32, 64, 5]))
        return s

    def note(self):
        n = self.r.choice('abcdefg')
        s = n + self.r.choice(['', '', '', '+', '-', '++', '+-']) + self.length()
        if self.r.random() < 0.08: s += '_' + self.r.choice(['', '<', '>', '<<']) + self.r.choice('abcdefg') + self.r.choice(['', '+', '-'])
        return s

    def token(self, depth=0):
        r, R = self.r.random(), self.r
        if r < 0.42: return self.note()
        if r < 0.50: return 'r' + self.length()
        if r < 0.54: return 'o' + str(self.num(0, 7, 0.06))
        if r < 0.58: return R.choice(['<', '>', '<', '>'])
        if r < 0.61: return 'l' + str(R.choice([1, 2, 4, 8, 16, 32, 48, 64, 192, self.num(1, 200)]))
        if r < 0.63: return 't' + str(self.num(1, 255))
        if r < 0.67: return 'v' + str(self.num(0, 15))
        if r < 0.70: return R.choice(['(', ')']) + R.choice(['', str(self.num(1, 10))])
        if r < 0.73: return '@' + str(self.num(1, 127))
        if r < 0.75: return R.choice(['q' + str(self.num(1, 8)), '@q' + str(self.num(1, 127))])
        if r < 0.77: return 'p' + str(self.num(1, 3))
        if r < 0.79: return 'D' + str(self.num(-8192, 8192))
        if r < 0.80: return 'T' + str(self.num(-64, 64))
        if r < 0.81: return 'P' + str(self.num(0, 7))
        if r < 0.82: return 'F' + str(self.num(1, 255))
        if r < 0.83: return 'y%d,%d' % (self.num(0, 255), self.num(0, 255))
        if r < 0.86:
            k = R.choice('PA')
            return 'M%s%d,%d,%d%s' % (k, self.num(0, 4), self.num(1, 100), self.num(-200, 200, 0.1), R.choice(['', ',' + str(self.num(0, 100))]))
        if r < 0.87: return R.choice(['MPOF', 'MAOF', 'EAOF', 'ROF'])
        if r < 0.89: return 'EA%d,%d' % (self.num(0, 64), self.num(1, 100))
        if r < 0.90: return 'R%d,%d' % (self.num(0, 127), self.num(1, 15))
        if r < 0.91: return '&'
        if r < 0.92: return 'L'
        if r < 0.93: return R.choice(['$SHARP{', '$FLAT{']) + ''.join(R.sample('cdefgab', R.randint(0, 4))) + '}'
        if r < 0.99 and depth < 3:
            inner = ' '.join(self.token(depth + 1) for _ in range(R.randint(1, 5)))
            if R.random() < 0.3: inner += ' / ' + ' '.join(self.token(depth + 1) for _ in range(R.randint(1, 2)))
            return '[' + inner + ']' + R.choice(['', str(self.num(1, 127))])
        return R.choice(['x', 'Z', '!', '['])           # junk

    def tone(self, x1):
        per_op = 11 if x1 else 10
        ranges = [(0, 31), (0, 31), (0, 31), (0, 15), (0, 15), (0, 127), (0, 3), (0, 15), (-3, 7)] + ([(0, 3), (0, 1)] if x1 else [(0, 1)])
        vals = []
        for _ in range(4): vals += [self.num(lo, hi, 0.01) for lo, hi in ranges]
        vals += [self.num(0, 7, 0.01), self.num(0, 7, 0.01)] + [self.num(0, 7, 0.01), self.num(0, 3, 0.01)][:self.r.choice([0, 0, 1, 2])]
        if self.r.random() < 0.05: vals = vals[:-self.r.randint(1, 5)]
        return '@%d={%s}' % (self.num(0, 127, 0.02), ','.join(map(str, vals)))

    def env(self):
        n = self.r.randint(1, 20)
        head = '@e%d' % self.num(1, 63, 0.03)
        if self.r.random() < 0.3: head += ',%d' % self.r.randint(0, n)
        return head + '={%s}' % ','.join(str(self.r.randint(-15, 15)) for _ in range(n))

    # ---- valid mode: legal structure, many commands ----
    def vtoken(self, depth, st):
        R = self.r
        r = R.random()
        if r < 0.38: return self.note()
        if r < 0.44: return 'r' + self.length()
        if r < 0.50 and depth == 0:
            if R.random() < 0.5 and st['oct'] > 2: st['oct'] -= 1; return '<'
            if st['oct'] < 5: st['oct'] += 1; return '>'
            return 'o4'
        if r < 0.53: st['oct'] = R.randint(2, 5) if depth == 0 else st['oct']; return 'o%d' % (st['oct'] if depth == 0 else 4) if depth == 0 else 'o4'
        if r < 0.56 and depth == 0 and not st['L']: st['L'] = True; return 'L ' + self.note()
        if r < 0.62 and depth < 3:
            body = [self.note()] + [self.vtoken(depth + 1, st) for _ in range(R.randint(0, 4))]
            if R.random() < 0.3: body += ['/', self.note()]
            return '[' + ' '.join(body) + ']' + R.choice(['', str(R.randint(1, 9))])
        if r < 0.64: return 'T' + str(R.randint(-12, 12))
        tok = self.token(depth)
        if tok in ('<', '>', 'L', 'x', 'Z', '!', '[') or tok.startswith(('[', 'o', 'T', 'l')) and depth > 0: return self.note()
        return tok

    def vprogram(self):
        R, out = self.r, []
        x1 = R.random() < 0.5
        out.append('#machine ' + ('x1' if x1 else R.choice(['pc88', 'pc88', 'mz'])) if (x1 or R.random() < 0.7) else '#title "t"')
        if R.random() < 0.3: out.append('#octave ' + R.choice(['n', 'x']))
        if R.random() < 0.4: out.append('#total_offset %d' % R.randint(-8, 100))
        if R.random() < 0.3: out.append('#title "' + ''.join(R.choice('abc XYZ') for _ in range(R.randint(0, 12))) + '"')
        for _ in range(R.randint(0, 5)): out.append(self.tone(x1))
        for _ in range(R.randint(0, 3)): out.append(self.env())
        letters = 'ABCDEFGHIJK' if x1 else 'ABCDEF'
        for _ in range(R.randint(1, 6)):
            chans = ''.join(sorted(set(R.choice(letters) for _ in range(R.randint(1, 4)))))
            st = {'oct': 4, 'L': False}
            body = ' '.join(self.vtoken(0, st) for _ in range(R.randint(2, 16)))
            line = chans + R.choice([' ', '\t']) + 'o4 ' + body
            if R.random() < 0.05: line = '// comment\n' + line
            out.append(line)
        return '\r\n'.join(out) + '\r\n'

    def program(self):
        if self.valid: return self.vprogram()
        R, out = self.r, []
        x1 = R.random() < 0.4
        if R.random() < 0.8: out.append('#machine ' + ('x1' if x1 else R.choice(['pc88', 'pc88', 'mz'])) if R.random() < 0.97 else '#machine foo')
        if R.random() < 0.3: out.append('#octave ' + R.choice(['n', 'x', 'x', 'q']))
        if R.random() < 0.4: out.append('#total_offset %d' % self.num(-8, 100, 0.08))
        if R.random() < 0.3: out.append('#title "' + ''.join(R.choice('abc XYZ') for _ in range(R.randint(0, 12))) + '"')
        for _ in range(R.randint(0, 4)): out.append(self.tone(x1))
        for _ in range(R.randint(0, 2)): out.append(self.env())
        letters = 'ABCDEFGHIJK'
        for _ in range(R.randint(1, 6)):
            chans = ''.join(sorted(set(R.choice(letters) for _ in range(R.randint(1, 4)))))
            body = ' '.join(self.token() for _ in range(R.randint(1, 14)))
            line = chans + R.choice([' ', ' ', '\t']) + body
            if R.random() < 0.06: line += ' // trailing comment'
            if R.random() < 0.04: line = '// full line comment\n' + line
            if R.random() < 0.03: line = '/* block\ncomment */' + line
            out.append(line)
        return '\r\n'.join(out) + R.choice(['\r\n', '\n', ''])


def mutate(text, rnd):
    b = bytearray(text.encode('latin-1'))
    for _ in range(rnd.randint(1, 3)):
        if not b: break
        i = rnd.randrange(len(b)); op = rnd.randrange(3)
        if op == 0: b[i] = rnd.randrange(256)
        elif op == 1: b.insert(i, rnd.choice(b'[]{}()/,=#@ -+._^~&$' + bytes(range(48, 58))))
        else: del b[i]
    return bytes(b)


def dll_error_line(err):
    """Line number in the DLL's (Shift-JIS) error message: '... <n>' (format "%s %d") or '... <n>行目' (format "...%d行目")."""
    import re
    m = re.search(rb'(\d+)\x8ds\x96\xda', err) or re.search(rb' (\d+)$', err)
    return int(m.group(1)) if m else None


def port_error_line(p_err):
    """The line the port reports (from the structured field; 0 = none)."""
    return p_err['line'] or None


# ------------------------------------------------------------------------------------------------ comparison
def port_result(exe, mml):
    with tempfile.NamedTemporaryFile(suffix='.mus', delete=False) as f: f.write(mml); p = f.name
    try:
        r = subprocess.run([exe, p], capture_output=True, text=True)
    finally:
        os.unlink(p)
    if r.returncode != 0: return {'crash': r.returncode}
    out = r.stdout.split('\n')
    if out[0] != 'OK': return {'error': out[1].strip() if len(out) > 1 else '', 'line': int(out[2]) if len(out) > 2 and out[2].strip().isdigit() else 0}
    d = {l.split(' ', 1)[0] + (' ' + l.split(' ')[1] if l.startswith('code') else ''): l for l in out if l}
    code = []
    for c in range(11):
        parts = d['code %d' % c].split(' ', 2)
        code.append(bytes.fromhex(parts[2]) if len(parts) > 2 else b'')
    return {'code': code, 'tones': bytes.fromhex(d['tones'].split(' ', 1)[1]), 'envs': bytes.fromhex(d['envs'].split(' ', 1)[1]),
            'x1_mode': int(d['x1'].split()[1]), 'total_offset': int(d['total'].split()[1])}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--count', type=int, default=300); ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--corrupt', type=float, default=0.25); ap.add_argument('--valid', action='store_true', help='generate mostly-valid programs (no corruption)')
    ap.add_argument('--keep', default=os.path.join(HERE, 'fuzz_failures'))
    a = ap.parse_args()
    exe = build_compdump(); rnd = random.Random(a.seed); gen = Gen(rnd, valid=a.valid)
    if a.valid: a.corrupt = 0.0
    dll = DllMixer()
    stats = {'both ok': 0, 'both rejected': 0, 'MISMATCH': 0, 'DLL crashed': 0}
    os.makedirs(a.keep, exist_ok=True)
    for i in range(a.count):
        text = gen.program().encode('latin-1')
        if rnd.random() < a.corrupt: text = mutate(text.decode('latin-1'), rnd)
        try:
            dll.set_device(4)                                # wipes every stored song, like OPNSetDevice does
            err = dll.compile(3, 0, text)
            d = dll.dump_song(3) if not err else None
        except EmuError as e:                                # the ORIGINAL plugin itself crashed on this input
            stats['DLL crashed'] += 1
            pr = port_result(exe, text)
            stats['  of which the port also crashed' if 'crash' in pr else '  of which the port rejected gracefully' if 'error' in pr else '  of which the port ACCEPTED'] = stats.get('  of which the port also crashed' if 'crash' in pr else '  of which the port rejected gracefully' if 'error' in pr else '  of which the port ACCEPTED', 0) + 1
            path = os.path.join(a.keep, 'dllcrash_%d_%d.mus' % (a.seed, i)); open(path, 'wb').write(text)
            print('DLL CRASH on program %d (%s) -> saved %s' % (i, str(e)[:90], path))
            dll = DllMixer()
            continue
        p = port_result(exe, text)
        verdict = None
        p_err = p if (p is not None and 'error' in p) else None
        p = None if p_err else p
        if (d is None) != (p is None): verdict = 'verdict differs: DLL %s, port %s' % ('rejected (%r)' % err if err else 'accepted', 'accepted' if p else 'rejected')
        elif d is None:
            # both rejected: the DLL reports "<message> <line>"; the port reports "line N: <message>" - compare the line number
            dl, pl = dll_error_line(err), port_error_line(p_err)
            if dl is not None and pl is not None:
                stats['error lines compared'] = stats.get('error lines compared', 0) + 1
                if dl != pl: verdict = 'error line differs: DLL %s, port %s  (DLL: %r  port: %r)' % (dl, pl, err.decode('cp932', 'replace')[:50], p_err['error'][:50])
            else:
                stats['rejected, no line to compare'] = stats.get('rejected, no line to compare', 0) + 1
        elif d is not None:
            for k in ('x1_mode', 'total_offset', 'tones', 'envs'):
                if d[k] != p[k]: verdict = 'compiled %s differ' % k; break
            for c in range(11):
                if verdict is None and d['code'][c] != p['code'][c]: verdict = 'channel %d bytecode differs (dll %d bytes, port %d bytes)' % (c, len(d['code'][c]), len(p['code'][c]))
        if verdict:
            stats['MISMATCH'] += 1
            path = os.path.join(a.keep, 'case_%d_%d.mus' % (a.seed, i)); open(path, 'wb').write(text)
            print('MISMATCH in program %d: %s  (saved %s)' % (i, verdict, path))
        else:
            stats['both ok' if d is not None else 'both rejected'] += 1
    print('programs: %d | accepted by both and identical: %d | rejected by both: %d | MISMATCHES: %d | original DLL crashed: %d'
          % (a.count, stats['both ok'], stats['both rejected'], stats['MISMATCH'], stats['DLL crashed']))
    for k in sorted(stats):
        if k.startswith('  of which') or k.startswith('error lines') or k.startswith('rejected, no line'): print('%s: %d' % (k, stats[k]))
    if stats.get('  of which the port ACCEPTED') or stats.get('  of which the port also crashed'): stats['MISMATCH'] += 1
    sys.exit(1 if stats['MISMATCH'] else 0)


if __name__ == '__main__':
    main()
