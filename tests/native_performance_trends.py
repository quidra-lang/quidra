#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import statistics
import subprocess
import tempfile
import time

WORKLOADS = {
    "bitwise": """int value = 1
for index in range(2000000)
    value = ((value XOR index) << 1) XOR (value >> 3)
print(value)
""",
    "sort": """int[] values = array(50000, fill = 0)
for index in range(50000)
    values[index] = 50000 - index
values = values.sorted()
print(values[0])
""",
    "string_concat": """string text = ""
for index in range(200000)
    text += "x"
print(len(text))
""",
    "stream_file": """int | error run()
    file.Handle output = try file.create("trend-output.txt")
    for index in range(50000)
        try output.write_line("abcdef")
    try output.flush()
    try output.seek(0)

    int lines = 0
    while true
        auto next = output.read_line()
        match next
            string line
                if line != "abcdef"
                    return error("unexpected file contents")
                lines += 1
            none
                break
            error problem
                return problem
    output.close()
    return lines

auto result = run()
match result
    int value
        print(value)
    error problem
        print(problem)
""",
    "map_delete": """map.Map<int, int> values = map.Map<int, int>()
for index in range(50000)
    values.set(index, index)
for index in range(0, 50000, 2)
    values.remove(index)
print(values.size())
""",
}

def run_checked(command, *, cwd, stdout=subprocess.DEVNULL):
    completed = subprocess.run(
        command, cwd=cwd, stdout=stdout, stderr=subprocess.PIPE,
        check=False, timeout=180,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {command}\n"
            + completed.stderr.decode("utf-8", "replace")
        )
    return completed

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("quidra")
    parser.add_argument("--output", required=True)
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()

    quidra = os.path.abspath(args.quidra)
    result = {"kind": "native-representative-trends", "repeats": args.repeats, "workloads": {}}

    with tempfile.TemporaryDirectory(prefix="quidra-native-trends-") as tmp:
        root = pathlib.Path(tmp)
        for name, source in WORKLOADS.items():
            source_path = root / f"{name}.qui"
            binary_path = root / f"{name}.bin"
            source_path.write_text(source)

            started = time.perf_counter()
            run_checked([quidra, "build", str(source_path), "-o", str(binary_path)], cwd=root)
            build_seconds = time.perf_counter() - started

            run_checked([str(binary_path)], cwd=root)
            samples = []
            for _ in range(args.repeats):
                started = time.perf_counter()
                run_checked([str(binary_path)], cwd=root)
                samples.append(time.perf_counter() - started)

            result["workloads"][name] = {
                "build_seconds": build_seconds,
                "run_seconds": samples,
                "run_median_seconds": statistics.median(samples),
                "artifact_bytes": binary_path.stat().st_size,
            }

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
