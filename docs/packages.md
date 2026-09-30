# Package management

Quidra packages are source packages with a `main.qui` entrypoint. Released
packages additionally carry a `quidra.package` manifest and are installed from
immutable release tags.

## First-party dependency layers

Quidra's official computational packages follow a strict dependency-layer model.
A higher layer may depend on lower layers; a lower layer must not depend on a
higher layer. Sibling repositories in the same layer are independent by
default and must not import one another merely to reuse domain semantics.

Core, Math, NN, Vision, Video, and DNN also use one synchronized release-train
version. On `develop`, every first-party package's `package.version` must equal
Core's `project.version`. A coordinated release publishes the same immutable
`vX.Y.Z` across all six repositories; package versions do not advance
independently. Dependency ranges may remain compatibility ranges, but they must
admit that same shared version, and release validation uses same-version
dependency tags.

```text
Layer 4:  DNN
          |
Layer 3:  NN    Vision    Video
           \      |      /
Layer 2:         Math
                  |
Layer 1:         Core
```

The layers define semantic ownership, not implementation technology:

| Layer | Repository | Ownership |
| --- | --- | --- |
| 1 | Core | Language/compiler, tensor and autograd substrate, devices, execution, package/native-extension mechanisms, and domain-neutral optimization infrastructure. Core provides mechanisms, not package semantics. |
| 2 | Math | Generic mathematical semantics, including numerical operations, exact mathematical semantics, native implementations, autograd rules, and Math-owned compiler optimizations. |
| 3 | NN | Architecture-independent neural-network mechanisms: modules/layers, losses, optimizers, parameter/training semantics, and NN-owned rewrites or fused kernels such as Conv + BatchNorm + ReLU. |
| 3 | Vision | Image and computer-vision semantics such as resize, crop, color/image transforms, image codecs, and Vision-owned kernels or optimizations. |
| 3 | Video | Video/temporal media semantics such as frame/video transforms, sampling, codecs, and Video-owned kernels or optimizations. |
| 4 | DNN | Concrete deep-neural-network model compositions such as ResNet, ViT, UNet, YOLO, and multimodal/video models. DNN may compose NN, Vision, and Video without forcing those Layer-3 packages to depend on one another. |

The allowed dependency direction for these packages is:

```text
Layer 4 packages -> Layer 3 packages / Math / Core as needed
Layer 3 packages -> Math / Core
Math             -> Core
Core             -> no higher first-party layer
```

This is an upper bound, not a declaration that every allowed edge exists.
The current DNN manifest depends on NN and Math (with Core as the substrate);
it does not depend on Vision or Video.

Layer-3 sibling dependencies are forbidden by default. For example, NN does
not import Vision, Vision does not import NN, and Video does not import either
sibling just to reuse domain semantics. Cross-domain composition belongs in
Layer 4 or another explicitly higher-level package.

Optimization follows the same ownership boundary. Core may provide typed IR,
pattern/rewrite registration, graph analysis, dispatch, native-kernel hooks,
and other generic compiler mechanisms. The package that owns the semantics
owns the actual rewrite rules, numerical validity conditions, native/CUDA/Metal
implementation, and backend policy. Thus Conv + BatchNorm + ReLU fusion belongs
to NN, while a model-specific ResNet rewrite belongs to DNN and mathematical
kernels or rewrites belong to Math. Being performance-critical,
compiler-visible, differentiable, or GPU-backed is never by itself a reason to
move semantics into Core.

## Commands

```text
quidra install quidra-dnn
quidra install quidra-dnn@0.5.0
quidra install owner/repository
quidra install https://github.com/owner/repository.git@1.2.3
quidra install ./local-package
quidra remove quidra-dnn
quidra list
quidra package-info quidra-dnn
quidra package-info quidra-dnn --json
quidra package sync ./my-package
quidra package sync ./my-package --check
quidra package validate ./my-package
quidra package-path
quidra lock program.qui
quidra lock program.qui --check
```

Official first-party distributions use the `quidra-` namespace. A bare name
such as `quidra-dnn` resolves to `https://github.com/quidra-lang/dnn.git`;
the suffix is the import/repository identifier. The old short spelling `dnn`
remains accepted as a compatibility alias. Third-party packages use
`owner/repository` or an explicit Git URL until a registry is introduced.

Remote installation requires `git`. Quidra never installs from `main`,
`develop`, `feature`, or another moving branch. It enumerates exact stable
release tags of the form `vMAJOR.MINOR.PATCH`, checks each tag's package
manifest, and clones only the selected tag. No package install script is
executed.

A released package may attach a platform asset to the immutable source release:

