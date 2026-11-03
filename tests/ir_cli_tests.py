"""Exercise optimizer modes through the actual IR command and JSON reports."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="quidra-ir-cli-") as directory:
        source = Path(directory) / "geometry.qui"
        source.write_text("""tensor<real32> combine(tensor<real32> first, tensor<real32> second)
    tensor<real32>{2, 3} left = first
    tensor<real32>{2, 3} right = second
    return left + right
tensor<real32>{2, 3} first = tensor.zeros<real32>([2, 3])
tensor<real32>{2, 3} second = tensor.zeros<real32>([2, 3])
tensor<real32>{2, 3} result = combine(first, second)
print(result.shape()[0])
""")

        def run(*arguments, success=True):
            result = subprocess.run([str(executable), "ir", *arguments],
                                    capture_output=True, text=True, timeout=30)
            if (result.returncode == 0) != success:
                raise AssertionError(result.stderr or result.stdout)
            return result

        baseline = run(str(source)).stdout
        off = run(str(source), "--proofs", "--step-regions",
                  "--opt=check-elimination=off")
        assert off.stdout.split("proofs ", 1)[0] == baseline
        for mode in ("off", "verify", "on"):
            result = off if mode == "off" else run(
                f"--opt=check-elimination={mode}", str(source), "--proofs",
                "--step-regions", f"--opt=step-regions={mode}")
            proof = json.loads(next(line[7:] for line in result.stdout.splitlines()
                                    if line.startswith("proofs ")))
            regions = json.loads(next(line[13:] for line in result.stdout.splitlines()
                                      if line.startswith("step_regions ")))
            assert proof["schema"] == "quidra.ir.proofs/1"
            assert regions["schema"] == "quidra.ir.step-regions/1"
            assert proof["pass_observed"] == (mode != "off")
            assert regions["pass_observed"] == (mode != "off")
            assert regions["annotation"]["published"] == (mode == "on")
            assert not regions["annotation"]["upstream_budget_complete"]
            rows = proof["functions"]
            assert rows
            if mode != "on":
                assert not any(row["application_observed"] for row in rows)
                assert "broadcast-proven" not in result.stdout.split("proofs ", 1)[0]
            else:
                assert any(row["application"] and row["application"]["applied"]
                           for row in rows)
                assert "broadcast-proven" in result.stdout.split("proofs ", 1)[0]
        for option, diagnostic in (
            ("--opt=unknown=on", "unknown optimization key"),
            ("--opt=inline=perhaps", "unknown optimization mode"),
            ("--opt=interproc=on", "requires exact-binding=on"),
        ):
            assert diagnostic in run(str(source), option, success=False).stderr
        assert "duplicate optimization key" in run(
            str(source), "--opt=inline=off", "--opt=inline=on", success=False).stderr
    print("IR CLI: default output, modes, proof/region receipts and invalid options verified")


if __name__ == "__main__":
    main()
