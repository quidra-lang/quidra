#!/usr/bin/env python3
"""Profiler buffering, owned attribution, traces and allocation-failure safety."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

probe = sys.argv[1]


def environment(path=None):
    env = dict(os.environ)
    for key in ("QUIDRA_PROFILE", "QUIDRA_PROFILE_SPLIT", "QUIDRA_METAL_CAPTURE"):
        env.pop(key, None)
    if path is not None:
        env["QUIDRA_PROFILE"] = str(path)
        env["QUIDRA_CACHE_DIR"] = str(path.parent / "cache")
    return env


def run(mode, path=None, status=0, extra=None):
    env = environment(path)
    env.update(extra or {})
    result = subprocess.run([probe, mode], env=env, input="\n", text=True,
                            capture_output=True, timeout=15)
    assert result.returncode == status, (mode, result.returncode, result.stderr)
    assert result.stderr == "", result.stderr
    return json.loads(path.read_text()) if path is not None and path.is_file() else None


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    run("--off")
    compiler = root / "compiler.json"
    run("--compiler", compiler)
    assert not compiler.exists(), "compiler claimed the report"
    typed = run("--typed-gpu", root / "typed-gpu.json")
    mixed = next(r for r in typed["aggregate"] if r["node_id"] == "mixed")
    assert mixed["gpu_ns"] == 3000, "nested GPU container time counted as activity"
    assert typed["schema"] == 2 and typed["gpu_aggregation"] == "typed_activity_container"
    assert typed["bytes_scope"] == "inclusive_events"
    assert (mixed["calls"], mixed["activity_calls"], mixed["activity_gpu_known_calls"],
            mixed["activity_gpu_unknown_calls"], mixed["activity_gpu_ns"], mixed["activity_bytes"]) == (7, 3, 2, 1, 3000, 56)
    assert (mixed["container_calls"], mixed["container_gpu_known_calls"],
            mixed["container_gpu_unknown_calls"], mixed["container_gpu_ns"], mixed["container_bytes"]) == (3, 2, 1, 28000, (1 << 64) - 1)
    assert mixed["activity_overflow"] is False and mixed["container_overflow"] is True
    assert mixed["gpu_calls"] == 2 and mixed["gpu_unknown_calls"] == 1
    large = next(r for r in typed["aggregate"] if r["node_id"] == "container")
    assert large["activity_calls"] == large["gpu_ns"] == 0 and large["container_gpu_ns"] == 999000
    spans = [e for e in typed["traceEvents"] if e.get("cat") == "gpu"]
    assert len(spans) == 5, "container Chrome spans were removed"
    assert sum(e["args"]["gpu_role"] == "container" for e in spans) == 3
    assert typed["complete"] is True
    roles = run("--gpu-roles", root / "gpu-roles.json")
    assert roles["dropped_records"] == {"invalid": 3, "storage": 0}
    roles = {r["kernel"]: r for r in roles["aggregate"]}
    assert roles["explicit activity"]["activity_gpu_ns"] == 100
    assert roles["explicit container"]["container_gpu_ns"] == 100
    assert roles["CPU dispatch"]["activity_calls"] == roles["CPU dispatch"]["container_calls"] == 0
    buffered = root / "buffered.json"
    process = subprocess.Popen([probe, "--records"], env=environment(buffered), text=True,
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert process.stdout.readline() == "ready\n"
        assert not buffered.exists(), "recording wrote before exit"
        _, errors = process.communicate("\n", timeout=15)
        assert process.returncode == 0 and errors == "", errors
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    report = json.loads(buffered.read_text())
    assert report["schema"] == 2 and report["clock_units"] == "ns"
    assert report["dropped_records"] == {"invalid": 4, "storage": 0}, report
    groups = {(r["node_id"], r["owner"], r["kernel"]): r for r in report["aggregate"]}
    gemm = groups[("node\\id", "core", "gemm")]
    assert (gemm["calls"], gemm["host_us"], gemm["gpu_us"], gemm["bytes"]) == (2, 5, 11, 128)
    unknown = groups[("node\\id", "native", "gemm")]
    assert unknown["host_unknown_calls"] == unknown["gpu_unknown_calls"] == 1
    assert groups[("provider-node", "test", "callback")]["calls"] == 100
    assert groups[("first", "quidra", "statement")]["calls"] == 1
    assert groups[("second", "quidra", "statement")]["calls"] == 1
    wide = groups[("wide", "test", "wide")]
    assert wide["calls"] == 2 and wide["bytes"] == (1 << 64) - 1 and wide["overflow"] is True
    gpu = [e for e in report["traceEvents"] if e.get("ph") == "X" and e.get("cat") == "gpu"]
    assert len(gpu) == 2 and all(e["pid"] == 2 and e["tid"] == 3 for e in gpu)
    assert all((e["ts"], e["dur"]) == (4, 5.5) for e in gpu)
    assert all(e["name"] == 'kernel"name' for e in gpu)
    assert all(e["args"]["site"]["file"] == 'unit"file\n.qui' for e in gpu)
    assert report["open_ranges"] == 0
    capabilities = report["device_capabilities"][0]
    assert capabilities["architecture"] == "node\\id"
    assert capabilities["device_key"] == 'unit"file\n.qui'
    assert capabilities["compute_units"] == 4 and capabilities["simd_width"] == 32
    assert capabilities["unified_memory"] is True
    assert report["device_capabilities"][1]["unified_memory"] is None
    assert report["device_capabilities"][2]["unified_memory"] is False
    for mode, status in (("--failure", 101), ("--repeat", 0)):
        result = run(mode, root / (mode + ".json"), status)
        assert result["open_ranges"] == 0
        assert len(result["traceEvents"]) == len(report["traceEvents"])
    opened = run("--open", root / "open.json")
    assert opened["open_ranges"] == 1 and opened["complete"] is False
    pressure = run("--pressure", root / "pressure.json")
    assert pressure["dropped_records"] == {"invalid": 0, "storage": 1}
    assert pressure["aggregate"][0]["calls"] == 4096
    transitions = run("--transition-pressure", root / "transitions.json")
    assert transitions["dropped_records"] == {"invalid": 0, "storage": 1}
    assert {r["node_id"] for r in transitions["aggregate"]} == {"first", "last"}
    assert all(r["host_calls"] == 1 for r in transitions["aggregate"])
    assert all(e["args"]["auto_closed"] is False for e in transitions["traceEvents"] if e.get("ph") == "X")
    config = run("--repeat", root / "config.json", extra={
        "QUIDRA_PROFILE_SPLIT": "1", "QUIDRA_METAL_CAPTURE": "step:12"})
    assert config["configuration"]["split_requested"] is True
    assert config["configuration"]["capture_step"] == 12
    invalid = run("--repeat", root / "invalid.json", extra={"QUIDRA_METAL_CAPTURE": "step:-1"})
    assert invalid["configuration"]["capture_step"] is None
    run("--records", root)
print("runtime profiler buffering, attribution and failure reports: ok")