```text
asset.linux-x86_64 = https://github.com/owner/package/releases/download/v1.2.3/package-linux-x86_64.tar.xz
asset.windows-x86_64 = https://github.com/owner/package/releases/download/v1.2.3/package-windows-x86_64.zip
```

The installer downloads only HTTPS assets, inspects the archive before
extraction, rejects absolute/traversing paths, links, and attempts to replace
`main.qui`, `quidra.package`, `.git`, or `quidra.lock`, then merges the
regular files into the package tree before publishing the install atomically.
`asset.default` is an optional fallback. Asset-backed installs additionally
require `curl` and `tar`; official Linux packages declare those runtime
dependencies.

When no package version is requested, Quidra selects the newest released tag
whose `requires.quidra` range accepts the running compiler. An explicit
version must exist as an exact release tag and must be compatible; otherwise
installation fails instead of silently selecting another version.

## Package manifest

Released packages contain `quidra.package` at the repository root:

```text
name = dnn
version = 0.5.0
repository = https://github.com/quidra-lang/dnn
description = Deep neural network models for Quidra
license = MIT
homepage = https://github.com/quidra-lang/dnn
requires.quidra = >=0.5.0 <0.6.0
requires.math = >=0.5.0 <0.6.0
requires.nn = >=0.5.0 <0.6.0
```

Package-to-package requirements use the same `requires.<import>` form. The
DNN example above follows the current layer graph: DNN depends on Math and NN,
not on sibling Layer-3 packages such as Vision or Video.

Versions use exact `MAJOR.MINOR.PATCH` Semantic Versioning. Requirement terms
are conjunctive and support `=`, `<`, `<=`, `>`, and `>=`. A released
package must declare `requires.quidra`. Its manifest version must exactly match
the release tag.

Official first-party computational packages are versioned in lockstep with
Core. Math, NN, Vision, Video, and DNN must carry the exact same
`MAJOR.MINOR.PATCH` value as the corresponding Core release, including when a
repository has no code change in that release. Their release workflows check
out first-party dependencies at that same version tag and reject divergence.

`description`, `license`, and `homepage` are optional descriptive metadata. `asset.<platform>` entries are optional immutable release payload URLs and are selected only for the running platform (or `asset.default` when present).
They do not participate in dependency resolution or execute any behavior.
`quidra package-info NAME` reads only the already-installed package; `--json`
provides stable machine-readable metadata without network access.

For a dependency other than Quidra, the current installer requires an already
installed compatible package and reports the required range when it is missing
or incompatible. Dependency resolution can become more automatic later without
changing the manifest format.

### Package-owned native sources

A package may keep native implementation beside its Quidra source. The manifest
uses named `native.source.*` entries and optional `native.pkg.*` pkg-config
dependencies. Sources may be common to every host or scoped to one canonical
host platform:

```toml
[native.source]
cpu = "native/cpu.cpp"

[native.source.macos-arm64]
metal = "native/metal.mm"

[native.source.linux-x86_64]
cuda = "native/kernels.cu"

[native.pkg]
codec = "libexample"
```

The generated `quidra.package` flattens platform sources as
`native.source.<platform>.<name> = path`. Only sources matching the current
host platform are compiled; common `native.source.*` inputs are always used.
Core treats these as build inputs, not as domain semantics. C, C++, and
`.s`/`.S` assembly sources use the host Clang toolchain; Objective-C++ `.mm`
is accepted on Apple hosts, and `.cu` sources use NVIDIA `nvcc`. `QUIDRA_NVCC` can select the CUDA compiler explicitly.
The CUDA toolkit root is resolved from `QUIDRA_CUDA_HOME`, `CUDA_HOME`,
`CUDA_PATH`, or the selected `nvcc` location so the package-owned object can
link the CUDA runtime. AOT and REPL/JIT use the same package source declarations.

This mechanism is intentionally domain-neutral: NN, DNN, Vision, Video,
scientific, and other packages own their kernels and third-party backend policy. Core only
compiles, links, caches, and loads the declared native inputs.

Package kernels that need to share Core's same-device asynchronous ordering may
query the borrowed backend-native queue/stream with
`qcore_device_queue_handle(device)`. Metal exposes Core's
`MTLCommandQueue`; CUDA/HIP use their backend default stream, represented by
native handle 0. The execution handle is mechanism only: packages still own
operation semantics, kernels, and backend policy.

### Package `project.toml`

`quidra.package` recognizes a closed set of keys, and an unknown key is an error
rather than a warning, so a package that added one could not be read by an
already-released compiler. Metadata beyond that set therefore lives in an
optional `project.toml` next to it, which older compilers simply never open:

```toml
[package]
name = "quidra-dnn"
import = "dnn"
display_name = "Quidra DNN"
version = "0.5.0"
repository = "https://github.com/quidra-lang/dnn"

[requires]
quidra = ">=0.5.0 <0.6.0"
abi = 1

[compiler.extension]
graph = "compiler/graph.toml"
```

