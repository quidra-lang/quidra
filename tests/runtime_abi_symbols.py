#!/usr/bin/env python3
"""Every runtime entry point that the backend's prelude declares is defined
by the runtime library.

  runtime_abi_symbols.py BACKEND_TESTS RUNTIME_ARCHIVE

BACKEND_TESTS --runtime-symbols prints the declared symbols (their
signatures come from the C prototypes, src/llvm_backend/runtime_abi.hpp);
each must be a defined, external text symbol of RUNTIME_ARCHIVE according to
nm (NM in the environment, else llvm-nm or nm on PATH).
"""
import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    tool, archive = sys.argv[1], sys.argv[2]
    nm = os.environ.get("NM") or shutil.which("llvm-nm") or shutil.which("nm")
    if not nm:
        print("runtime_abi_symbols: no nm found", file=sys.stderr)
        return 2
    declared = subprocess.run([tool, "--runtime-symbols"], check=True, capture_output=True,
                              text=True).stdout.split()
    listing = subprocess.run([nm, "-g", "--defined-only", archive], check=True,
                             capture_output=True, text=True).stdout
    defined = set()
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] == "T":
            symbol = fields[2]
            # Mach-O and 32-bit Windows prefix C symbols with an underscore.
            defined.add(symbol)
            if symbol.startswith("_"):
                defined.add(symbol[1:])
    missing = [symbol for symbol in declared if symbol not in defined]
    if not declared:
        print("runtime_abi_symbols: the prelude declares no runtime entry point", file=sys.stderr)
        return 1
    if missing:
        print("runtime_abi_symbols: declared but not defined by the runtime library: " +
              ", ".join(missing), file=sys.stderr)
        return 1
    print(f"runtime_abi_symbols: {len(declared)} declared runtime entry points are defined")
    return 0


if __name__ == "__main__":
    sys.exit(main())
