#!/usr/bin/env python3
"""Native integer radix members, checked against an independent exact oracle."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def radix_text(value, base, uppercase=False):
    digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" if uppercase else "0123456789abcdefghijklmnopqrstuvwxyz"
    magnitude = abs(value)
    text = ""
    while magnitude:
        magnitude, digit = divmod(magnitude, base)
        text = digits[digit] + text
    return ("-" if value < 0 else "") + (text or "0")


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    lines = []
    expected = []

    def output(expression, text):
        lines.append(f"println({expression})")
        expected.append(str(text))

    # Every signed/unsigned width, including minima and the unsigned maximum.
    cases = [
        ("int", -(2**160 + 255)), ("nat", 2**160 + 255),
        ("int8", -(2**7)), ("int16", -(2**15)),
        ("int32", -(2**31)), ("int64", -(2**63)),
        ("nat8", 2**8 - 1), ("nat16", 2**16 - 1),
        ("nat32", 2**32 - 1), ("nat64", 2**64 - 1),
    ]
    for kind, value in cases:
        name = "value_" + kind
        lines.append(f"{kind} {name} := {value}")
        for base in (2, 8, 16, 36):
            for uppercase in (False, True):
                member = ("B" if uppercase else "b") + str(base)
                output(f"{name}.{member}", radix_text(value, base, uppercase))
        lines.append(f"{kind} zero_{kind} := 0")
        output(f"zero_{kind}.B16", "0")
        output(f'{name}.b16 == "{{{name}:base=16}}"', "true")
        output(f'{name}.B36 == "{{{name}:base=36,upper}}"', "true")
        lines += [f"string reference_{kind}({kind} &value)", "    return value.b16"]
        output(f"reference_{kind}(&{name})", radix_text(value, 16))
        lines += [f"string parameter_{kind}({kind} value)", "    return value.B36"]
        output(f"parameter_{kind}({name})", radix_text(value, 36, True))

    # All 70 canonical names, not just the familiar binary/octal/hexadecimal.
    lines.append("int all_bases := 123456789012345678901234567890")
    for base in range(2, 37):
        for uppercase in (False, True):
            member = ("B" if uppercase else "b") + str(base)
            output(f"all_bases.{member}", radix_text(123456789012345678901234567890, base, uppercase))

    output("0.b2", "0")
    output("0.B36", "0")
    output("255.b16", "ff")
    output("(-255).B16", "-FF")
    output("(128 + 127).b16", "ff")
    lines += ["inferred := 255.b16", "string copied := inferred"]
    output('copied + ":" + 35.B36', "ff:Z")
    output("255.b16.upper_ascii()", "FF")
    lines += ["T identity<T>(T value)", "    return value"]
    output("identity(255.b16)", "ff")
    output("identity(copied)", "ff")
    output('"{255.b16} {(-255).B16}"', "ff -FF")
    output("255.b16[0]", "f")
    output("255.b16.len()", "2")

    lines += [
        "class Fields()", "    int b16 := 7", '    string B16 := "field"',
        "    int b02 := 2", "Fields fields := Fields()",
        "class Methods()", "    string b16()", '        return "method"',
        "Methods methods := Methods()",
        "int next(int &count)", "    count += 1", "    return 255", "int counter := 0",
    ]
    output("fields.b16", "7")
    output("identity(fields.b16)", "7")
    output("fields.B16", "field")
    output("fields.b02", "2")
    output("methods.b16()", "method")
    lines += ["class ReferenceHolder(local int &input)", "    int &value",
              "    &value = &input", "int referenced := 255",
              "ReferenceHolder holder := ReferenceHolder(&referenced)"]
    output("holder.value.b16", "ff")
    output("next(&counter).b16", "ff")
    output("counter", "1")

    with tempfile.TemporaryDirectory(prefix="quidra-integer-radix-") as directory:
        root = Path(directory)
        source = root / "radix.qui"
        source.write_text("\n".join(lines) + "\n")
        environment = os.environ.copy()
        environment["QUIDRA_CACHE_DIR"] = str(root / "cache")
        executable = root / ("radix.exe" if os.name == "nt" else "radix")
        subprocess.run([compiler, "build", str(source), "-o", str(executable)],
                       env=environment, check=True)
        actual = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
        wanted = "\n".join(expected) + "\n"
        if actual.stdout != wanted:
            import difflib
            sys.stderr.writelines(difflib.unified_diff(
                wanted.splitlines(True), actual.stdout.splitlines(True),
                fromfile="expected", tofile="actual"))
            raise SystemExit(1)
        print(f"integer radix native tests passed ({len(expected)} exact outputs)")


if __name__ == "__main__":
    main()
