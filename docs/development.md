# Development and release workflow

This document is the canonical workflow for developing and releasing Quidra.

## Branch roles

- `main` is always the latest released, stable version.
- `develop` is the integration branch for normal development. New language features, compiler work, runtime changes, documentation changes, and tests land here first.
- `release/vX.Y.Z` is a temporary stabilization branch created from `develop` only after the scope for a release is frozen.
- `hotfix/vX.Y.Z` is used only when an already released version needs an urgent fix based on `main`.

Do not develop new features directly on `main`.

A released version is identified canonically by its immutable Git tag and GitHub Release, not by a long-lived version branch. Release branches may be deleted after the release is complete.

## Versioning

Quidra uses Semantic Versioning.

While the project is below 1.0:

- `0.X.0` is used for a release that adds language features, syntax, standard-library capabilities, or materially changes semantics.
- `0.X.Y` is used for backwards-compatible fixes and maintenance of the corresponding minor line.
- `1.0.0` is reserved for the first explicitly stable language contract.

Examples:

- bug fixes only after v0.1.0 -> v0.1.1
- adding `elif`, new string operations, or new array operations after v0.1.0 -> v0.2.0

Do not reuse an existing released version number for different source.

## Normal development

Normal work happens on `develop`.

1. Start from the latest `develop`.
2. Implement the change completely across the relevant layers: grammar/parser, checker, IR, backend/runtime, tests, examples, documentation, manifest, and packaging when applicable.
3. Add regression tests for both accepted and rejected behavior.
4. Run the full test suite.
5. Keep `develop` buildable. A temporary failing commit is acceptable during active work, but do not treat a change as complete until CI is green.

The working version remains the last released version until a release is prepared. Development commits do not create tags or GitHub Releases.

## Preparing a release

When the next version is ready to stabilize:

1. Decide the version number from the actual compatibility and feature delta.
2. Create `release/vX.Y.Z` from the tested `develop` commit.
3. Freeze features on that branch. Only release blockers, tests, documentation, packaging fixes, and version metadata changes belong there.
4. Update every authoritative version field and generated/package metadata that depends on it.
5. Verify that README, language/spec documents, the manifest, examples, CLI help/describe output, and release notes describe the version that is actually being shipped.
6. Run all release checks below.
7. Merge the release branch into `main`.
8. Create an annotated tag `vX.Y.Z` on that exact `main` commit.
9. Create the GitHub Release from that tag and attach the release artifacts.
10. Merge any stabilization-only fixes/version changes back into `develop` if they are not already present.
11. The release branch may then be deleted. The tag and GitHub Release are the permanent historical record.

Never tag a commit that has not passed the release checks.

## Release checks

A release is not complete until all of the following pass:

- GCC release build
- Clang release build
- macOS build/tests/native smoke
- Windows build/tests/native smoke
- sanitizer build/tests, including the runtime archive used by generated native programs
- warnings-as-errors builds on GCC, Clang, macOS, and MSVC
- `ctest --output-on-failure`
- native smoke tests
- parser/checker negative tests
- standard-library tests
- import/module tests
- REPL/CLI tests
- packaging tests for supported artifacts
- version output verification
- `quidra describe` / manifest consistency
- examples execute with their documented output

When language semantics or performance-sensitive runtime behavior changes, also run the relevant benchmarks and record unexpected regressions before release.

## Hotfixes

For an urgent fix to the currently released version:

1. Create `hotfix/vX.Y.Z` from `main`.
2. Make only the minimal compatible fix.
3. Run the same release checks.
4. Merge into `main`.
5. Tag and publish the patch release.
6. Merge the hotfix into `develop`.
7. Delete the hotfix branch after the tag and release exist.

## History rules

- `main` must never point to unreleased experimental work.
- Released tags are immutable.
- Do not force-move or recreate a published release tag.
- Do not silently rewrite a published release's source while retaining the same version.
- `develop` is allowed to move ahead of `main`.
- Release/hotfix branches exist for stabilization, not as permanent version archives.

This keeps one simple interpretation:

```text
main       = what users can download as the latest stable release
develop    = what Quidra is becoming
vX.Y.Z tag = exactly what version X.Y.Z was
```
