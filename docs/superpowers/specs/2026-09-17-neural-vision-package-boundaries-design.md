# Neural and Vision Package Boundaries Design

## Purpose

Quidra 0.2 separates language-owned numeric foundations from first-party,
ordinary source packages. The compiler and standard runtime remain responsible
for tensors, image file I/O, automatic differentiation, safe trainable state,
and the small native kernels needed to implement those foundations. Deep-neural
network abstractions live in `quidra-lang/dnn`; image-processing algorithms live
in `quidra-lang/vision`.

The result has one package path and one import model. Neither `dnn` nor `vision`
receives compiler recognition, namespace injection, a private extension
namespace, or package metadata privileges.

## Repository layout

The three repositories are peers in the working directory:

```text
Quidra/
├── quidra/  # compiler, runtime, standard namespaces, specification
├── dnn/     # ordinary installed Quidra package
└── vision/  # ordinary installed Quidra package
```

`quidra` is developed on the latest remote `develop`. The two package
repositories start from their current default branches; both are empty at the
time of this design.

## Architecture

```text
tensor
├── image     standard tensor-native image file I/O
└── neural    autodiff, gradients, parameters, persistent state

ordinary first-party packages
├── dnn       layers, activations, losses, optimizers
└── vision    image-processing and computer-vision algorithms
```

All image values are tensors. There is no `Image` class, implicit image wrapper,
or image/tensor conversion boundary.

## Standard `image` namespace

The existing built-in `vision` namespace is renamed to `image`. Its public API
is limited to tensor-native file/data I/O:

```quidra
tensor<uint8> | error image.read<uint8>(string path)
void | error image.write(string path, tensor<uint8> value, int quality = 95)
```

The decoded representation remains CHW: grayscale `[1,H,W]`, RGB `[3,H,W]`,
and RGBA `[4,H,W]`. Existing explicit dtype, layout, range, codec, and alpha
handling guarantees remain intact. High-level transforms are not added to this
namespace.

Implementation names, IR names, runtime source names, diagnostics, manifests,
tests, documentation, and examples use `image`; `vision.read` and
`vision.write` cease to resolve.

## Standard `neural` namespace

### Retained foundation

The following remain language-owned:

- `neural<T>` graph values, `track`, and `untrack`;
- define-by-run graph construction and backward traversal;
- `neural.Parameter<T>` with stable opaque identity;
- immutable `neural.Gradients` values;
- generic persistent `neural.State<T>`;
- `neural.grad`, and generic state `save`/`load`;
- safe gradient lookup and explicit Parameter update operations;
- operand-level differentiable numeric primitives that an ordinary package
  cannot implement without losing graph lineage or native performance.

The primitives take their data and state explicitly. They do not own model
structure and do not construct layers or optimizers. Examples include affine
application, convolution over explicit input/weight/bias operands, reductions,
elementwise differentiable math, broadcasting/gathering where needed, and
state-explicit normalization or random masking. Primitive naming describes the
numeric operation, not a framework object.

`Gradients` exposes a typed lookup for a supplied Parameter. Parameter mutation
is possible only through a checked update primitive that preserves identity,
checks dtype/shape, requires write authority, and fails before partial mutation.
These APIs are the boundary used by external optimizers.

### Removed framework surface

The core no longer exports or constructs layer/optimizer classes such as
`Linear`, `Conv2D`, `BatchNorm`, `Dropout`, `SGD`, or `Adam`. It no longer
exports high-level activation or loss names such as `relu`, `sigmoid`, `tanh`,
`softmax`, MSE, cross entropy, or binary cross entropy. The old optimizer-aware
model-walking `neural.step` contract is removed.

Native implementation code may be retained only after it is refactored into a
generic operand-level primitive with no compiler knowledge of a `dnn` package
or a DNN class layout. Old class-specific IR and diagnostics are deleted or
renamed accordingly.

## `dnn` package

`dnn` is a normal declaration-only Quidra source package rooted at `main.qui`.
It is installed through `quidra package install` or found through
`QUIDRA_PACKAGE_PATH`, then imported with:

```quidra
import dnn
```

The package owns:

