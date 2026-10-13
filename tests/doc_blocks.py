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


POINT = """class Point()
    real64 x
    real64 y

"""
POINT_GENERATION = """class Point point(real64 x, real64 y, local real64 scale = 2.0)
    x *= scale
    y *= scale
    real64 length_squared = x * x + y * y

    real64 sum()
        return x + y

    Point copy()
        return point

"""
CONFIG = """class Config()
    int retries = 3
    real64 timeout = 5.0
    string endpoint

"""
GENERIC = """class Box<T>()
    T value

T first<T>(T[] values)
    return values[0]

class Convert()
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
        if stripped.startswith("auto point = Point(") and "class Point" not in code:
            return "class Point(real64 x, real64 y)\n\n"
        if "class Point" not in code and (stripped.startswith("Point pending\n") or "\nPoint point = Point(" in code):
            return POINT_GENERATION
        if stripped.startswith("Box<int> box"):
            return GENERIC
        if stripped.startswith("Point point") and "Config config" in code:
            return POINT + CONFIG
        if stripped.startswith("Point partial"):
            return POINT.replace("    real64 y\n", "    real64 y\n    real64 length_squared\n")
        if stripped.startswith("Point a") or "Point point\n" in code:
            return POINT
    return ""


def fixtures(directory: Path) -> None:
    (directory / "geometry.qui").write_text(
        "class Point this(local int x_initial, local int y_initial)\n    int x\n    int y\n\n\n    this.x = x_initial\n    this.y = y_initial\n",
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
        """class Parameter<T: floating> this(local tensor<T> value)
    tensor<T> stored
    private autograd.Target gradient_state


    this.stored = value
    this.gradient_state = autograd.target()

    tensor<T> .track()
        return this.stored.track(&this.gradient_state)

    void .clear_grad()
        .this.gradient_state.clear_grad()

class State<T: floating> this(local tensor<T> value)
    tensor<T> stored


    this.stored = value

reflect.Each<Parameter<T>> parameters<T: floating, M>(M &model)
    return reflect.each<Parameter<T>>(&model)

void clear_grad<M>(M &model)
    for Parameter<real16> &parameter = &parameters(&model)[]
        .parameter.clear_grad()
    for Parameter<real16b> &parameter = &parameters(&model)[]
        .parameter.clear_grad()
    for Parameter<real32> &parameter = &parameters(&model)[]
        .parameter.clear_grad()
    for Parameter<real64> &parameter = &parameters(&model)[]
        .parameter.clear_grad()

class Adam<M> this(local M &model_initial, local real64 learning_rate = 0.001) | error
    private M &model
    private int marker = 0


    &this.model = &model_initial
    this.marker = 1

    void | error .update()
        this.marker += 1
        clear_grad(&this.model)
        return
""",
        encoding="utf-8",
    )

    dnn = packages / "dnn"
    dnn.mkdir()
    (dnn / "main.qui").write_text(
        """public import mode = "./mode.qui"

class FC this(local int features_in, local int features_out) | error
    int marker = 0


    this.marker = features_in + features_out

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
