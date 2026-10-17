#!/usr/bin/env python3
"""Exercise actual native initialization checks on aliased array selectors."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
import tempfile


@dataclass(frozen=True)
class Case:
    name: str
    source: str
    expected: str
    fails: bool


def cases() -> list[Case]:
    result = []
    for dtype in ("int", "nat", "bool"):
        first, second = ("true", "false") if dtype == "bool" else ("1", "0")
        values = "int[2] values = [10, 20]\n"
        declaration = f"{dtype}[2] selector\n"
        for through_parameter in (False, True):
            name = "parameter" if through_parameter else "alias"
            if through_parameter:
                prefix = (f"int[] choose(int[2] values, {dtype}[2] &selector)\n"
                          "    return values[selector]\n" + values + declaration)
                select = "int[] result = choose(values, &selector)\n"
                target = "selector"
            else:
                prefix = values + declaration + f"{dtype}[2] &alias = &selector\n"
                select = "int[] result = values[alias]\n"
                target = "alias"
            for written in (0, 1, 2):
                writes = ""
                if written >= 1:
                    writes += f"{target}[0] = {first}\n"
                if written == 2:
                    writes += f"{target}[1] = {second}\n"
                # The prefix proves runtime was reached; compilation/link errors
                # and an early unrelated failure cannot satisfy this assertion.
                source = prefix + writes + 'print("before")\n' + select
                if written < 2:
                    source += 'print("after")\n'
                    expected = "before"
                else:
                    count, value = (1, 10) if dtype == "bool" else (2, 20)
                    source += f"print(len(result) == {count} and result[0] == {value})\n"
                    if dtype != "bool":
                        source += "print(result[1] == 10)\n"
                    expected = "beforetrue" + ("" if dtype == "bool" else "true")
                result.append(Case(f"{dtype}-{name}-{written}-initialized", source,
                                   expected, written < 2))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.expanduser().resolve()
    if not compiler.is_file() or not os.access(compiler, os.X_OK):
        parser.error(f"Not an executable: {compiler}")
    failures = 0
    tests = cases()
    with tempfile.TemporaryDirectory(prefix="quidra-selectors-") as tmp:
        root = Path(tmp)
        env = os.environ.copy()
        env["QUIDRA_CACHE_DIR"] = str(root / "cache")
        env["QUIDRA_ERROR_FORMAT"] = "text"
        for case in tests:
            source = root / (case.name + ".qui")
            source.write_text(case.source, encoding="utf-8")
            try:
                checked = subprocess.run([str(compiler), "check", str(source)],
                                         capture_output=True, text=True, env=env, timeout=60)
                if checked.returncode:
                    failures += 1
                    print(f"FAIL {case.name}: compile check\n{checked.stdout}{checked.stderr}")
                    continue
                ran = subprocess.run([str(compiler), "run", str(source)],
                                     capture_output=True, text=True, env=env, timeout=60)
                ok = ran.returncode == (101 if case.fails else 0) and ran.stdout == case.expected
                if case.fails:
                    ok = ok and "Quidra runtime error[UNINITIALIZED]" in ran.stderr
                if not ok:
                    failures += 1
                    print(f"FAIL {case.name}: exit={ran.returncode}, "
                          f"stdout={ran.stdout!r}, stderr={ran.stderr!r}")
                else:
                    print(f"PASS {case.name}")
            except subprocess.TimeoutExpired:
                failures += 1
                print(f"FAIL {case.name}: timeout")
    print(f"{len(tests) - failures}/{len(tests)} native selector contracts passed")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
