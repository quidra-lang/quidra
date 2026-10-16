"""Check native optimizer modes, program arguments, and executable cache keys."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="quidra-native-opt-") as directory:
        root = Path(directory)
        environment = dict(os.environ)
        for name in tuple(environment):
            if name.startswith("QUIDRA_") and name not in {
                "QUIDRA_CLANGXX", "QUIDRA_CLANG", "QUIDRA_RUNTIME_LIBRARY",
                "QUIDRA_NATIVE_INCLUDE_DIR",
            }:
                del environment[name]
        environment.update(QUIDRA_CACHE_DIR=str(root / "cache"),
                           QUIDRA_TUNING_DIR=str(root / "tuning"))
        source = root / "modes.qui"
        source.write_text("""cli args
    string opt = option(default = "program")
tensor<real32> first = tensor.ones<real32>([2, 3])
tensor<real32> second = tensor.ones<real32>([2, 3])
tensor<real32> result = first + second
if args.opt == "fail"
    int8 value = int8(127)
    value += int8(1)
print(args.opt)
print(NL)
print(result[1, 2].item() == real32(2))
print(NL)
""")
        library = root / "library.qui"
        library.write_text("""export "C" real32 combined()
    tensor<real32> first = tensor.ones<real32>([2, 3])
    tensor<real32> second = tensor.ones<real32>([2, 3])
    tensor<real32> result = first + second
    return result[1, 2].item()
""")

        def run(*arguments, success=True):
            result = subprocess.run([str(executable), *map(str, arguments)],
                                    cwd=root, env=environment, capture_output=True,
                                    text=True, timeout=90)
            if (result.returncode == 0) != success:
                raise AssertionError(result.stderr or result.stdout)
            return result

        def proven(text):
            return bool(re.search(r"\bcall\s+ptr\s+@quidra_tensor_binary_proven\(", text))

        default_llvm = run("llvm", source).stdout
        off = "--opt=check-elimination=off"
        verify = "--opt=check-elimination=verify"
        on = "--opt=check-elimination=on"
        assert run("llvm", source, off).stdout == default_llvm
        assert run("llvm", source, verify).stdout == default_llvm
        assert not proven(default_llvm)
        assert proven(run("llvm", source, on).stdout)
        for command in ("llvm", "run", "build"):
            rejected = run(command, source, "--opt=interproc=on", success=False)
            assert "requires exact-binding=on" in rejected.stderr
        assert "duplicate optimization key" in run(
            "run", source, on, on, success=False).stderr

        # The executable cache rejects newly changed inputs during its two
        # second file-identity window. Leave the authored inputs quiet first.
        time.sleep(2.1)
        assert run("run", source, on).stdout == "program\ntrue\n"

        def entries():
            return [json.loads(path.read_text())
                    for path in (root / "cache" / "run").rglob("metadata.json")]

        first = entries()
        if os.name == "nt":
            # Windows deliberately does not claim a complete native linker
            # dependency trace: publishing such a cache entry would be unsafe.
            assert not first, "Windows published an incompletely traced native cache"
        else:
            assert len(first) == 1, "actual native run did not publish its mode cache entry"
            first_key = first[0]["pre_key"]
            first_options = {value["name"]: value["value"]
                             for value in first[0]["invocation"]["options"]}
            assert first_options["compile.optimizer.check_elimination"] == "on"
            assert "compile.optimizer.cfg_facts" not in first_options, \
                "omitted optional mode was flattened into an explicit default"

        assert run(on, source, "--opt", "child").stdout == "child\ntrue\n"
        if os.name != "nt":
            assert {entry["pre_key"] for entry in entries()} == {first_key}, \
                "shorthand prefix mode or child arguments changed the executable key"
        assert run("run", source, off, "--", "--opt", "child").stdout == "child\ntrue\n"
        if os.name != "nt":
            second = entries()
            assert len(second) == 2 and len({entry["pre_key"] for entry in second}) == 2, \
                "different optimizer modes shared cached executable identity"
        assert run("run", source, on, "--opt=cfg-facts=on").stdout == "program\ntrue\n"
        if os.name != "nt":
            explicit = entries()
            assert len(explicit) == 3 and len({entry["pre_key"] for entry in explicit}) == 3, \
                "explicit optional On shared the omitted optional mode cache key"
            assert any(any(value["name"] == "compile.optimizer.cfg_facts" and value["value"] == "on"
                           for value in entry["invocation"]["options"])
                       for entry in explicit)
        assert run("run", source, "--no-cache", on, "--", "--opt", "child").stdout == "child\ntrue\n"
        if os.name == "nt":
            assert not entries(), "Windows native cache must remain unpublished"
        else:
            assert len(entries()) == 3, "uncached mode unexpectedly published an entry"

        errors = [run("run", source, option, "--", "--opt", "fail", success=False)
                  for option in (off, on)]
        assert all(result.returncode == 101 for result in errors)
        assert (errors[0].stdout, errors[0].stderr) == (errors[1].stdout, errors[1].stderr), \
            "native mode changed the first checked error or its attribution"

        output = root / ("mode-program.exe" if os.name == "nt" else "mode-program")
        run("build", source, on, "--keep-llvm", "-o", output)
        assert output.exists() and proven(Path(str(output) + ".ll").read_text())
        built = subprocess.run([str(output), "--opt", "built"], cwd=root,
                               env=environment, capture_output=True, text=True, timeout=30)
        assert built.returncode == 0 and built.stdout == "built\ntrue\n"
        archive = root / ("mode-library.lib" if os.name == "nt" else "mode-library.a")
        run("build", library, "--lib", on, "--keep-llvm", "-o", archive)
        assert archive.exists() and proven(Path(str(archive) + ".ll").read_text()), \
            "library compilation ignored the native optimizer mode"
    print("native optimizer CLI: lowering, native parity, arguments and cache modes verified")


if __name__ == "__main__":
    main()
