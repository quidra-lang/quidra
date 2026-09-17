# Lossless Image Dtype Design

## Goal

Make `image.read` preserve image sample type instead of silently reducing every image to `uint8`, while keeping Quidra statically typed and preserving the tensor-only image model.

## Public type model

The image pixel dtypes are every numeric tensor element type currently supported by Quidra:

- `int8`, `int16`, `int32`, `int64` (`int`)
- `uint8`, `uint16`, `uint32`, `uint64`
- `float32`, `float64` (`float`)

Without an expected type, `image.read(path)` has the static type:

```text
tensor<int8> | tensor<int16> | tensor<int32> | tensor<int> |
tensor<uint8> | tensor<uint16> | tensor<uint32> | tensor<uint64> |
tensor<float32> | tensor<float> | error
```

No `Image` wrapper type is introduced. Successful values remain rank-3 CHW tensors.

## Expected-type narrowing

`image.read` may use an explicit expected `tensor<T> | error` context to request one supported dtype. It never converts a different source dtype to satisfy that request; a mismatch becomes `error`.

`try` propagates its non-error expected type into the wrapped expression as `expected | error`. Therefore this is valid inside a function whose return type accepts `error`:

```quidra
tensor<uint8> pixels = try image.read("input.png")
```

If the decoded image is not `uint8`, the read operation returns `error`. The spelling does not imply a cast.

At top level the corresponding explicit form is:

```quidra
tensor<uint8> | error loaded = image.read("input.png")
```

Using `auto` intentionally keeps the complete dtype union and therefore requires exhaustive handling when matched.

## Codec rules

Image I/O must not normalize, clamp, sign-convert, or narrow pixel samples implicitly.

- JPEG, WebP, and the currently supported BMP layouts decode as `uint8`.
- PNG preserves 16-bit unsigned samples as `uint16`; 8-bit samples decode as `uint8`. Lower packed sample depths may be losslessly widened to `uint8` without normalization.
- TIFF maps unsigned, signed, and IEEE floating-point samples to the matching Quidra dtype when the stored sample width is 8, 16, 32, or 64 bits and Quidra has an exact corresponding type.
- A format/sample combination that cannot be represented without loss returns `error` instead of converting silently.

`image.write` accepts all image tensor dtypes but succeeds only when the selected file format can represent the tensor dtype exactly. Unsupported format/dtype combinations return `error`.

## Runtime representation

The image runtime passes raw CHW bytes plus the existing tensor dtype code. `image.read` reports both a tensor and its dtype code to generated code. The LLVM backend constructs the correct union alternative from that dtype. When a single expected dtype was selected statically, runtime decoding validates that exact dtype and reports a mismatch as an image error.

## vision package

The first-party `vision` package remains ordinary Quidra source and continues to operate directly on tensors.

Geometry operations and operations that only copy/compare samples should preserve the input tensor dtype where Quidra generic code can express the operation safely.

`grayscale` uses the documented luminance equation exactly at the algorithm level:

```text
Y = 0.299 R + 0.587 G + 0.114 B
```

It must not use the previous `77/150/29 over 256` approximation. Floating inputs use the coefficients directly. Integer inputs use an overflow-safe calculation equivalent to the exact decimal coefficients and a documented deterministic rounding rule before returning the same integer dtype. No hidden normalization is performed.

## Safety and compatibility

- Existing `uint8` images continue to round-trip unchanged.
- Dtype mismatch is an ordinary `error`, never an implicit conversion.
- `auto image.read(...)` becomes a wider union by design, so examples/tests that know the expected dtype should use an explicit expected union or `try` in an error-returning function.
- Documentation and LLM guidance must state that image dtype is preserved and that expected-type narrowing checks rather than converts.
