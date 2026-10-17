#!/usr/bin/env python3
"""Keep domain frameworks out of Quidra Core.

This intentionally scans only live Core implementation/configuration. Historical
benchmark evidence is excluded by construction and must remain immutable.
"""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_PATHS = (
    "src/runtime_image.cpp",
    "src/runtime_video.cpp",
    "tests/video_tests.sh",
    ".github/workflows/video.yml",
)

FORBIDDEN_IMPLEMENTATION_MARKERS = (
    "BuiltinCallable::ImageRead",
    "BuiltinCallable::ImageWrite",
    "BuiltinCallable::VideoOpen",
    "BuiltinCallable::VideoRead",
    "BuiltinCallable::VideoWidth",
    "BuiltinCallable::VideoHeight",
    "BuiltinCallable::VideoFps",
    "BuiltinCallable::VideoFrames",
    "BuiltinCallable::VideoDuration",
    "BuiltinCallable::VideoPosition",
    "BuiltinCallable::VideoSeek",
    "LinearMatmul",
    "LinearDot",
    "TensorMatmul",
    "TensorDot",
    "quidra_tensor_matmul",
    "quidra_tensor_dot",
    "compute_matmul(",
    "compute_dot(",
    "BuiltinCallable::Linear",
    "$std.linear.",
    "quidra_linear_",
    "tensor.convolve",
    "quidra_tensor_convolve",
    "unfold2d",
    "im2col",
    "col2im",
    "BuiltinCallable::StatsSum",
    "BuiltinCallable::StatsMean",
    "BuiltinCallable::StatsMin",
    "BuiltinCallable::StatsMax",
    "StatsReduce",
    "quidra_stats_",
    "$std.stats.",
    "BuiltinCallable::TensorSum",
    "BuiltinCallable::TensorMean",
    "BuiltinCallable::TensorSumLast",
    "BuiltinCallable::TensorMaxLast",
    "BuiltinCallable::TensorMinLast",
    "BuiltinCallable::TensorAbsolute",
    "BuiltinCallable::TensorExponential",
    "BuiltinCallable::TensorLogarithm",
    "BuiltinCallable::TensorSquareRoot",
    "$std.math.",
    "BuiltinCallable::Abs",
    "BuiltinCallable::Sqrt",
    "BuiltinCallable::Min",
    "BuiltinCallable::Max",
    "BuiltinCallable::MathSin",
    "BuiltinCallable::MathCos",
    "BuiltinCallable::MathTan",
    "BuiltinCallable::MathLog",
    "BuiltinCallable::MathExp",
    "BuiltinCallable::MathPow",
    "BuiltinCallable::MathTrunc",
    "BuiltinCallable::MathRound",
    "BuiltinCallable::MathFloor",
    "BuiltinCallable::MathCeil",
    "BuiltinCallable::MathIsFinite",
    "NumericAbs",
    "NumericMinMax",
    "MathUnary",
    "MathIsFinite",
    "MathRoundInt",
    "MathPow",
    "quidra_math_trunc_int",
    "quidra_math_round_int",
    "quidra_math_floor_int",
    "quidra_math_ceil_int",
    "quidra_math_is_finite",
    "quidra_bigint_abs",
    "quidra_bigreal_abs",
    "quidra_bigreal_sqrt",
    "quidra_bigreal_math_unary",
    "quidra_bigreal_round",
    "RealKind::Pi",
    "RealKind::E",
    "RealKind::Sqrt",
    "RealKind::Sin",
    "RealKind::Cos",
    "RealKind::Tan",
    "RealKind::Log",
    "RealKind::Exp",
    "RealKind::Abs",
    "simplify_sqrt",
    "sqrt_rational_decimal",
    "real_math(",
    "round_real(",
    "std::sqrt(",
    "std::sin(",
    "std::cos(",
    "std::tan(",
    "std::log(",
    "std::exp(",
    "\"$pi\"",
    "\"$e\"",
    "is_standard_real_constant",
    "TensorAutogradUnary",
    "quidra_tensor_autograd_unary",
    "compute_abs_backward",
    "ptx_log_validate_kernel",
    "hip_log_validate_source",
    "msl_log_validate_source",
    "AutogradOp::Sum",
    "AutogradOp::Mean",
    "compute_mean_to",
    "compute_mean_backward",
    "compute_reduce(",
    "compute_mean(",
    "sum_last",
    "max_last",
    "min_last",
    "conv1d",
    "conv2d",
    "conv3d",
    "pool2d",
    "softmax",
    "flashattention",
    "layernorm",
    "batchnorm",
    "fusedadam",
    "DnnMode",
    "dnn_mode",
    "set_dnn_mode",
    "open_dnn_nvidia_library",
    "QUIDRA_DNN_NVIDIA_LIBRARY_PATH",
    "QUIDRA_NN_NVIDIA_LIBRARY_PATH",
    "cublas",
    "cudnn",
    "nccl",
    "opencv",
    "libavformat",
    "libavcodec",
    "libswscale",
    "QUIDRA_IMAGE_LIBRARY_PATH",
    "QUIDRA_VIDEO_LIBRARY_PATH",
    ".qnn",
    ".dnn",
)

FORBIDDEN_CMAKE_MARKERS = (
    "find_package(PNG",
    "find_package(JPEG",
    "find_package(TIFF",
    "WEBP_INCLUDE_DIR",
    "WEBP_LIBRARY",
    "AVFORMAT_LIBRARY",
    "AVCODEC_LIBRARY",
    "AVUTIL_LIBRARY",
    "SWSCALE_LIBRARY",
)

