#!/usr/bin/env python3
"""Direct backend capture checks; all diagnostic artifacts use a private temp cache."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

probe = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="quidra-device-capture-") as temporary:
    root = pathlib.Path(temporary)
    def run(name, request, *, fake=False, hold=False, two_boundaries=False, close_before_work=False):
        cache = root / name
        env = {k: v for k, v in os.environ.items() if not k.startswith("QUIDRA_") and k != "MTL_CAPTURE_ENABLED"}
        env.update(QUIDRA_CACHE_DIR=str(cache), QUIDRA_TUNING_DIR=str(root / "tuning"),
                   QUIDRA_METAL_IDLE_OPS="100000", QUIDRA_METAL_IDLE_NS="1000000000")
        if request is not None:
            env.update(QUIDRA_METAL_CAPTURE=request, MTL_CAPTURE_ENABLED="1")
        if fake:
            env["QUIDRA_TEST_FAKE_GPU_COUNT"] = "1"
        args = [str(probe)] + (["--hold"] if hold else []) + (["--capture-two-boundaries"] if two_boundaries else []) + (["--capture-close-before-work"] if close_before_work else [])
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
        if result.returncode == 77:
            return None
        assert result.returncode == 0, (name, result.returncode, result.stdout, result.stderr)
        reports = list(cache.rglob("capture.json")) if cache.exists() else []
        if request is None:
            assert not reports, "capture off created a capture status/session"
            return None
        assert len(reports) == 1, (name, "standalone capture status missing", reports)
        document = json.loads(reports[0].read_text())
        status = document.get("capture", document.get("metal_capture", document))
        return status

    run("off", None, fake=True)
    maximum = run("maximum", "step:18446744073709551615", fake=True)
    assert maximum["requested_step"] == 18446744073709551615
    assert maximum["state"] == "no_metal_work"
    assert not maximum["start_attempted"] and not maximum["start_succeeded"]
    assert maximum["manager_acquires"] == maximum["manager_queries"] == 0
    late = run("closed-before-work", "step:0", close_before_work=True)
    if late is not None:
        assert late["state"] == "no_metal_work" and not late["start_attempted"]
        assert late["manager_acquires"] == late["manager_queries"] == 0
        assert not pathlib.Path(late["output_path"]).exists(), "late work started capture after final status was frozen"
    zero = run("zero", "step:0", two_boundaries=True)
    if zero is not None and zero["start_succeeded"]:
        assert zero["stopped"] and zero["exact_core_step"]
        assert zero["artifact_exists_known"] and zero["artifact_exists"]
        assert zero["created_commands"] == zero["committed_commands"] >= 1
        assert zero["extra_commits"] >= 1, "target-ending capture commit must remain visible"
        one = run("one", "step:1", two_boundaries=True)
        assert one["requested_step"] == 1 and one["exact_core_step"] and one["stopped"]
        assert one["extra_commits"] >= 1, "prior-step capture commit must remain visible"
        held = run("held", "step:0", hold=True)
        assert held["state"] == "partial" and not held["exact_core_step"]
        assert held["held_edges"] >= 1 and held["extra_commits"] == 0
    elif zero is not None:
        assert zero["state"] in {"unsupported", "unavailable", "no_metal_work"}, zero
print("Device capture ownership, step boundaries and artifacts: ok")