- layer data structures and creation helpers for `Linear`, `Conv2D`,
  `BatchNorm`, and `Dropout`;
- layer `forward` methods for tensor inference and differentiable execution;
- ReLU, sigmoid, tanh, softmax, and GELU where the available primitives permit
  a correct implementation;
- MSE, cross entropy, and binary cross entropy;
- SGD and Adam state and update APIs;
- initialization policy, execution-mode policy, and DNN-specific validation.

Quidra does not support user-defined constructor bodies or a class/function with
the same name. Constructor-style exported functions therefore create focused
package classes while preserving the desired call form:

```quidra
auto layer = dnn.Linear(input = 2, output = 1, seed = 7)
auto optimizer = dnn.Adam(rate = 0.001)
```

The concrete returned class types remain public for explicit annotations. This
is ordinary source-level name resolution, not compiler special casing.

Optimizers retrieve gradients from `neural.Gradients` and request checked
updates of explicit Parameters. Optimizer state lives in `dnn` objects. No
optimizer name, hyperparameter policy, model layout, or Parameter registry is
owned by core.

The repository contains an MIT license, focused source files imported by
`main.qui`, README/API documentation, runnable examples, and package tests. It
contains no migration wording or references to formerly built-in APIs.

## `vision` package

`vision` is also a normal declaration-only Quidra source package:

```quidra
import vision

tensor<uint8> source = try image.read<uint8>("input.png")
tensor<uint8> resized = vision.resize(source, width = 320, height = 240)
```

Its initial coherent API covers representative high-level operations that can
be expressed safely with tensor indexing and checked arithmetic:

- nearest-neighbor resize;
- right-angle rotation and horizontal/vertical flip;
- explicit crop;
- threshold;
- box blur;
- RGB/RGBA to grayscale conversion.

Every operation consumes and returns tensors directly, validates rank/channel
and argument constraints, and reports invalid requests explicitly. Algorithms
do not perform hidden normalization, BGR conversion, alpha removal, or dtype
conversion.

The repository has the same quality bar as `dnn`: MIT license, `main.qui`,
focused source, README/API documentation, examples, and tests, with only the new
design described.

## Import and package model

The grammar accepts ordinary imports and aliases only:

```quidra
import dnn
import cv = vision
import local = "./local.qui"
```

The `import target += package` production is deleted. `ImportDecl` has no
extension target, the frontend has no extension merge/private namespace path,
and `quidra.package.json` has no `extends` semantics. The package lock continues
to hash all regular package files and ordinary transitive imports.

Old extension syntax is an ordinary parse error. There is no deprecation mode,
legacy diagnostic, alias, compatibility parser, or migration shim.

## Error and safety behavior

The refactor preserves Quidra's existing design laws:

- Tensor and model values retain value semantics; writable storage remains
  explicit with `&`.
- Parameter update APIs require stable writable storage and preserve Parameter
  identity.
- Shape, dtype, initialization, and numeric constraints fail deterministically.
- Invalid graph operations or stale/foreign gradients never cause partial
  updates.
- Typed IR records the resolved primitive rather than rediscovering behavior
  from source spelling.
- Package code receives no hidden mutation or hidden global training state.

## Verification

Core tests cover the renamed image namespace, rejection of old image and import
syntax, retained tensor/autodiff/Parameter behavior, removal of old framework
exports, ordinary package resolution, and each new generic primitive. Existing
unrelated compiler, parser, type-policy, CLI, native, documentation, and
performance suites remain green.

Package tests install `dnn` and `vision` using the real package command, compile
ordinary `import dnn` / `import vision` consumers, and execute representative
training and image-processing examples. The final integration run searches all
three repositories for obsolete extension syntax, built-in `vision` I/O,
class-specific neural framework symbols in core, and legacy compatibility copy.

## Delivery

Changes are committed in reviewable stages. `quidra` is pushed to `develop`;
the empty package repositories receive their initial `main` histories. GitHub
CI is observed after each push. The final report records the exact three commit
SHAs, local test evidence, CI conclusions, what remains in core, what moved to
each package, what was deleted, and any verified limitation.
