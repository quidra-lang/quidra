"""Fenced Quidra blocks of the documentation and the context they need.

Shared by tests/documentation_examples.py, which checks every block, and by the
refactoring golden corpus (tests/golden/corpus.py), which compiles them. A
block is checked with `prelude(...) + code` written next to `fixtures(...)`,
with QUIDRA_PACKAGE_PATH pointing at the fixtures' `packages` directory.
"""

from __future__ import annotations

import re
from pathlib import Path

# Documents whose fences tests/documentation_examples.py checks, relative to the
# repository root.
DOCUMENTS = [
    "README.md",
    "docs/spec/language.md",
    "docs/spec/llm-guide.md",
]

FENCE = re.compile(r"\x60\x60\x60quidra\n(.*?)\x60\x60\x60", re.S)


def blocks(text: str) -> list[str]:
    """The fenced Quidra blocks of a Markdown document, in order."""
    return FENCE.findall(text)


POINT = """class Point
    real64 x
    real64 y

"""
CONFIG = """class Config
    int retries = 3
    real64 timeout = 5.0
    string endpoint

"""
GENERIC = """class Box<T>
    T value

T first<T>(T[] values)
    return values[0]

class Convert
    T identity<T>(T value)
        return value

"""
LOOKUP = """int | none | error lookup(int id)
    if id < 0
        return error("invalid id")
    if id == 0
        return none
    return id

"""
def prelude(root: Path, path: Path, code: str) -> str:
    """The declarations a fence relies on from earlier prose, by document."""
    rel = path.relative_to(root).as_posix()
    if rel == "README.md":
        if code.lstrip().startswith("int | none | error doubled("):
            return LOOKUP
        if code.strip().startswith("auto result = lookup(1)") or code.strip().startswith("auto | error result = lookup(1)"):
            return LOOKUP
    if rel == "docs/spec/language.md":
        stripped = code.lstrip()
        if stripped.startswith("Box<int> box"):
            return GENERIC
        if stripped.startswith("Point point") and "Config config" in code:
            return POINT + CONFIG
        if stripped.startswith("Point a") or "Point point\n" in code:
            return POINT
    return ""


def fixtures(directory: Path) -> None:
    (directory / "geometry.qui").write_text(
        "class Point\n    int x\n    int y\n\n    construct(int x, int y)\n        this.x = x\n        this.y = y\n",
        encoding="utf-8",
    )
    (directory / "local.qui").write_text(
        "int local_value()\n    return 1\n", encoding="utf-8"
    )
    (directory / "shared.qui").write_text(
        "int shared_value()\n    return 1\n", encoding="utf-8"
    )
    shared = directory / "shared"
    shared.mkdir()
    (shared / "root.qui").write_text(
        "int root_value()\n    return 1\n", encoding="utf-8"
    )

    packages = directory / "packages"
    plotting = packages / "plotting"
    plotting.mkdir(parents=True)
    (plotting / "main.qui").write_text(
        "const int answer = 42\n\nint placeholder()\n    return 0\n",
        encoding="utf-8",
    )

    nn = packages / "nn"
    nn.mkdir()
    (nn / "main.qui").write_text(
        """class Parameter<T: floating>
    tensor<T> stored
    private autograd.Target gradient_state

    construct(tensor<T> value)
        this.stored = value
        this.gradient_state = autograd.target()

    tensor<T> track()
        return this.stored.track(&this.gradient_state)

class State<T: floating>
    tensor<T> stored

    construct(tensor<T> value)
        this.stored = value

class Adam
    int marker = 0

    Adam | error construct()
        this.marker = 1

    void zero_grad<M>(M &model)
        return

    void step<M>(M &model)
        return
""",
        encoding="utf-8",
    )

    dnn = packages / "dnn"
    dnn.mkdir()
    (dnn / "main.qui").write_text(
        """public import mode = "./mode.qui"

class Parameter<T: floating>
    tensor<T> stored
    private autograd.Target gradient_state

    construct(tensor<T> value)
        this.stored = value
        this.gradient_state = autograd.target()

    tensor<T> track()
        return this.stored.track(&this.gradient_state)

class FC
    int marker = 0

    FC | error construct(int features_in, int features_out)
        this.marker = features_in + features_out

class Adam
    int marker = 0

    Adam | error construct()
        this.marker = 1

    void zero_grad<M>(M &model)
        return

    void step<M>(M &model)
        return
""",
        encoding="utf-8",
    )
    (dnn / "mode.qui").write_text(
        "void fast()\n    return\n\nvoid deterministic()\n    return\n", encoding="utf-8"
    )

    math = packages / "math"
    math.mkdir()
    (math / "main.qui").write_text(
        """const real pi = real(3.141592653589793)

real64 sqrt(real64 value)
    return value ^ 0.5

tensor<T> mean<T: floating>(tensor<T> value)
    return value

tensor<T> matmul<T: numeric>(tensor<T> left, tensor<T> right)
    return left * right
""",
        encoding="utf-8",
    )