It separates four identities that `quidra.package`'s single `name` cannot carry
at once:

| Field | Meaning |
| --- | --- |
| `name` | distribution/package-manager identity, e.g. `quidra-dnn` |
| `import` | the identifier `import NAME` binds, and the installed directory name |
| `display_name` | human-facing title, e.g. `Quidra DNN` |
| `repository` | where releases come from |

The distribution name is used by package-manager commands such as install,
list, remove and package-info. The import name is the constrained one: it must
be a valid Quidra identifier, so it cannot contain a hyphen, and it equals
`quidra.package`'s legacy `name` plus the installed directory name. Keeping
that legacy field as the import identifier lets older compilers continue to
read new package releases.

`requires.abi` cannot appear in `quidra.package`, because every `requires.<dep>`
value there is parsed as a version range. It lives here instead, and it is an
exact match against the compiler's ABI version rather than a range: the ABI
number changes only when the contract itself changes, so a package either speaks
it or does not.

When `project.toml` is present the compiler reads it alongside `quidra.package`
and rejects the package if the two disagree on the import name or the version,
which is what makes `quidra.package` safe to generate from it.

`quidra package sync [DIR]` is the canonical generator for the compatibility
manifest. It derives `quidra.package` from `project.toml`, including package
identity, release asset URLs, native sources/pkg-config dependencies and package
requirements. `--check` verifies the generated file without writing;
`quidra package validate [DIR]` is the equivalent validation-only command.
Package repositories therefore do not need a second TOML parser merely to keep
these two metadata files synchronized.

The optional `[compiler.extension]` table maps a package-owned logical extension
name to a package-relative declarative descriptor. Core validates that each
descriptor stays inside the package and exists as a regular file. A descriptor
starts with `[extension]`, `version = 1`, and the generic
`phase = "tensor-region"`. Core snapshots descriptor contents during import,
carries them through specialization/checking into typed IR, and routes candidate
tensor regions by the source package that owns each function. Import aliases do
not affect ownership. Regions also expose whether they reach backward and
whether they may require higher-order backward, allowing package policy to
conservatively avoid a first-order-only implementation. Descriptors are data,
not in-process native compiler plugins; domain patterns and backend policy remain
package-owned. Core validates only the descriptor structure required by this
generic substrate: every `[operation.*]` table has a non-empty function target,
and every `[fusion.*]` sequence references declared operation IDs without empty
entries. A fusion may optionally name `replacement = "operation_id"`; that id
must name another declared operation in the same extension. Core may apply such
a replacement only through a domain-neutral call contract. The initial rewrite
requires a same-basic-block pure tensor chain, unary tail operations, no
observable use of eliminated intermediate SSA values, a replacement parameter
contract identical to the first operation, and a result type identical to the
last operation. If any condition is not proven, the fusion remains analysis
metadata only. Domain meaning, the replacement implementation, backend policy,
and numerical validity stay with the owning package.

Local directory installation remains available for package development:

```text
quidra install ./my-package
quidra install ./legacy-package --name legacy_package
```

A local package with a manifest is compatibility-checked. A legacy local package
without a manifest can still be installed explicitly and is treated as
unversioned. Existing packages are replaced by local installation only with
`--force`. Development checkouts can also be exposed through
`QUIDRA_PACKAGE_PATH` without copying them into the package store.

## Installed store and lockfile

The default installed source store is:

```text
~/.quidra/packages/<import-name>/
```

`quidra list` prints package versions when a manifest is present.
`quidra package-path` prints the default store location.

`quidra lock FILE.qui` writes `quidra.lock` version 3. Every direct or
transitive package reached by the program's import graph records both identities:

```text
quidra-lock-v3
quidra-dnn dnn 0.5.0 <sha256>
quidra-vision vision 0.5.0 <sha256>
```

The columns are distribution name, import name, version and content hash.
An unversioned local development package uses `-` in the version column.
Version-1 and version-2 lockfiles remain readable for compatibility; rewriting
the lockfile upgrades it to version 3. The
SHA-256 covers the package's regular-file tree while deliberately excluding
`.git` metadata. Compilation checks both the recorded version and content
hash, so a lockfile identifies the exact package contents rather than merely a
compatible release.

`quidra lock FILE.qui --check` is the non-writing CI form.

## Release identity

A package release is identified by all of the following agreeing:

- `quidra.package` version `X.Y.Z`;
- immutable Git tag `vX.Y.Z`;
- the source tree referenced by that tag;
- the package's declared Quidra and package dependency ranges.

Branches are development locations, never install identities.
