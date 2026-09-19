"""Identity-leak scan of the Reference Pack (methodology 10, 2.6, scoped to this unit)."""
import json, os, re, sys
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
text = open(os.path.join(HERE, "reference_pack.md")).read()
real = sorted(json.load(open(os.path.join(HERE, "mapping.json")))["mapping"])
leak_terms = ["python", "cpython", "pypy", "pip", "pep", "guido", "quidra", "rust", "golang",
              "java", "kotlin", "swift", "zig", "typescript", "javascript", "node", "npm",
              "clang", "gcc", "rustc", "javac", "kotlinc", "swiftc", "tsc", "jvm", "cargo",
              "interpreter", "pythonic", "snake_case", "dunder", "docstring", "pypi",
              ".py", ".rs", ".go", ".java", ".kt", ".ts", ".zig", ".cpp", ".swift", ".qui",
              "__main__", "__init__", "stdlib", "import ", "sys.stdout"]
fails = []
for w in real:
    n = len(re.findall(r"(?<![A-Za-z0-9_])" + re.escape(w) + r"(?![A-Za-z0-9_])", text))
    if n:
        fails.append("real keyword %r appears %d time(s)" % (w, n))
low = text.lower()
for t in leak_terms:
    if re.search(r"(?<![a-z0-9_])" + re.escape(t) + (r"(?![a-z0-9_])" if t[0] != "." else ""), low):
        fails.append("identity term %r appears" % t)
print("scanned reference_pack.md: %d chars, %d lines" % (len(text), len(text.splitlines())))
print("real keywords checked: %d   identity terms checked: %d" % (len(real), len(leak_terms)))
if fails:
    print("LEAK SCAN FAILED:")
    for f in fails:
        print("   " + f)
    sys.exit(1)
print("LEAK SCAN PASSED: zero hits.")
