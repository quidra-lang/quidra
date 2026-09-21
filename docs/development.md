# Development and release workflow

This document is the canonical workflow for developing and releasing Quidra.
It is the single authority for the release procedure: `RELEASING.md` at the
repository root points here and deliberately restates none of it.

It is intentionally operational: an engineer or LLM given an instruction such
as "release Quidra", "release the core", "follow the release procedure", or
"本体をリリースして" should execute the complete procedure below rather than
only describing it. Do not stop after writing a plan, opening a pull request,
merging a branch, or creating a tag; continue through release verification and
the post-release `develop` preparation in step 10. Ask only when a real merge
conflict or an ambiguous destructive choice cannot be resolved from the
repository. Always fetch the current remote refs first: never release from a
stale local SHA, never reset away newer remote work, and never move an
existing release tag.

## Permanent branch roles

The normal repository has two permanent branches:

- `main` is the latest published stable release.
- `develop` is the integration branch for the next release.

Temporary work branches may exist, but they are not release sources and are not
installation targets. The current historical `feature` branch is an
exceptional integration branch for the present development cycle. Work on it
must be intentionally merged into `develop` before normal development and
release continue. Do not tag or publish `feature` directly.

Do not delete and recreate `develop` after a release. It remains the
long-lived development branch.

## Versioning

Quidra uses Semantic Versioning. While the language is below 1.0:

- `0.X.0` is appropriate for new language features, standard-library
  capabilities, or material semantic changes.
- `0.X.Y` is appropriate for compatible fixes and maintenance of the same
  minor line.
- `1.0.0` is reserved for the first explicitly stable language contract.

A version number in a development branch is not itself a release. Only an
immutable `vX.Y.Z` tag on `main` is a released version.

Versions are plain `MAJOR.MINOR.PATCH`. A `-dev` or `-rc` suffix is rejected by
the semantic-version parser and by CMake, so a development branch carries an
ordinary version number, not a decorated one.

## Single source of truth for project metadata

`project.toml` at the repository root is the single source of truth for project
metadata. It owns the project name, CLI name, source extension, tagline and
version; the language version; the ABI, IR and package-schema numbers; the
repository URLs; the backend id/display/kind table; the supported operating
systems and targets; and the toolchain minimums. Nothing else in this
repository may declare one of those values independently.

Everything else is derived from it:

- `python3 scripts/sync_metadata.py` regenerates `quidra.manifest.json` and the
  compiler version printed in the README REPL transcript.
- CMake reads the version out of `project.toml` before `project()` and
  generates `include/quidra/project.hpp` at configure time.

Run `python3 scripts/sync_metadata.py` after every edit to `project.toml`.
`python3 scripts/sync_metadata.py --check` writes nothing and exits non-zero
when a derived file disagrees.

`python3 tests/metadata_ssot_tests.py` runs that check and additionally proves
that the files which cannot be generated still agree with `project.toml`: the
CMake and Debian toolchain minimums, the README build-requirement prose, the
backend display names compiled into `src/device_backend.cpp`, the backend ids
documented in the manifest, and the repository the installer script downloads
from. It also fails if the version appears anywhere it would be a second
definition rather than an example. It runs as the `quidra_metadata_ssot` test,
as a no-build `metadata` job on every push and pull request, and again before a
release tag is created.

The README build-requirement list restates the `[dependencies]` minimums in
prose, because build requirements have to be readable without running a script.
That is the only permitted restatement, and the check above verifies it rather
than trusting it.

Benchmark metadata is deliberately outside this. `benchmark/template/` is
copied whole into the measurement sandbox and must stay immutable during a
frozen measurement window, so it owns its own metadata under
`benchmark/template/config/`.

## Normal development

1. Fetch the current remote `develop` HEAD. Never start from a remembered SHA.
2. Implement and test changes on `develop` or on a temporary branch that will
   be merged into `develop`.
3. Keep source, language specification, tests, examples, manifest data, and
   packaging behavior consistent.
4. Do not merge ordinary unfinished development into `main`.
5. Do not create release tags from `develop` or temporary branches.

## Releasing Quidra

