# Package management

Quidra packages are source packages with a `main.qui` entrypoint. Released
packages additionally carry a `quidra.package` manifest and are installed from
immutable release tags.

## Commands

```text
quidra install quidra-dnn
quidra install quidra-dnn@0.2.0
quidra install owner/repository
quidra install https://github.com/owner/repository.git@0.2.0
quidra install ./local-package
quidra remove quidra-dnn
quidra list
quidra package-info quidra-dnn
quidra package-info quidra-dnn --json
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
asset.linux-x86_64 = https://github.com/owner/package/releases/download/v0.1.0/package-linux-x86_64.tar.xz
asset.windows-x86_64 = https://github.com/owner/package/releases/download/v0.1.0/package-windows-x86_64.zip
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
version = 0.1.0
repository = https://github.com/quidra-lang/dnn
description = Neural network layers and optimizers for Quidra
license = MIT
homepage = https://github.com/quidra-lang/dnn
requires.quidra = >=0.1.0 <0.3.0
```

Package-to-package requirements use the same form:

```text
requires.vision = >=0.1.0 <0.3.0
```

Versions use exact `MAJOR.MINOR.PATCH` Semantic Versioning. Requirement terms
are conjunctive and support `=`, `<`, `<=`, `>`, and `>=`. A released
package must declare `requires.quidra`. Its manifest version must exactly match
the release tag.

`description`, `license`, and `homepage` are optional descriptive metadata. `asset.<platform>` entries are optional immutable release payload URLs and are selected only for the running platform (or `asset.default` when present).
They do not participate in dependency resolution or execute any behavior.
`quidra package-info NAME` reads only the already-installed package; `--json`
provides stable machine-readable metadata without network access.

For a dependency other than Quidra, the current installer requires an already
installed compatible package and reports the required range when it is missing
or incompatible. Dependency resolution can become more automatic later without
changing the manifest format.

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
version = "0.2.0"
repository = "https://github.com/quidra-lang/dnn"

[requires]
quidra = ">=0.1.0 <0.3.0"
abi = 1
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
quidra-dnn dnn 0.2.0 <sha256>
quidra-vision vision 0.2.0 <sha256>
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
