#!/usr/bin/env python3
"""End-to-end tests of Quidra's extended-real / NaN-rejecting contract.

Runs the actual compiler and generated programs; compiler failures cannot
satisfy tests expecting a runtime error. No language behavior is mocked.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

@dataclass(frozen=True)
class Case:
    name: str
    source: str
    expected: str
    failure: bool = False


def cases() -> list[Case]:
    result: list[Case] = []
    for ty in ('real16', 'real16b', 'real32', 'real64', 'real'):
        pos = '∞' if ty == 'real' else 'inf'
        neg = '-∞' if ty == 'real' else '-inf'
        result.extend([
            Case(f'{ty}-literal', f'{ty} a = inf\n{ty} b = -inf\nprint("{{a}} {{b}}")\n', f'{pos} {neg}'),
            Case(f'{ty}-parse', f'{ty} a = {ty}.parse("inf")\n{ty} b = {ty}.parse("-inf")\nprint("{{a}} {{b}}")\n', f'{pos} {neg}'),
            Case(f'{ty}-zero-divide', f'{ty} one = 1.0\n{ty} zero = 0.0\nprint("{{one / zero}} {{-one / zero}}")\n', f'{pos} {neg}'),
            Case(f'{ty}-extended-arithmetic', f'{ty} a = inf\n{ty} b = -inf\n{ty} one = 1.0\nprint("{{a + one}} {{b - one}} {{one / a}} {{a * -one}}")\n', f'{pos} {neg} 0.0 {neg}'),
            Case(f'{ty}-comparison', f'{ty} a = inf\n{ty} b = -inf\n{ty} one = 1.0\nprint("{{a == a}} {{a != b}} {{b < one}} {{one < a}} {{a > b}}")\n', 'true true true true true'),
            Case(f'{ty}-array', f'{ty}[] a = [inf, 2.0, -inf]\nprint("{{a[0]}} {{a[1]}} {{a[2]}}")\n', f'{pos} 2.0 {neg}'),
            Case(f'{ty}-constant-zero-divide', f'{ty} a = 1.0 / 0.0\nprint(a)\n', pos),
            Case(f'{ty}-zero-negative-power', f'{ty} zero = 0.0\n{ty} exponent = -2.0\nprint(zero ^ exponent)\n', pos),
        ])
        for name, expression in (
            ('zero-zero', 'zero / zero'),
            ('inf-minus-inf', 'a - a'),
            ('opposite-infinities', 'a + b'),
            ('inf-div-inf', 'a / b'),
            ('zero-times-inf', 'zero * a'),
            ('invalid-hidden-by-power', '(a - a) ^ 0.0'),
        ):
            result.append(Case(f'{ty}-{name}',
                f'{ty} a = inf\n{ty} b = -inf\n{ty} zero = 0.0\nprint("before")\n'
                f'{ty} result = {expression}\nprint(result)\nprint("after")\n', 'before', True))
        if ty != 'real':
            result.append(Case(f'{ty}-signed-zero-divide', f'{ty} one = 1.0\n{ty} zero = -0.0\nprint(one / zero)\n', neg))
            result.append(Case(f'{ty}-tensor', f'tensor<{ty}> one = tensor.ones<{ty}>([2])\n'
                f'tensor<{ty}> zero = tensor.zeros<{ty}>([2])\n'
                f'tensor<{ty}> result = one / zero\nprint(result[0].item())\n', pos))
            result.append(Case(f'{ty}-tensor-nan', f'tensor<{ty}> zero = tensor.zeros<{ty}>([2])\n'
                f'print("before")\ntensor<{ty}> result = zero / zero\nprint(result[0].item())\n', 'before', True))
    for ty, large in (('real16', '60000.0'), ('real16b', '3.0e38'), ('real32', '3.0e38'), ('real64', '1.0e308')):
        result.append(Case(f'{ty}-overflow', f'{ty} large = {large}\n{ty} result = large * 10.0\nprint(result)\n', 'inf'))
        result.append(Case(f'{ty}-constant-overflow', f'{ty} result = {large} * 10.0\nprint(result)\n', 'inf'))
    for source in ('real16', 'real16b', 'real32', 'real64', 'real'):
        for target in ('real16', 'real16b', 'real32', 'real64', 'real'):
            if source == target: continue
            result.append(Case(f'convert-{source}-{target}', f'{source} a = inf\n{source} b = -inf\n'
                f'{target} x = {target}(a)\n{target} y = {target}(b)\nprint("{{x}} {{y}}")\n',
                '∞ -∞' if target == 'real' else 'inf -inf'))
    result.extend([
        Case('exact-unicode-roundtrip', 'real a = real.parse("∞")\nreal b = real.parse("-∞")\nprint("{a} {b}")\n', '∞ -∞'),
        Case('exact-finite-does-not-overflow', 'real a = 1.0e308 * 10.0\nreal b = inf\nprint(a < b)\n', 'true'),
        Case('exact-infinity-powers', 'real a = inf\nreal b = -inf\nprint("{a ^ 2.0} {b ^ 3.0} {a ^ -2.0} {a ^ 0.0}")\n', '∞ -∞ 0.0 1.0'),
        Case('exact-infinite-exponents', 'real a = 2.0\nreal b = 0.5\nreal e = inf\nprint("{a ^ e} {b ^ e} {a ^ -e} {b ^ -e}")\n', '∞ 0.0 0.0 ∞'),
    ])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('compiler', nargs='?', help='Path to the real Quidra executable')
    parser.add_argument('--case', help='Run only one case by name')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    selected = [c for c in cases() if not args.case or c.name == args.case]
    if not selected: parser.error('Unknown case')
    if args.list:
        print('\n'.join(c.name for c in selected)); return 0
    if not args.compiler: parser.error('A compiler executable is required')
    compiler = Path(args.compiler).expanduser().resolve()
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        parser.error(f'Not an executable: {compiler}')
    records = []
    with tempfile.TemporaryDirectory(prefix='quidra-infinity-') as tmp:
        root = Path(tmp)
        env = os.environ.copy()
        env['QUIDRA_CACHE_DIR'] = str(root / 'cache')
        env['QUIDRA_ERROR_FORMAT'] = 'text'
        for case in selected:
            source = root / (case.name + '.qui')
            source.write_text(case.source, encoding='utf-8')
            try:
                checked = subprocess.run([str(compiler), 'check', str(source)], capture_output=True,
                                         text=True, env=env, timeout=60)
                if checked.returncode:
                    records.append(dict(name=case.name, ok=False, stage='check',
                                        returncode=checked.returncode, stdout=checked.stdout, stderr=checked.stderr))
                    print(f'FAIL {case.name}: rejected by checker'); continue
                ran = subprocess.run([str(compiler), 'run', str(source)], capture_output=True,
                                     text=True, env=env, timeout=60)
                ok = (ran.returncode == (101 if case.failure else 0) and ran.stdout == case.expected)
                if case.failure: ok = ok and 'Quidra runtime error[' in ran.stderr
                records.append(dict(name=case.name, ok=ok, stage='run', returncode=ran.returncode,
                                    stdout=ran.stdout, stderr=ran.stderr, expected=case.expected))
                print(f'{"PASS" if ok else "FAIL"} {case.name}')
            except subprocess.TimeoutExpired as exc:
                records.append(dict(name=case.name, ok=False, stage='timeout', message=str(exc)))
                print(f'FAIL {case.name}: timeout')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    failed = sum(not r['ok'] for r in records)
    print(f'{len(records) - failed}/{len(records)} passed; {failed} failed')
    return int(failed != 0)

if __name__ == '__main__':
    sys.exit(main())
