"""Integration contract: program ownership, attribution and failure flushing."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    probe = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="quidra-observation-") as temporary:
        root = Path(temporary)
        base = {k: v for k, v in os.environ.items()
                if not k.startswith("QUIDRA_COUNTERS") and k not in {
                    "QUIDRA_PROFILE", "QUIDRA_TIMELINE", "QUIDRA_PROFILE_SPLIT",
                    "QUIDRA_METAL_CAPTURE"}}
        off = subprocess.run([probe, "--normal"], env=base, capture_output=True)
        assert off.returncode == 0, off.stderr
        for mode, marks in (("--normal", False), ("--failure", False),
                            ("--compiler", False), ("--normal", True)):
            name = mode + ("-marks" if marks else "")
            timeline = root / (name + "-timeline.json")
            profile = root / (name + "-profile.json")
            counters = root / (name + "-counters.jsonl")
            env = dict(base, QUIDRA_TIMELINE=str(timeline),
                       QUIDRA_PROFILE=str(profile), QUIDRA_COUNTERS=str(counters))
            if marks:
                env["QUIDRA_COUNTERS_STEP"] = "marks"
            result = subprocess.run([probe, mode], env=env, capture_output=True)
            assert result.returncode == (101 if mode == "--failure" else 0), result.stderr
            if mode == "--compiler":
                assert not timeline.exists() and not profile.exists() and not counters.exists()
                continue
            if mode == "--normal":
                assert (result.stdout, result.stderr) == (off.stdout, off.stderr)
            data = json.loads(timeline.read_text())
            hosts = data["marks"] + data["steps"]
            points = [event["point"] for event in hosts]
            for point in ("process_start", "runtime_init", "first_statement",
                          "exit_begin", "exit_end"):
                assert points.count(point) == 1, (mode, point, points)
            assert points.count("step_boundary") == (3 if marks else 1), points
            assert points.count("time_mark") == 2, points
            assert next(e for e in hosts if e["point"] == "first_statement")["site"]["node_id"] == "first"
            trace = json.loads(profile.read_text())
            assert any(event.get("name") == "native range" for event in trace["traceEvents"]), trace
            assert any(event.get("name") == "parallel_for" for event in trace["traceEvents"]), trace
            worker_events = [event for event in trace["traceEvents"]
                if event.get("args", {}).get("site", {}).get("node_id") == "worker body"]
            assert worker_events and all(event["args"]["auto_closed"] is False
                                         for event in worker_events), worker_events
            records = [json.loads(line) for line in counters.read_text().splitlines()]
            assert sum(record.get("record") == "total" for record in records) == 1, records
            assert sum(record.get("event") == "mark" for record in records) == (3 if marks else 1), records
            total = next(record for record in records if record["record"] == "total")
            assert total["package_refusals"] == (0 if mode == "--failure" else 1), total
        print("runtime observation integration: ok")


if __name__ == "__main__":
    main()
