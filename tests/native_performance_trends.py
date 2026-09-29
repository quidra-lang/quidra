#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import pathlib
import statistics
import subprocess
import sys
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
        auto | error next = output.read_line()
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

auto | error result = run()
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
    "file_read_whole": """string text = file.read("parse-input.txt")
print(len(text))
""",
    "file_split_whole": """string text = file.read("parse-input.txt")
string[] rows = text.split(ENTER)
int lines = 0
for line in rows
    if line != ""
        lines += 1
print(lines)
""",
    "file_read_stream": """file.Handle reader = file.open("parse-input.txt")
int lines = 0
while true
    auto | error next = reader.read_line()
    match next
        string line
            lines += 1
        none
            break
        error problem
            process.exit(3)
reader.close()
print(lines)
""",
    "file_parse_stream": """file.Handle reader = file.open("parse-input.txt")
int lines = 0
int checksum = 0
while true
    auto | error next = reader.read_line()
    match next
        string line
            string[] fields = line.split(" ")
            int parsed_index = int.parse(fields[0])
            int value = int.parse(fields[1])
            if parsed_index != lines
                process.exit(3)
            lines += 1
            checksum = (checksum + value) % 1000000007
        none
            break
        error problem
            process.exit(3)
reader.close()
print("{lines} {checksum}")
""",
    "file_parse_whole": """string text = file.read("parse-input.txt")
string[] rows = text.split(ENTER)
int lines = 0
int checksum = 0
for line in rows
    if line != ""
        string[] fields = line.split(" ")
        int parsed_index = int.parse(fields[0])
        int value = int.parse(fields[1])
        if parsed_index != lines
            process.exit(3)
        lines += 1
        checksum = (checksum + value) % 1000000007
print("{lines} {checksum}")
""",
}

FILE_PARSE_LINES = 100000
FILE_PARSE_CHECKSUM_MODULUS = 1000000007

def write_file_parse_fixture(root):
    path = root / "parse-input.txt"
    checksum = 0
    with path.open("w", encoding="utf-8", newline="") as handle:
        for index in range(FILE_PARSE_LINES):
            value = index % 100003
            checksum = (checksum + value) % FILE_PARSE_CHECKSUM_MODULUS
            handle.write(f"{index} {value}\n")
    return path, checksum

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


def peak_rss_bytes(binary, cwd):
    # Measure RSS in a fresh helper process so RUSAGE_CHILDREN contains exactly
    # this binary rather than the maximum from earlier representative runs.
    helper = r"""
import json, resource, subprocess, sys
completed = subprocess.run([sys.argv[1]], cwd=sys.argv[2],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
usage = resource.getrusage(resource.RUSAGE_CHILDREN)
scale = 1 if sys.platform == "darwin" else 1024
print(json.dumps({
    "returncode": completed.returncode,
    "stdout": completed.stdout.decode("utf-8", "replace"),
    "stderr": completed.stderr.decode("utf-8", "replace"),
    "peak_rss_bytes": int(usage.ru_maxrss * scale),
}))
"""
    measured = subprocess.run(
        [sys.executable, "-c", helper, binary, str(cwd)],
        cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=True, text=True, timeout=180,
    )
    data = json.loads(measured.stdout)
    if data["returncode"] != 0:
        raise RuntimeError(
            f"RSS measurement failed ({data['returncode']}): {binary}\n"
            + data["stderr"]
        )
    return data["peak_rss_bytes"], data["stdout"]

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
        fixture_path, fixture_checksum = write_file_parse_fixture(root)
        fixture_bytes = fixture_path.stat().st_size
        result["file_parse_fixture"] = {
            "lines": FILE_PARSE_LINES,
            "bytes": fixture_bytes,
            "checksum": fixture_checksum,
        }
        for name, source in WORKLOADS.items():
            source_path = root / f"{name}.qui"
            binary_path = root / f"{name}.bin"
            source_path.write_text(source)

            started = time.perf_counter()
            run_checked([quidra, "build", str(source_path), "-o", str(binary_path)], cwd=root)
            build_seconds = time.perf_counter() - started

            validation = run_checked(
                [str(binary_path)], cwd=root, stdout=subprocess.PIPE)
            output_bytes = validation.stdout
            rss_bytes, rss_output = peak_rss_bytes(str(binary_path), root)
            if rss_output.encode("utf-8") != output_bytes:
                raise RuntimeError(
                    f"{name}: output changed between correctness and RSS runs")

            samples = []
            for _ in range(args.repeats):
                started = time.perf_counter()
                run_checked([str(binary_path)], cwd=root)
                samples.append(time.perf_counter() - started)

            if name == "file_read_whole":
                expected = f"{fixture_bytes}\n".encode()
                if output_bytes != expected:
                    raise RuntimeError(
                        f"{name}: expected byte count {fixture_bytes}, got {output_bytes!r}")
            elif name in {"file_split_whole", "file_read_stream"}:
                expected = f"{FILE_PARSE_LINES}\n".encode()
                if output_bytes != expected:
                    raise RuntimeError(
                        f"{name}: expected line count {FILE_PARSE_LINES}, got {output_bytes!r}")
            elif name in {"file_parse_stream", "file_parse_whole"}:
                expected = f"{FILE_PARSE_LINES} {fixture_checksum}\n".encode()
                if output_bytes != expected:
                    raise RuntimeError(
                        f"{name}: parse checksum mismatch: {output_bytes!r}")

            result["workloads"][name] = {
                "build_seconds": build_seconds,
                "run_seconds": samples,
                "run_median_seconds": statistics.median(samples),
                "peak_rss_bytes": rss_bytes,
                "output_sha256": hashlib.sha256(output_bytes).hexdigest(),
                "artifact_bytes": binary_path.stat().st_size,
            }

        stream = result["workloads"]["file_parse_stream"]
        whole = result["workloads"]["file_parse_whole"]
        if stream["output_sha256"] != whole["output_sha256"]:
            raise RuntimeError(
                "streaming and whole-file parse paths produced different output")

        # Keep the original 12.49 s / ~1.6 GiB report from silently returning.
        # These are intentionally loose regression envelopes, not tuning targets.
        max_parse_seconds = 2.0
        max_parse_rss_bytes = 256 * 1024 * 1024
        for name in ("file_parse_stream", "file_parse_whole"):
            row = result["workloads"][name]
            if row["run_median_seconds"] > max_parse_seconds:
                raise RuntimeError(
                    f"{name}: median runtime regression "
                    f"{row['run_median_seconds']:.3f}s > {max_parse_seconds:.1f}s")
            if row["peak_rss_bytes"] > max_parse_rss_bytes:
                raise RuntimeError(
                    f"{name}: peak RSS regression "
                    f"{row['peak_rss_bytes']} > {max_parse_rss_bytes}")

        read_whole = result["workloads"]["file_read_whole"]["run_median_seconds"]
        split_whole = result["workloads"]["file_split_whole"]["run_median_seconds"]
        read_stream = result["workloads"]["file_read_stream"]["run_median_seconds"]
        result["file_parse_profile"] = {
            "whole_file_read_seconds": read_whole,
            "whole_file_split_increment_seconds": max(0.0, split_whole - read_whole),
            "whole_file_parse_increment_seconds": max(
                0.0, whole["run_median_seconds"] - split_whole),
            "stream_line_traversal_seconds": read_stream,
            "stream_split_parse_increment_seconds": max(
                0.0, stream["run_median_seconds"] - read_stream),
        }

    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
