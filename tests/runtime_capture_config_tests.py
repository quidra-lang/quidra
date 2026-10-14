#!/usr/bin/env python3
"""Capture configuration and typed status without creating any Metal work."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time

probe = sys.argv[1]
MAX = (1 << 64) - 1

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)

    def run(mode, request="step:0", profile=False, extra=None, status=0, bad_cache=False):
        folder = root / ("capture-ユニコード-" + str(len(list(root.iterdir()))))
        folder.mkdir()
        cache = folder / "cache"
        if bad_cache:
            cache.write_text("preserve cache refusal")
        env = dict(os.environ)
        for key in ("QUIDRA_PROFILE", "QUIDRA_PROFILE_SPLIT", "QUIDRA_TIMELINE", "QUIDRA_COUNTERS",
                    "QUIDRA_METAL_CAPTURE", "QUIDRA_COUNTERS_STEP"):
            env.pop(key, None)
        env["QUIDRA_CACHE_DIR"] = str(cache)
        if request is not None:
            env["QUIDRA_METAL_CAPTURE"] = request
        trace = folder / "trace.json"
        if profile:
            env["QUIDRA_PROFILE"] = str(trace)
        env.update(extra or {})
        child = subprocess.run([probe, mode], env=env, capture_output=True, timeout=10)
        assert child.returncode == status, (mode, request, child.returncode, child.stderr)
        assert child.stdout == child.stderr == b"", (mode, child.stdout, child.stderr)
        reports = list(cache.rglob("capture.json")) if cache.is_dir() else []
        if mode in ("--compiler", "--off", "--invalid", "--prepare-pressure"):
            assert not cache.exists(), "unclaimed/off/invalid capture created a cache directory"
            if mode == "--prepare-pressure" and profile:
                return None, json.loads(trace.read_text())
            assert not trace.exists(), "compiler claimed the profile report"
            return None, None
        if bad_cache:
            assert cache.read_text() == "preserve cache refusal"
            assert not reports
            doc = json.loads(trace.read_text()) if profile else None
            return None, doc
        assert len(reports) == 1, "standalone capture request produced no companion status"
        report = json.loads(reports[0].read_text())
        assert report["schema"] == 1 and report["clock_units"] == "ns" and report["kind"] == "metal_capture"
        capture = report["capture"]
        assert capture["requested_step"] == int(request[5:])
        assert Path(capture["report_path"]) == reports[0].resolve()
        output = Path(capture["output_path"])
        assert output.name == "capture.gputrace" and output.parent == reports[0].parent.resolve()
        assert not output.exists(), "portable configuration produced a native capture"
        assert capture["preparation"]["clock_queries"] == 2
        assert capture["preparation"]["failed"] is False
        if os.name != "nt":
            assert stat.S_IMODE(output.parent.stat().st_mode) == 0o700
        doc = json.loads(trace.read_text()) if profile else None
        if doc:
            assert doc["schema"] == 2 and doc["capture"] == capture
        return capture, doc

    run("--compiler", profile=True)
    run("--compiler", f"step:{MAX}")
    run("--off", None)
    for request in ("", "step:", "step:-1", "step:+1", "step: 1", "step:1 ", "step:1x", f"step:{MAX + 1}"):
        run("--invalid", request)
    for request in ("step:0", "step:0000", f"step:{MAX}"):
        capture, _ = run("--entry", request)
        assert capture["state"] == ("no_metal_work" if int(request[5:]) == 0 else "not_reached")
        assert capture["current_step"] == 0 and capture["step_saturated"] is False
        assert capture["start_attempted"] is False and capture["manager_queries"] == 0
        assert capture["selected_device"] is None, "no native device was selected"
        assert capture["start_begin_ns"] is None and capture["artifact_exists"] is None
    capture, _ = run("--owned", f"step:{MAX}", profile=True)
    assert capture["state"] == "stopped" and capture["snapshot_complete"] is True
    assert capture["selected_device"] == 4 and capture["completion_reason"] == "step_ended"
    assert capture["output_path"].endswith("capture.gputrace")
    assert capture["first_serial"] == capture["last_serial"] == MAX
    assert capture["first_step"] == capture["last_step"] == MAX
    assert capture["error"] == {"domain": "owned-domain", "message": "m" * 100, "code": -7,
                                "truncated": False, "storage_failed": False}
    assert capture["manager_acquires"] == capture["manager_releases"] == 1
    assert capture["gaps"] == {"opaque_work_unverified": 3}
    assert capture["exact_core_step"] is True and capture["start_begin_ns"] == 100
    pressure, _ = run("--pressure", profile=True)
    assert pressure["state"] == "stopped" and pressure["error"]["message"] == "m" * 100
    assert pressure["dropped_status_updates"] == 1 and pressure["snapshot_complete"] is False
    assert pressure["retained_snapshot_complete"] is True and pressure["exact_core_step"] is False
    delayed, _ = run("--delayed")
    assert delayed["state"] == "partial" and delayed["exact_core_step"] is False
    assert delayed["gaps"]["invalid_command_lifecycle"] == 1 and delayed["current_step"] == 1
    unknown, _ = run("--snapshot")
    assert unknown["state"] == "unavailable" and unknown["snapshot_complete"] is False
    assert unknown["completion_reason"] is None, "busy fallback cannot know the stop reason"
    assert unknown["selected_device"] is None and unknown["manager_queries"] is None
    assert unknown["start_attempted"] is None and unknown["start_begin_ns"] is None and unknown["error"] is None
    assert unknown["gaps"] == {"stop_busy": 1}
    marks, _ = run("--marks", extra={"QUIDRA_COUNTERS_STEP": "marks"})
    assert marks["current_step"] == 1
    saturated, _ = run("--saturation", f"step:{MAX}")
    assert saturated["current_step"] == MAX and saturated["step_saturated"] is True
    threaded, _ = run("--threads")
    assert threaded["current_step"] == 8000
    late, _ = run("--late-exit")
    assert late["state"] == "no_metal_work"
    late, _ = run("--late-exit", profile=True, extra={"QUIDRA_COUNTERS_STEP": "marks"})
    assert late["state"] == "no_metal_work"
    final, _ = run("--finalizer")
    assert final["extra_commits"] == 1, "normal finalizer failed to publish outside the report lock"
    failed, _ = run("--failure", status=101)
    assert failed["state"] == "interrupted" and failed["completion_reason"] == "immediate_failure"
    assert failed["gaps"] == {"immediate_failure": 1}
    run("--prepare-pressure")
    _, pressure_prep = run("--prepare-pressure", profile=True)
    assert pressure_prep["capture"]["state"] == "unavailable"
    assert pressure_prep["capture"]["dropped_status_updates"] == 1
    assert pressure_prep["capture"]["gaps"] == {"storage_failed": 1}
    _, refused = run("--bad-cache", profile=True, bad_cache=True)
    assert refused["capture"]["state"] == "unavailable"
    assert refused["capture"]["preparation"]["failed"] is True
    assert refused["capture"]["gaps"] == {"path_unavailable": 1}
    if hasattr(os, "mkfifo"):
        folder = root / "concurrent-flush"
        folder.mkdir()
        trace = folder / "trace.fifo"
        os.mkfifo(trace)
        env = {k: v for k, v in os.environ.items() if not k.startswith("QUIDRA_")}
        env.update(QUIDRA_CACHE_DIR=str(folder / "cache"), QUIDRA_PROFILE=str(trace), QUIDRA_METAL_CAPTURE="step:0")
        child = subprocess.Popen([probe, "--concurrent-flush"], env=env, text=True,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            capture = None
            while time.monotonic() < deadline:
                reports = list((folder / "cache").rglob("capture.json"))
                if reports:
                    try:
                        capture = json.loads(reports[0].read_text())["capture"]
                        break
                    except (OSError, json.JSONDecodeError):
                        pass
                time.sleep(0.01)
            assert capture is not None, "companion did not precede the blocked profile write"
            child.stdin.write("\n")
            child.stdin.flush()
            assert child.stdout.readline() == "advanced\n"
            with trace.open() as stream:
                paired = json.load(stream)
            _, errors = child.communicate(timeout=5)
            assert child.returncode == 0 and errors == ""
            assert paired["capture"] == capture, "capture metadata changed between one-shot companion/profile writes"
            assert capture["current_step"] == 0 and capture["dropped_status_updates"] == 0
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()
print("standalone capture configuration, ownership, status and failure safety: ok")
