# Development and release workflow

This document is the canonical workflow for developing and releasing Quidra.
It is intentionally operational: an engineer or LLM given an instruction such
as "release Quidra", "follow the release procedure", or
"本体をリリースして" should execute the complete procedure below rather than
only describing it.

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
3. Determine the release version from the actual compatibility/feature delta
   and make every authoritative version field agree, including
   `quidra.manifest.json`, CLI version output, packaging metadata, and release
   documentation.
4. Run the complete test suite on `develop` and require green CI. Fix release
   blockers on `develop`; do not bypass failing checks.
5. Merge `develop` into `main` while preserving both branches' valid history.
   If `main` contains changes not already in `develop`, reconcile them
   explicitly; never replace one branch wholesale merely to make the histories
   match.
6. Fetch/verify the resulting remote `main` HEAD and run or wait for the
   `main` CI checks. The exact commit that passes is the release commit.
7. Create the immutable annotated tag `vX.Y.Z` on that exact `main` commit
   and push the tag. Never tag `develop`, `feature`, or an unverified commit.
8. The tag-triggered release workflow must finish successfully and create the
   GitHub Release and all supported platform artifacts. A release is not
   complete while that workflow is failing or incomplete.
9. Verify that the GitHub Release tag, `main` version metadata, packaged
   `quidra --version`, and published assets all report the same `X.Y.Z`.
10. Bring any release-time `main` changes back into `develop` if necessary.
    Then advance `develop` to the next intended development version and
    commit/push that change. During normal pre-1.0 feature development, use the
    next minor version by default; use the next patch only when intentionally
    continuing maintenance of the current minor line.
11. Leave `main` at the released version. Continue ordinary work on
    `develop`.

The release tag is immutable. Never force-move, delete/recreate, or reuse a
published `vX.Y.Z` tag for different source.

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
core release, and then follows its own release procedure.

The package installer consumes library release tags only. It never installs a
library from `main` or `develop`.

## History invariant

```text
main       = latest released stable source
develop    = next release development
vX.Y.Z     = immutable released source
```

Temporary branches can help integration, but they never change that invariant.
