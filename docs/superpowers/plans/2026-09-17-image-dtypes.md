# Image Dtypes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve supported image sample dtypes across `image.read`/`image.write`, support expected-type narrowing without conversion, and update `vision` to preserve dtype while using the exact grayscale coefficients.

**Architecture:** The checker exposes a full union of numeric tensor dtypes plus `error`, but narrows `image.read` when the expected context is `tensor<T> | error`. `try` passes `expected | error` inward. Runtime image decoding returns a tensor plus its dtype code; LLVM builds the correct union tag. Codec writers reject dtype/format combinations that cannot be represented exactly. The external `vision` package uses generic tensor functions where possible and removes the old integer grayscale approximation.

**Tech Stack:** C++20, Quidra compiler/checker/typed IR/LLVM emitter/runtime, libpng, libjpeg, libtiff, libwebp, Quidra source packages, Bash integration tests.

**Spec:** `docs/superpowers/specs/2026-09-17-image-dtypes-design.md`

## Global Constraints

- Core final target branch: `develop`.
- External package final target branch: `main`.
- Re-read target HEAD immediately before merge because other chats may push concurrently.
- Never silently normalize, narrow, clamp, or change signedness to make a file fit an expected dtype.
- `Image` is not a public Quidra type; image values remain CHW `tensor<T>`.
- Dtype mismatch is `error`, not conversion.

---

### Task 1: Checker typing and expected-type narrowing

**Files:**
- Modify: `src/checker.cpp`
- Modify: `quidra.manifest.json`
- Modify: `docs/spec/language.md`
- Modify: `docs/spec/llm-guide.md`
- Test: `tests/compiler_tests.cpp` and/or `tests/stdlib_tests.sh`

**Interfaces:**
- Produces: default `image.read` type containing every numeric tensor dtype plus `error`.
- Produces: narrowed `tensor<T> | error` when that exact expected context is supplied.
- Produces: `try` propagation of `expected | error` to its operand.

- [ ] **Step 1: Write failing checker tests**

Add cases that require these programs to type-check:

```quidra
tensor<uint16> | error loaded = image.read("x.png")
```

and, inside an `error`-returning function:

```quidra
tensor<uint8> load(string path) | error
    tensor<uint8> pixels = try image.read(path)
    return pixels
```

Also assert that assigning `image.read(...)` directly to `tensor<uint8>` without `try`/`error` remains invalid.

- [ ] **Step 2: Verify the new tests fail on the old checker.**

- [ ] **Step 3: Implement the full image result union and expected-type selection in `BuiltinCallable::ImageRead`.**

- [ ] **Step 4: Change `TryExpr` checking so an expected non-error type is passed to the operand as `expected | error`.**

- [ ] **Step 5: Update manifest/language/LLM documentation to describe preservation and narrowing-as-checking.**

- [ ] **Step 6: Run checker/compiler tests and confirm green.**

---

### Task 2: Generic tensor bridge for image runtime

**Files:**
- Modify: `src/runtime.cpp`
- Modify: `src/runtime_image.cpp`
- Modify: `include/quidra/ir.hpp`
- Modify: `src/ir.cpp`
- Modify: `src/llvm_backend.cpp`
- Test: `tests/stdlib_tests.sh`

**Interfaces:**
- Produces runtime image read API that returns a tensor and reports its tensor dtype code.
- Consumes existing tensor dtype codes 1..10 used by the runtime/backend.
- Produces runtime image write API taking an explicit tensor dtype code.

- [ ] **Step 1: Add failing runtime/integration coverage for a 16-bit PNG and multiple TIFF sample formats.**

Generate fixtures in the shell test with Python standard-library binary construction where practical, or a tiny helper compiled against the already-required codec libraries. Assert the decoded Quidra match alternative and sample values.

- [ ] **Step 2: Verify the old runtime fails those tests because it always returns `tensor<uint8>`.**

- [ ] **Step 3: Generalize the tensor CHW bridge in `runtime.cpp` from uint8-only storage to `(dtype, raw bytes, C,H,W)` and generic info/copy helpers.**

