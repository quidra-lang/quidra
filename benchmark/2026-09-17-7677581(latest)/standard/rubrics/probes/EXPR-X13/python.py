import os, sys
os.makedirs("x13pkg", exist_ok=True)
open("x13pkg/__init__.py", "w").close()
open("x13pkg/m.py", "w").write(
    "__all__ = ['pub']\n"
    "def pub():\n    return _priv()\n"
    "def _priv():\n    return 7\n")
sys.path.insert(0, ".")
from x13pkg import m
ns = {}
exec("from x13pkg.m import *", ns)
assert "pub" in ns and "_priv" not in ns
print("X13", m.pub())
