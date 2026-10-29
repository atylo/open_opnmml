#!/usr/bin/env python3
"""
compile_boundary.py - boundary-value comparison of the DLL's compiler and the port.

For every MML command and each of its numeric arguments, the argument is replaced one at a time by edge values (negative
numbers, 0, 1, limits -1/0/+1, numbers that overflow 32 bits, a missing value ...) while the others stay valid.  Both compilers must
agree on accepted/rejected, on the error line, and on the complete compiled output.
"""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from engine import DllMixer
from compile_fuzz import build_compdump, port_result, dll_error_line, port_error_line
import re

EDGE = ['-99999999999', '-4294967297', '-2147483649', '-8193', '-8192', '-129', '-128', '-65', '-64', '-9', '-8', '-3', '-2', '-1', '', '0', '1', '2', '3', '4', '5',
        '7', '8', '9', '10', '11', '15', '16', '17', '31', '32', '33', '63', '64', '65', '100', '101', '126', '127', '128', '129', '191', '192', '193',
        '255', '256', '257', '8191', '8192', '8193', '16000', '16001', '65535', '65536', '987654', '2147483647', '2147483648', '4294967295', '4294967296', '99999999999']
TONE_PC88 = [25, 2, 2, 0, 0, 35, 2, 1, 3, 0] * 4 + [2, 7]
TONE_X1 = [25, 2, 2, 0, 0, 35, 2, 1, 3, 0, 0] * 4 + [2, 7]

# (template, list of placeholder positions is implicit: every {} is varied separately)
TEMPLATES = ['o{}c', 'l{}c', 't{}c', 'v{}c', '({}c', '){}c', '@{}c', 'q{}c', '@q{}c', 'p{}c', 'D{}c', 'T{}c', 'P{}c', 'F{}c',
             'y{},1', 'y1,{}', 'MP{},2,3,4', 'MP1,{},3,4', 'MP1,2,{},4', 'MP1,2,3,{}', 'MA{},2,3,4', 'MA1,{},3,4', 'MA1,2,{},4', 'MA1,2,3,{}',
             'EA{},5', 'EA5,{}', 'R{},5', 'R5,{}', 'c{}', 'c%{}', 'c4.{}', 'c4^{}', 'c4~{}', 'r{}', 'c{}.', '[c]{}', '[c / d]{}', 'c_{}c',
             'o{}c<<c>>c', 'c+{}', 'c4&c{}', 'L{}c']


def variants():
    for tpl in TEMPLATES:
        for e in EDGE:
            yield 'A ' + tpl.replace('{}', e) + '\r\n', 'pc88'
    for tpl in ('MP1,2,3,4 MAOF', 'MPOF', 'ROF', 'EAOF', 'MAOF c4', 'MP1,2,3', 'MP,2,3,4', 'MP1,,3,4', 'MP1 2 3 4', 'MA2,50,-200', 'MA2,50,200', 'MP0,1,127', 'MP0,1,128',
                'MP0,1,-129', 'R 5 , 5', 'R5 5', 'y 5 , 5', 'y5', 'EA 5 , 5', 'EA5', '$SHARP{}', '$SHARP{cde}', '$SHARP{x}', '$FLAT{', '$SHARP', '$FLAT{ba }',
                'c_c', 'c_<<g', 'c4_g', 'r_c', 'c1.._>c', 'c%1_>>>>>>>>c', '[[[[[c]]]]]', '[[[[[[c]]]]]]', '[]', '[c', 'c]', '/', '[c/]', '[/c]'):
        yield 'A ' + tpl + '\r\n', 'pc88'
    # tone definitions: vary each position, tone number, and the length of the list
    for dialect, base, per_op in (('pc88', TONE_PC88, 10), ('x1', TONE_X1, 11)):
        pre = '' if dialect == 'pc88' else '#machine x1\r\n'
        for n in ('-1', '0', '1', '127', '128', '987654', ''):
            yield pre + '@%s={%s}\r\n' % (n, ','.join(map(str, base))), dialect
        for pos in range(len(base)):
            for e in ('-129', '-4', '-3', '-1', '0', '1', '2', '3', '4', '7', '8', '15', '16', '31', '32', '63', '64', '127', '128', '255', '99999999999'):
                v = [str(x) for x in base]; v[pos] = e
                yield pre + '@1={%s}\r\n' % ','.join(v), dialect
        for k in range(len(base) - 3, len(base) + 4):
            yield pre + '@1={%s}\r\n' % ','.join(map(str, (base * 2)[:k])), dialect
        yield pre + '@1 = { ' + ' , '.join(map(str, base)) + ' }\r\n', dialect
        yield pre + '@1={' + ','.join(map(str, base)) + '\r\n', dialect
    for n in ('-1', '0', '1', '63', '64', '987654'):
        for body in ('1,2,3', '', '1', ','.join(['1'] * 63), ','.join(['1'] * 64), '1,,2', '1,2,'):
            yield '@e%s={%s}\r\n' % (n, body), 'pc88'
    for loop in ('', ',0', ',2', ',-1', ',99', ',987654'):
        yield '@e5%s={1,2,3,4}\r\n' % loop, 'pc88'
    for d in ('#total_offset -9', '#total_offset -8', '#total_offset 0', '#total_offset 100', '#total_offset 101', '#total_offset', '#total_offset x', '#total_offset 5 junk',
              '#octave n', '#octave x', '#octave', '#octave z', '#OCTAVE N', '#machine x1', '#machine X1', '#machine pc88', '#machine mz', '#machine', '#machine q',
              '#title "abc"', '#title abc', '#title', '#title "abc', '#title ""', '#foo', '#', '# title "x"'):
        yield d + '\r\nA c4\r\n', 'pc88'