- [ ] **Step 4: Change `ImageRead`/`ImageWrite` IR metadata so the LLVM emitter has the result union and concrete write tensor type.**

- [ ] **Step 5: Replace `quidra_image_read_u8`/`quidra_image_write_u8` lowering with dtype-aware runtime calls.**

For unrestricted reads, LLVM switches on the reported dtype code and stores the matching union tag. For narrowed reads, it requests the expected dtype and turns mismatch into the `error` alternative.

- [ ] **Step 6: Run stdlib/native/backend tests and confirm existing uint8 behavior remains green.**

---

### Task 3: Codec dtype preservation

**Files:**
- Modify: `src/runtime_image.cpp`
- Test: `tests/stdlib_tests.sh`

**Interfaces:**
- JPEG/WebP/BMP: exact uint8 only.
- PNG: uint8 or uint16 according to decoded sample precision; packed sub-byte values may widen losslessly to uint8.
- TIFF: exact signed/unsigned/floating sample type for 8/16/32/64 bits when mapped to a Quidra numeric dtype.

- [ ] **Step 1: Implement a raw-byte `Image` representation carrying dtype code, dimensions, channels, and CHW storage.**

- [ ] **Step 2: Preserve 16-bit PNG data as `uint16` rather than reducing it to uint8.**

- [ ] **Step 3: Read TIFF `BITSPERSAMPLE` + `SAMPLEFORMAT` and map exact sample types to Quidra dtype codes. Reject unsupported packed/nonexact combinations.**

- [ ] **Step 4: Make TIFF writing emit matching `BITSPERSAMPLE` and `SAMPLEFORMAT`; make PNG writing accept only exact unsigned 8/16-bit tensors; keep JPEG/WebP/BMP uint8-only.**

- [ ] **Step 5: Assert unsupported write combinations return `error` and do not convert.**

- [ ] **Step 6: Run core CI test commands represented by the repository workflows.**

---

### Task 4: vision generic dtype preservation and grayscale formula

**Files in `quidra-lang/vision`:**
- Modify: `main.qui`
- Modify: `README.md`
- Modify: `examples/process.qui`
- Modify: `tests/integration.sh`

**Interfaces:**
- Geometry/copy/comparison operations become `tensor<T> ...<T>(tensor<T> pixels, ...)` where their arithmetic is type-independent.
- `grayscale` preserves dtype and implements `0.299 R + 0.587 G + 0.114 B` with deterministic integer rounding and direct floating coefficients.

- [ ] **Step 1: Add failing vision tests using at least `tensor<uint16>` and `tensor<float32>` for dtype-preserving geometry and grayscale.**

- [ ] **Step 2: Verify the current uint8-only package rejects those tests.**

- [ ] **Step 3: Generalize crop/resize/flips/rotations/threshold/dilate/erode where the same Quidra source is valid after monomorphization.**

- [ ] **Step 4: Replace the old grayscale `77/150/29 over 256` approximation.**

For integer specializations, compute an overflow-safe value mathematically equivalent to `(299*R + 587*G + 114*B)/1000`, round deterministically, and return the original integer dtype. For floating specializations, compute `0.299*R + 0.587*G + 0.114*B` directly.

- [ ] **Step 5: Update README/examples to show expected-type image reads instead of assuming `auto` is `uint8 | error`.**

- [ ] **Step 6: Run vision integration and example checks against the core feature branch/revision, then against merged core `develop`.**

---

### Task 5: Integration and concurrent-head-safe merge

**Files:** all files modified above.

- [ ] **Step 1: Open PR core feature branch -> `develop` and vision feature branch -> `main`.**

- [ ] **Step 2: Confirm feature-branch/PR CI is green.**

- [ ] **Step 3: Re-fetch `quidra/develop` and `vision/main` immediately before merge.**

- [ ] **Step 4: If either target moved, confirm GitHub reports the PR mergeable against the new head and re-run/re-check CI as needed; never force-update target branches.**

- [ ] **Step 5: Merge core first, then ensure vision CI uses the resulting latest `develop` and merge vision second.**

- [ ] **Step 6: Fetch final target heads and workflow status and report the exact SHAs.**
