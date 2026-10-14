#!/usr/bin/env python3
"""Timeline buffering, schema, attribution and failure flushing."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


probe = sys.argv[1]


def environment(path=None):
    result = dict(os.environ)
    result.pop("QUIDRA_TIMELINE", None)
    if path is not None:
        result["QUIDRA_TIMELINE"] = str(path)
    return result


def run(mode, path=None, status=0):
    result = subprocess.run([probe, mode], env=environment(path), input="\n",
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == status, (mode, result.returncode, result.stdout, result.stderr)
    assert result.stderr == "", result.stderr
    return result


with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    run("--disabled")
    compiler = root / "compiler.json"
    run("--compiler", compiler)
    assert not compiler.exists(), "compiler-only hooks claimed the program's report"

    buffered = root / "buffered.json"
    process = subprocess.Popen([probe, "--records"], env=environment(buffered), text=True,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    try:
        assert process.stdout.readline() == "ready\n"
        assert not buffered.exists(), "recording performed file I/O before exit"
        output, errors = process.communicate("\n", timeout=10)
        assert process.returncode == 0 and output == "" and errors == "", (output, errors)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    report = json.loads(buffered.read_text())
    assert report["schema"] == 1 and report["clock_units"] == "ns", report
    assert report["clock"] in ("mach_absolute_ns", "clock_monotonic_ns", "qpc_ns")
    assert report["complete"] is False and report["dropped_records"] == {
        "invalid": 6, "storage": 0}, report
    assert report["step_attribution"], "report omitted step-attribution caveat"
    assert [(row["step"], row["host_ns"], row["cpu_ns"]) for row in report["steps"]] == [(0, 300, 40)]
    assert [(row["point"], row["step"], row["host_ns"]) for row in report["marks"]] == [
        ("runtime_init", 0, 100), ("time_mark", 0, 250), ("time_mark", 1, 400)]
    site = {"file": 'unit"file\n.qui', "node_id": "node\\id", "line": 24, "column": 5}
    assert all(row["site"] == site for row in report["steps"] + report["marks"])
    assert [(row["reason"], row["step"], row["begin_ns"], row["end_ns"]) for row in report["waits"]] == [
        ("read", 0, 110, 130), ("sync", 1, 310, 330)]
    assert report["waits"][0]["site"] == site
    assert report["waits"][1]["site"]["file"] == "provider.qui"
    assert report["warmups"][0]["function"] == 7 and report["warmups"][0]["site"] == site
    assert report["compiles"][0]["pipeline"] == 3 and report["compiles"][0]["timed_path"] is False
    assert report["compiles"][0]["begin_ns"] == 180 and report["compiles"][0]["end_ns"] == 220
    assert report["compiles"][0]["site"] == site
    assert len(report["gpu"]) == 103, len(report["gpu"])
    gpu = {row["serial"]: row for row in report["gpu"]}
    assert gpu[11]["commit_step"] == 0 and gpu[11]["site"] == site
    assert (gpu[11]["scheduled_begin_ns"], gpu[11]["scheduled_end_ns"],
            gpu[11]["gpu_begin_ns"], gpu[11]["gpu_end_ns"]) == (240, 260, 260, 290)
    assert gpu[12]["timing_available"] is False and gpu[11]["timing_available"] is True
    assert gpu[(1 << 64) - 1]["bytes"] == (1 << 64) - 1
    assert gpu[(1 << 64) - 1]["ops"] == (1 << 32) - 1
    assert all(serial in gpu for serial in range(100, 200)), "concurrent callbacks lost records"
    assert report["gpu_steps"] == [{"device": 0, "serial": 11, "step": 0,
                                   "gpu_begin_ns": 260, "gpu_end_ns": 290,
                                   "timing_available": True}]
    capabilities = report["device_capabilities"][0]
    assert capabilities["backend"] == "test" and capabilities["architecture"] == "node\\id"
    assert capabilities["device_key"] == 'unit"file\n.qui'
    assert capabilities["compute_units"] == 4 and capabilities["simd_width"] == 8
    assert capabilities["unified_memory"] is True
    assert report["device_capabilities"][1]["unified_memory"] is None
    assert report["device_capabilities"][2]["unified_memory"] is False
    assert report["gpu_attribution_complete"] is False
    sampling = report["gpu_sampling"][0]
    assert sampling["setup_gap"] == "node\\id"
    assert (sampling["sample_buffer_allocations"], sampling["compute_encoders"],
            sampling["additional_compute_encoders"], sampling["sampled_passes"],
            sampling["calibration_queries"], sampling["calibration_host_ns"],
            sampling["resolved_intervals"], sampling["missing_intervals"]) == (1, 2, 1, 3, 2, 40, 2, 1)
    assert report["gpu_sampling_gaps"] == [{"device": 0, "serial": 11, "step": 0,
        "compute": True, "additional_encoder": False, "timing_incomplete": True,
        "reason": 'unit"file\n.qui'}]

    failure = root / "failure.json"
    run("--failure", failure, status=101)
    assert json.loads(failure.read_text()) == report, "failure flush lost buffered fixture records"

    repeated = root / "repeated.json"
    process = subprocess.Popen([probe, "--repeat"], env=environment(repeated), text=True,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    try:
        assert process.stdout.readline() == "flushed\n"
        original = repeated.read_bytes() + b" \n "
        repeated.write_bytes(original)
        output, errors = process.communicate("\n", timeout=10)
        assert process.returncode == 0 and output == "" and errors == "", (output, errors)
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    assert repeated.read_bytes() == original, "repeat flush rewrote the finalized report"
    repeat_report = json.loads(original)
    assert repeat_report["complete"] is True and repeat_report["marks"] == []

    pressure = root / "pressure.json"
    run("--pressure", pressure)
    pressure_report = json.loads(pressure.read_text())
    assert pressure_report["complete"] is False and pressure_report["dropped_records"] == {
        "invalid": 0, "storage": 1}, pressure_report
    assert pressure_report["marks"] == [] and pressure_report["steps"][0]["host_ns"] == 200

    boundary_pressure = root / "boundary-pressure.json"
    run("--boundary-pressure", boundary_pressure)
    boundary_report = json.loads(boundary_pressure.read_text())
    assert boundary_report["dropped_records"] == {"invalid": 1, "storage": 1}, boundary_report
    assert [(row["step"], row["host_ns"]) for row in boundary_report["steps"]] == [(1, 300)]
    assert [(row["step"], row["host_ns"]) for row in boundary_report["marks"]] == [(1, 200)]

    real = root / "real.json"
    run("--real", real)
    real_report = json.loads(real.read_text())
    marks = real_report["marks"]
    assert [row["point"] for row in marks] == ["process_start", "runtime_init", "first_statement", "exit_begin", "exit_end"]
    assert all(row["host_ns"] > 0 for row in marks), marks
    assert marks[0]["host_ns"] <= marks[1]["host_ns"] <= marks[2]["host_ns"] <= marks[3]["host_ns"] <= marks[4]["host_ns"]
    assert marks[0]["cpu_available"] is False and marks[1]["cpu_available"] is True

    # An optional report failure must not replace the producer's exit status.
    run("--records", root)

print("runtime timeline buffering, schema and failure reports: ok")
