"""Check proven tensor entries preserve values, work and mandatory failures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="quidra-tensor-proven-") as directory:
        env = dict(os.environ)
        env["QUIDRA_CACHE_DIR"] = str(Path(directory) / "cache")
        env["QUIDRA_TUNING_DIR"] = str(Path(directory) / "tuning")

        def run(*args):
            return subprocess.run([str(executable), *args], env=env,
                                  capture_output=True, timeout=30)

        result = run()
        if result.returncode:
            sys.stderr.buffer.write(result.stdout + result.stderr)
            raise AssertionError("tensor proven value/counter comparison failed")
        original_errors = {
            "overflow-first": b"Quidra runtime error[TENSOR]: tensor integer arithmetic overflow\n",
            "uninitialized": b"Quidra runtime error[UNINITIALIZED]: value is uninitialized\n",
            "nonfinite": b"Quidra runtime error[REAL_OVERFLOW]: tensor<real32> multiplication overflow\n",
            "dtype": b"Quidra runtime error[TENSOR]: tensor operands must have identical element types\n",
            "compare-init": b"Quidra runtime error[UNINITIALIZED]: value is uninitialized\n",
        }
        for case, original_error in original_errors.items():
            checked = run("--error", case, "checked")
            proven = run("--error", case, "proven")
            if checked.returncode != 101 or proven.returncode != 101:
                sys.stderr.buffer.write(checked.stderr + proven.stderr)
                raise AssertionError(f"{case}: mandatory failure was bypassed")
            # Check the original text across platform newline conventions;
            # checked/proven output still has to match byte for byte.
            if (checked.stdout, checked.stderr.replace(b"\r\n", b"\n")) != (b"", original_error) or \
                    (checked.stdout, checked.stderr) != (proven.stdout, proven.stderr):
                sys.stderr.buffer.write(checked.stderr + proven.stderr)
                raise AssertionError(f"{case}: original first failure changed")
        sys.stdout.buffer.write(result.stdout)
        print("tensor proven entries: all five mandatory failures preserved")


if __name__ == "__main__":
    main()
