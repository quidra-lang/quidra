#!/usr/bin/env bash
# Native tail paths, including activations that reuse a physical frame.
set -euo pipefail
python3 - "$1" <<'PY'
import json
from pathlib import Path
import subprocess
import sys
import tempfile

compiler = str(Path(sys.argv[1]).resolve())

def check(root, name, source, expected, *, code=None):
    work = root / name
    work.mkdir()
    (work / 'main.qui').write_text(source)
    built = subprocess.run([compiler, 'build', 'main.qui', '-o', 'program'],
                           cwd=work, text=True, capture_output=True)
    assert built.returncode == 0, (name, built.stdout, built.stderr)
    import os
    env = dict(os.environ, QUIDRA_ERROR_FORMAT='json')
    ran = subprocess.run([str(work / 'program')], cwd=work, env=env,
                         text=True, capture_output=True)
    assert ran.returncode == 101, (name, ran.returncode, ran.stdout, ran.stderr)
    report = json.loads(ran.stderr.splitlines()[0])
    assert report['path_complete'], (name, report)
    path = [(hop['function'], hop['line'], hop['count']) for hop in report['path']]
    assert path == expected, (name, path, expected, report)
    if code is not None:
        assert report['code'] == code, (name, report)
    print('ok  ', name, flush=True)

self_source = '''int32 count(int32 n)
    if n == 0
        return 10 / n
    return count(n - 1)

int32 n := int32("500")
print(count(n))
'''
stale_source = '''int32 count(int32 n, bool fails)
    if n == 0
        if fails
            return 10 / n
        return 0
    return count(n - 1, fails)

int32 result := count(7, false) + count(3, false)
for: 0 <= nat i < 10000
    result = result + count(5, false)
print(result)
int32 n := int32("DEPTH")
print(count(n, true))
'''
exit_source = '''int32 sink(int32 denominator)
    return 10 / denominator

int32 count(int32 n, int32 denominator)
    if n == 0
        return sink(denominator)
    return count(n - 1, denominator)

print(count(7, 1))
print(count(3, 1))
int32 n := int32("DEPTH")
int32 zero := int32("0")
print(count(n, zero))
'''
child_source = '''int32 sink(int32 denominator)
    return 10 / denominator

int32 count(int32 n, bool fails)
    if n == 0
        if fails
            return sink(n) + 1
        return 0
    return count(n - 1, fails)

print(count(7, false))
print(count(3, false))
int32 n := int32("DEPTH")
print(count(n, true))
'''
branch_source = '''int32 sink(int32 denominator)
    return 10 / denominator

int32 choose(bool first, int32 a, int32 b)
    if first
        return sink(a)
    return sink(b)

print(choose(true, 1, 1))
print(choose(false, 1, 1))
int32 zero := int32("0")
print(choose(BRANCH, zero, zero))
'''
unique_source = '''int32 sink(int32 denominator)
    return 10 / denominator

int32 relay(int32 denominator)
    return sink(denominator)

int32 outer(int32 denominator)
    return relay(denominator)

int32 zero := int32("0")
print(outer(zero))
'''
mixed_source = '''int32 count(int32 n)
    if n == 0
        return 10 / n
    if n % 2 == 0
        return count(n - 1)
    return count(n - 1) + 1

int32 n := int32("3000")
print(count(n))
'''
indirect_source = '''int32 sink(int32 denominator)
    return 10 / denominator

int32 apply(func<int32(int32)> operation, int32 denominator)
    return operation(denominator)

int32 zero := int32("0")
print(apply(sink, zero))
'''

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    check(root, 'unique', unique_source, [('sink', 5, 1), ('relay', 8, 1), ('outer', 11, 1)])
    check(root, 'self', self_source, [('count', 4, 500), ('count', 7, 1)])
    check(root, 'self-limit', self_source.replace('"500"', '"5000"'),
          [('count', 4, 4095), ('count', 7, 1)], code='CALL_DEPTH_LIMIT')
    for depth in (0, 4):
        self_hops = [('count', 6, depth)] if depth else []
        check(root, f'stale-{depth}', stale_source.replace('DEPTH', str(depth)),
              self_hops + [('count', 13, 1)])
        self_hops = [('count', 7, depth)] if depth else []
        check(root, f'exit-{depth}', exit_source.replace('DEPTH', str(depth)),
              [('sink', 6, 1)] + self_hops + [('count', 13, 1)])
    for depth in (0, 4):
        self_hops = [('count', 9, depth)] if depth else []
        check(root, f'child-{depth}', child_source.replace('DEPTH', str(depth)),
              [('sink', 7, 1)] + self_hops + [('count', 14, 1)])
    for branch, line in (('true', 6), ('false', 7)):
        check(root, f'branch-{branch}', branch_source.replace('BRANCH', branch),
              [('sink', line, 1), ('choose', 12, 1)])
    check(root, 'indirect', indirect_source, [('sink', 5, 1), ('apply', 8, 1)])

    task_source = self_source.split('int32 n :=')[0] + """void fails()
    int32 n := int32("500")
    print(count(n))

void waits()
    int unused := 0

parallel
    fails()
    waits()
"""
    check(root, 'task', task_source,
          [('count', 4, 500), ('count', 8, 1), ('fails', 14, 1), ('<node body>', 14, 1)])

    # Both sites survive: one physical call and one eliminated tail call.
    # JSON retains the complete alternating path while text elides it.
    work = root / 'mixed'
    work.mkdir()
    (work / 'main.qui').write_text(mixed_source)
    built = subprocess.run([compiler, 'build', 'main.qui', '-o', 'program'],
                           cwd=work, text=True, capture_output=True)
    assert built.returncode == 0, (built.stdout, built.stderr)
    import os
    ran = subprocess.run([str(work / 'program')], cwd=work,
                         env=dict(os.environ, QUIDRA_ERROR_FORMAT='json'),
                         text=True, capture_output=True)
    assert ran.returncode == 101, ran.stderr
    report = json.loads(ran.stderr.splitlines()[0])
    assert report['path_complete'], report
    hops = report['path']
    assert [(h['function'], h['line'], h['count']) for h in hops[:-1]] == [
        ('count', 6 if i % 2 == 0 else 5, 1) for i in range(3000)], report
    assert hops[-1]['function'] == 'count' and hops[-1]['line'] == 9, report
    assert len(hops) == 3001, report
    text = subprocess.run([str(work / 'program')], cwd=work, text=True, capture_output=True)
    assert text.returncode == 101, text.stderr
    assert '… 2981 more …' in text.stderr, text.stderr
    assert 'call path incomplete' not in text.stderr, text.stderr
    print('ok   mixed', flush=True)
print('native tail trace tests passed')
PY