FORBIDDEN_WORKFLOW_DEPENDENCY_MARKERS = (
    "libpng",
    "libjpeg",
    "jpeg-turbo",
    "libtiff",
    "libwebp",
    "ffmpeg",
    "libavformat",
    "libavcodec",
    "libavutil",
    "libswscale",
    "cublas",
    "cudnn",
    "nccl",
    "opencv",
)

# Fixed-width complex power is Core arithmetic, not a domain math API.
# Recognize only its existing principal-branch implementation statements.
COMPLEX_POWER_SIGNATURE = (
    'extern "C" void quidra_complex_pow(double re, double im, double exponent_re, double exponent_im,\n'
    '                                   int kind, double* out, unsigned long long line,\n'
    '                                   unsigned long long column) {'
)
COMPLEX_POWER_STATEMENTS = (
    "    const double log_modulus = std::log(std::hypot(re, im));",
    "    const double scale = std::exp(x);",
    "    out[0] = round_part(scale * std::cos(y), part);",
    "    out[1] = round_part(scale * std::sin(y), part);",
)
FINITE_INTERFACE_COMMENT = (
    "// Values that stay inside the package (a -inf mask inside a softmax) are"
)


def implementation_markers(relative: str, text: str) -> tuple[str, ...]:
    if relative == "include/quidra/native_extension.h":
        # This exact full-line comment illustrates package-private intermediates;
        # it neither declares nor implements the operation named in the example.
        lines = text.splitlines(keepends=True)
        for index, line in enumerate(lines):
            if line == FINITE_INTERFACE_COMMENT + "\n":
                lines[index] = "\n"
                break
        text = "".join(lines)
    if relative == "src/runtime_complex.cpp":
        start = text.find("\n" + COMPLEX_POWER_SIGNATURE)
        if start >= 0:
            start += 1
            end = text.find("\n}\n", start)
            if end >= 0:
                body = text[start:end + 1]
                if all(body.count("\n" + statement + "\n") == 1
                       for statement in COMPLEX_POWER_STATEMENTS):
                    for statement in COMPLEX_POWER_STATEMENTS:
                        body = body.replace("\n" + statement + "\n", "\n", 1)
                    text = text[:start] + body + text[end + 1:]
    folded = text.casefold()
    return tuple(marker for marker in FORBIDDEN_IMPLEMENTATION_MARKERS
                 if marker.casefold() in folded)


def check_implementation_marker_scope() -> None:
    power = "\n" + COMPLEX_POWER_SIGNATURE + "\n" + "\n".join(COMPLEX_POWER_STATEMENTS) + "\n}\n"
    cases = (
        ("src/runtime_complex.cpp", power, ()),
        ("src/runtime_complex.cpp", power + "std::sin(extra);\n", ("std::sin(",)),
        ("src/runtime_complex.cpp", power.replace("\n}\n", "\n    std::cos(extra);\n}\n"), ("std::cos(",)),
        ("src/runtime_complex.cpp", power.replace("quidra_complex_pow", "domain_power"),
         ("std::sin(", "std::cos(", "std::log(", "std::exp(")),
        ("src/runtime_complex.cpp", power + "void softmax();\n", ("softmax",)),
        ("include/quidra/native_extension.h", FINITE_INTERFACE_COMMENT + "\n", ()),
        ("include/quidra/native_extension.h", FINITE_INTERFACE_COMMENT + "\nvoid softmax();\n", ("softmax",)),
        ("src/other.cpp", FINITE_INTERFACE_COMMENT + "\n", ("softmax",)),
    )
    for relative, text, expected in cases:
        if implementation_markers(relative, text) != expected:
            raise RuntimeError("Core/domain implementation marker scope regression")


check_implementation_marker_scope()

failures: list[str] = []

for relative in FORBIDDEN_PATHS:
    if (ROOT / relative).exists():
        failures.append(f"forbidden Core-owned domain path exists: {relative}")

implementation_files = [ROOT / "CMakeLists.txt"]
for directory in ("include", "src"):
    base = ROOT / directory
    for path in base.rglob("*"):
        if path.is_file() and path.suffix in {".h", ".hpp", ".cpp", ".inc"}:
            implementation_files.append(path)

for path in implementation_files:
    text = path.read_text(encoding="utf-8")
    relative = path.relative_to(ROOT)
    for marker in implementation_markers(relative.as_posix(), text):
        failures.append(f"{relative}: forbidden domain marker {marker!r}")

cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
for marker in FORBIDDEN_CMAKE_MARKERS:
    if marker in cmake:
        failures.append(f"CMakeLists.txt: forbidden domain dependency marker {marker!r}")

workflow_root = ROOT / ".github" / "workflows"
for path in sorted(workflow_root.glob("*.y*ml")):
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        folded = stripped.casefold()
        for marker in FORBIDDEN_WORKFLOW_DEPENDENCY_MARKERS:
            if marker.casefold() in folded:
                relative = path.relative_to(ROOT)
                failures.append(
                    f"{relative}:{line_number}: forbidden Core-owned domain dependency marker {marker!r}"
                )

if failures:
    print("Core/domain responsibility boundary failed:", file=sys.stderr)
    for failure in failures:
        print(f"  - {failure}", file=sys.stderr)
    raise SystemExit(1)

print("core/domain responsibility boundary: ok")
