#!/usr/bin/env python3
"""Measure Quidra generic monomorphization scaling without CI timing gates.

The benchmark compiles matched program pairs. Both programs define the same N
classes and the same generic function. The baseline reads class fields directly;
the specialized program routes the same values through one concrete generic
instantiation per class. Subtracting the matched baseline helps separate ordinary
source/class growth from specialization work.

Example:
    python3 benchmark/generic_specialization_scaling.py ./build/quidra
    python3 benchmark/generic_specialization_scaling.py ./build/quidra --sizes 4,8,16 --repeats 1
"""

from __future__ import annotations

import argparse
import os
import statistics
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Measurement:
    seconds: float
    binary_bytes: int


def parse_sizes(text: str) -> list[int]:
    try:
        sizes = [int(piece.strip()) for piece in text.split(",") if piece.strip()]
    except ValueError as error:
        raise argparse.ArgumentTypeError("--sizes must be comma-separated positive integers") from error
    if not sizes or any(size <= 0 for size in sizes):
        raise argparse.ArgumentTypeError("--sizes must contain positive integers")
    return sizes


def program_source(count: int, specialized: bool) -> str:
    lines = [
        "int read_value<T>(T value)",
        "    return value.value",
        "",
    ]

    for index in range(count):
        lines.extend(
            [
                f"class Case{index}",
                "    int value",
                "",
            ]
        )

    lines.append("int total = 0")
    for index in range(count):
        type_name = f"Case{index}"
        variable = f"item{index}"
        lines.append(f"{type_name} {variable} = {type_name}(value = {index})")
        if specialized:
            lines.append(f"total += read_value<{type_name}>({variable})")
        else:
            lines.append(f"total += {variable}.value")
    lines.extend(["print(total)", ""])
    return "\n".join(lines)


def build_once(quidra: Path, source_text: str, work_dir: Path, stem: str) -> Measurement:
    source = work_dir / f"{stem}.qui"
    suffix = ".exe" if os.name == "nt" else ""
    binary = work_dir / f"{stem}{suffix}"
    source.write_text(source_text, encoding="utf-8", newline="\n")

    start = time.perf_counter()
    result = subprocess.run(
        [str(quidra), "build", str(source), "-o", str(binary)],
        cwd=work_dir,
        capture_output=True,
        text=True,
    )
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        raise SystemExit(
            f"build failed for {source.name} (exit {result.returncode})\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    if not binary.exists():
        raise SystemExit(f"build reported success but did not create {binary}")
    return Measurement(elapsed, binary.stat().st_size)


def measure(quidra: Path, count: int, repeats: int) -> tuple[Measurement, Measurement]:
    baseline_source = program_source(count, specialized=False)
    specialized_source = program_source(count, specialized=True)

    baseline_times: list[float] = []
    specialized_times: list[float] = []
    baseline_sizes: list[int] = []
    specialized_sizes: list[int] = []

    with tempfile.TemporaryDirectory(prefix="quidra-generic-scaling-") as temporary:
        work_dir = Path(temporary)
        for repeat in range(repeats):
            # Alternate the order so one side does not consistently benefit from
            # filesystem/toolchain warming when multiple repeats are requested.
            order = (False, True) if repeat % 2 == 0 else (True, False)
            for is_specialized in order:
                label = "specialized" if is_specialized else "baseline"
                measurement = build_once(
                    quidra,
                    specialized_source if is_specialized else baseline_source,
                    work_dir,
                    f"n{count}_{label}_{repeat}",
                )
                if is_specialized:
                    specialized_times.append(measurement.seconds)
                    specialized_sizes.append(measurement.binary_bytes)
                else:
                    baseline_times.append(measurement.seconds)
                    baseline_sizes.append(measurement.binary_bytes)

    return (
        Measurement(statistics.median(baseline_times), int(statistics.median(baseline_sizes))),
        Measurement(statistics.median(specialized_times), int(statistics.median(specialized_sizes))),
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("quidra", type=Path, help="path to the built quidra executable")
    parser.add_argument(
        "--sizes",
        type=parse_sizes,
        default=parse_sizes("8,16,32,64"),
        help="comma-separated specialization counts (default: 8,16,32,64)",
    )
    parser.add_argument(
        "--repeats",
        type=int,
        default=3,
        help="builds per baseline/specialized program (default: 3)",
    )
    args = parser.parse_args()

    quidra = args.quidra.expanduser().resolve()
    if not quidra.is_file():
        parser.error(f"quidra executable not found: {quidra}")
    if args.repeats <= 0:
        parser.error("--repeats must be positive")

    print(
        "count  baseline_ms  specialized_ms  delta_ms  time_ratio  "
        "baseline_bytes  specialized_bytes  delta_bytes"
    )
    for count in args.sizes:
        baseline, specialized = measure(quidra, count, args.repeats)
        baseline_ms = baseline.seconds * 1000.0
        specialized_ms = specialized.seconds * 1000.0
        delta_ms = specialized_ms - baseline_ms
        ratio = specialized.seconds / baseline.seconds if baseline.seconds else float("inf")
        delta_bytes = specialized.binary_bytes - baseline.binary_bytes
        print(
            f"{count:5d}  {baseline_ms:11.2f}  {specialized_ms:14.2f}  "
            f"{delta_ms:8.2f}  {ratio:10.3f}  {baseline.binary_bytes:14d}  "
            f"{specialized.binary_bytes:17d}  {delta_bytes:11d}"
        )


if __name__ == "__main__":
    main()
