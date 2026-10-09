#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys
import tempfile

quidra = pathlib.Path(sys.argv[1]).resolve()

# `quidra run` keeps the programs it builds in a run cache of this suite run
# only, so every run of the suite sees the same hits and misses.
RUN_CACHE = tempfile.TemporaryDirectory(prefix="quidra-cache-")
os.environ["QUIDRA_CACHE_DIR"] = RUN_CACHE.name

with tempfile.TemporaryDirectory(prefix="quidra process ") as temporary:
    root = pathlib.Path(temporary)
    output = root / "shell output.txt"
    output_text = str(output)
    if '"' in output_text or "{" in output_text or "}" in output_text:
        raise SystemExit("unexpected temporary path spelling")

    if os.name == "nt":
        helper = root / "quoted command.cmd"
        helper.write_text(
            '@echo off\r\n'
            '<nul set /p "=shell-ok" > "%~1"\r\n'
            'exit /b 0\r\n',
            encoding="utf-8",
        )
        helper_text = str(helper)
        command = (
            'string command = '
            f'"{{DQ}}{helper_text}{{DQ}} '
            '{DQ}{output_path}{DQ}"'
        )
    else:
        command = (
            'string command = "printf shell-ok > '
            '{DQ}{output_path}{DQ}"'
        )

    source = root / "process-portability.qui"
    source.write_text(
        f'''string output_path = "{output_text}"
{command}
process.Result result = process.shell(command)
print(result.started)
print(NL)
print(result.status == 0)
print(NL)
auto | error loaded = file.read(output_path)
match loaded
    string value
        print(value == "shell-ok")
        print(NL)
    error problem
        print(false)
        print(NL)
''',
        encoding="utf-8",
    )

    completed = subprocess.run(
        [str(quidra), "run", str(source)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        check=False,
    )
    expected = "true\ntrue\ntrue"
    actual = completed.stdout.strip()
    if completed.returncode != 0 or actual != expected:
        sys.stderr.write(
            "process.shell portability regression\n"
            f"status: {completed.returncode}\n"
            f"expected:\n{expected}\n"
            f"actual:\n{actual}\n"
            f"stderr:\n{completed.stderr}\n"
        )
        raise SystemExit(1)