When explicitly instructed to release the core repository, perform these steps
in order.

1. Fetch the latest remote `develop` and `main` HEADs immediately before
   release work. Inspect their actual difference. Never overwrite newer remote
   work with an older checkout or remembered SHA.
2. Confirm that all work intended for the release is already present in
   `develop`. A temporary branch such as the exceptional `feature` branch
   must have been merged into `develop` before this point.
3. Determine the release version from the actual compatibility/feature delta.
   Set it in `project.toml`, which is the one place a version is authored, then
   run `python3 scripts/sync_metadata.py` and commit what it regenerates. CLI
   version output, manifest metadata and packaging metadata all follow from
   that single edit; do not set any of them by hand. Confirm that the tag
   `vX.Y.Z` does not already exist before continuing.
4. Run the complete test suite on `develop` and require green CI. Fix release
   blockers on `develop`; do not bypass failing checks.
5. Merge `develop` into `main` while preserving both branches' valid history.
   If `main` contains changes not already in `develop`, reconcile them
   explicitly; never replace one branch wholesale merely to make the histories
   match.
6. Fetch/verify the resulting remote `main` HEAD and run or wait for the
   `main` CI checks. The exact commit that passes is the release commit.
7. The `release` workflow runs on every push to `main`. It re-checks that the
   derived metadata still matches `project.toml`, reads the version from
   `project.toml`, refuses to continue if `vX.Y.Z` already exists, and then
   creates and pushes the immutable annotated tag on that commit itself. Do not
   create or push the tag by hand: the workflow aborts when the tag is already
   there. Nothing is ever tagged from `develop`, `feature`, or an unverified
   commit, because only a push to `main` starts this.
8. That workflow must finish successfully and create the GitHub Release and all
   supported platform artifacts. A release is not complete while it is failing
   or incomplete.
9. Verify that the GitHub Release tag, `main` version metadata, packaged
   `quidra --version`, and published assets all report the same `X.Y.Z`.
10. Bring any release-time `main` changes back into `develop` if necessary.
    Then advance `develop` to the next intended development version by editing
    `project.toml` and running `python3 scripts/sync_metadata.py`, and
    commit/push that change. During normal pre-1.0 feature development, use the
    next minor version by default; use the next patch only when intentionally
    continuing maintenance of the current minor line.
11. Verify the CI of that post-release `develop` commit.
12. Leave `main` at the released version and continue ordinary work on
    `develop`. Report the released tag, the exact `main` SHA, the release and
    CI result, and the new `develop` version.

The release tag is immutable. Never force-move, delete/recreate, or reuse a
published `vX.Y.Z` tag for different source. A published release that needs
correction is superseded by a new Semantic Versioning release, never by
retargeting the existing tag.

## Release checks

A core release is not complete until the applicable checks pass:

- GCC and Clang release builds;
- Linux, macOS, and Windows CI;
- sanitizer and warnings-as-errors builds;
- `ctest --output-on-failure`;
- parser/checker negative tests;
- native runtime smoke tests;
- standard-library and module/import tests;
- package/version/lockfile tests;
- REPL/CLI tests;
- packaging tests;
- `python3 scripts/sync_metadata.py --check`;
- `quidra --version` and `quidra describe` consistency;
- documented examples.

When semantics or performance-sensitive behavior changes, run the relevant
benchmarks as well.

## Core and first-party library release order

Quidra core and first-party libraries do not have to share the same version.

Release order is:

```text
Quidra core
    -> immutable core release tag
    -> update DNN/Vision against that released core
    -> release DNN/Vision independently
```

Do not publish a library whose `requires.quidra` refers only to an unreleased
`develop` or `feature` state. After a core release exists, each library
updates its own version and `requires.quidra` range, tests against the tagged
core release, and then follows its own release procedure, which lives in that
library's `docs/development.md`. Neither library has a `RELEASING.md`.

The package installer consumes library release tags only. It never installs a
library from `main` or `develop`.

## History invariant

```text
main       = latest released stable source
develop    = next release development
vX.Y.Z     = immutable released source
```

Temporary branches can help integration, but they never change that invariant.
