# Package management

Quidra packages are source packages with a `main.qui` entrypoint. Released
packages additionally carry a `quidra.package` manifest and are installed from
immutable release tags.

## Commands

```text
quidra install dnn
quidra install dnn@0.2.0
quidra install owner/package
quidra install https://github.com/owner/package.git@0.2.0
quidra install ./local-package
quidra remove dnn
quidra list
quidra package-path
quidra lock program.qui
quidra lock program.qui --check
```

A bare package name such as `dnn` resolves to
`https://github.com/quidra-lang/dnn.git`. An `owner/package` spelling resolves
to the corresponding GitHub repository. An explicit Git URL is used as written.

Remote installation requires `git`. Quidra never installs from `main`,
`develop`, `feature`, or another moving branch. It enumerates exact stable
release tags of the form `vMAJOR.MINOR.PATCH`, checks each tag's package
manifest, and clones only the selected tag. No package install script is
executed.

When no package version is requested, Quidra selects the newest released tag
whose `requires.quidra` range accepts the running compiler. An explicit
version must exist as an exact release tag and must be compatible; otherwise
installation fails instead of silently selecting another version.

## Package manifest

Released packages contain `quidra.package` at the repository root:

```text
name = dnn
version = 0.2.0
repository = https://github.com/quidra-lang/dnn
requires.quidra = >=0.2.0 <0.3.0
```

Package-to-package requirements use the same form:

```text
requires.vision = >=0.3.0 <0.4.0
```

Versions use exact `MAJOR.MINOR.PATCH` Semantic Versioning. Requirement terms
are conjunctive and support `=`, `<`, `<=`, `>`, and `>=`. A released
package must declare `requires.quidra`. Its manifest version must exactly match
the release tag.

For a dependency other than Quidra, the current installer requires an already
installed compatible package and reports the required range when it is missing
or incompatible. Dependency resolution can become more automatic later without
changing the manifest format.

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
~/.quidra/packages/<name>/
```

`quidra list` prints package versions when a manifest is present.
`quidra package-path` prints the default store location.

`quidra lock FILE.qui` writes `quidra.lock` version 2. Every direct or
transitive package reached by the program's import graph is recorded as:

```text
quidra-lock-v2
dnn 0.2.0 <sha256>
vision 0.3.1 <sha256>
```

An unversioned local development package uses `-` in the version column. The
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