def main():
    import argparse
    ap = argparse.ArgumentParser(); ap.add_argument('--part', default='0/1', help='N/M: run only every M-th variant starting at N (to split the work)'); ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args(); part, parts = map(int, a.part.split('/'))
    exe = build_compdump()
    dll = DllMixer()
    total = both_ok = both_rej = bad = crash = compared = 0
    all_v = list(variants())
    if a.limit: all_v = all_v[:a.limit]
    print('variants in this part: %d of %d' % (len(all_v[part::parts]), len(all_v)), flush=True)
    for text, _ in all_v[part::parts]:
        total += 1
        b = text.encode('latin-1')
        try:
            dll.set_device(4); err = dll.compile(3, 0, b); d = dll.dump_song(3) if not err else None
        except Exception as e:
            crash += 1
            p = port_result(exe, b)
            print('DLL CRASH on %r (port: %s)' % (text, 'crash' if 'crash' in p else 'rejects gracefully' if 'error' in p else 'ACCEPTS'))
            dll = DllMixer(); continue
        p = port_result(exe, b)
        if 'crash' in p:
            bad += 1; print('PORT CRASH on %r' % text); continue
        p_err = p if 'error' in p else None
        verdict = None
        if (d is None) != (p_err is None) is False: pass
        if (d is None) != (p_err is not None): verdict = 'DLL %s / port %s' % ('rejects (%s)' % err.decode('cp932', 'replace') if err else 'accepts', 'rejects' if p_err else 'accepts')
        elif d is None:
            dl, pl = dll_error_line(err), port_error_line(p_err)
            if dl is not None and pl is not None:
                compared += 1
                if dl != pl: verdict = 'error line differs: DLL %s port %s (DLL: %r)' % (dl, pl, err.decode('cp932', 'replace')[:50])
            both_rej += 1
        else:
            for k in ('x1_mode', 'total_offset', 'tones', 'envs'):
                if d[k] != p[k]: verdict = 'compiled %s differ' % k; break
            for c in range(11):
                if verdict is None and d['code'][c] != p['code'][c]: verdict = 'channel %d bytecode differs: dll %s port %s' % (c, d['code'][c].hex(), p['code'][c].hex())
            both_ok += 1
        if verdict:
            bad += 1; print('MISMATCH %-40r %s' % (text, verdict))
    print('boundary programs: %d | accepted by both and identical: %d | rejected by both: %d | DLL crashes: %d | error lines compared: %d | MISMATCHES: %d' % (total, both_ok, both_rej, crash, compared, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
