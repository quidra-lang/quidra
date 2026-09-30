# Development and release workflow

This document is the canonical workflow for developing and releasing Quidra.
It is the single authority for the release procedure: `RELEASING.md` at the
repository root points here and deliberately restates none of it.

It is intentionally operational: an engineer or LLM given an instruction to
release Quidra should execute the complete applicable procedure below rather
than only describing it. Do not stop after writing a plan, opening a pull
request, merging a branch, or creating a tag; continue through release
verification and post-release `develop` preparation. Ask only when a real
merge conflict or an ambiguous destructive choice cannot be resolved from the
repositories. Always fetch the current remote refs first: never release from a
stale local SHA, never reset away newer remote work, and never move an
existing release tag.

## Release command scope

An unqualified release request is a coordinated Quidra release, not a Core-only
release. Instructions such as "release Quidra", "release", "リリースして", or
equivalent wording that does not name a repository put all eight official
repositories in scope:

1. Quidra Core (`quidra-lang/quidra`)
2. Quidra Math (`quidra-lang/math`)
3. Quidra NN (`quidra-lang/nn`)
4. Quidra Vision (`quidra-lang/vision`)
5. Quidra Video (`quidra-lang/video`)
6. Quidra DNN (`quidra-lang/dnn`)
7. Quidra Playground (`quidra-lang/playground`)
8. Quidra Website (`quidra-lang/website`)

Do not silently stop after releasing Core when the request is unqualified.
A repository-specific instruction such as "release Core", "release Math",
"release NN", "release DNN", "release Vision", "release Video",
"release Playground", or "release Website" narrows the scope to that repository.

For a coordinated release, use the canonical order shown later in this document:
Core -> Math -> NN -> Vision -> Video -> DNN -> Playground -> Website. Math is
released immediately after Core; the three Layer-3 packages then establish their
released states before the Layer-4 DNN package is released. The fixed sibling
order keeps the procedure deterministic for both humans and automation.

## Permanent branch roles

The normal repository has two permanent branches:

- `main` is the latest published stable release once the repository has published at least one release. Before a newly created first-party repository's first release, `main` may contain bootstrap history only; that state is not a release or installation identity.
- `develop` is the integration branch for the next release.

Routine development is performed directly on `develop`. Do not create new
work branches for ordinary changes, and never use any non-`main`/`develop`
ref as a release or installation identity.

Do not delete and recreate `develop` after a release. It remains the
long-lived development branch.

## Versioning

Quidra uses Semantic Versioning. While the language is below 1.0:

- `0.X.0` is appropriate for new language features, standard-library
  capabilities, or material semantic changes.
- `0.X.Y` is appropriate for compatible fixes and maintenance of the same
  minor line.
- `1.0.0` is reserved for the first explicitly stable language contract.

Core and all first-party computational packages (Math, NN, Vision, Video, and
DNN) use one synchronized release-train version. Their `project.toml` package
versions must exactly equal Core's `[project].version` on `develop`, and every
coordinated release publishes the same immutable `vX.Y.Z` across those six
repositories. Package versions never advance independently.

A version number in a development branch is not itself a release. Only an
immutable `vX.Y.Z` tag on `main` is a released version.

Versions are plain `MAJOR.MINOR.PATCH`. A `-dev` or `-rc` suffix is rejected by
the semantic-version parser and by CMake, so a development branch carries an
ordinary version number, not a decorated one.

## Single source of truth for project metadata

`project.toml` at the repository root is the single source of truth for project
metadata. It owns the project name, CLI name, source extension, tagline and
version; the ABI, IR and package-schema numbers; the
repository URLs; the backend id/display/kind table; the supported operating
systems and targets; and the toolchain minimums. Nothing else in this
repository may declare one of those values independently.

Everything else is derived from it:

- `python3 scripts/sync_metadata.py` regenerates `quidra.manifest.json` and the
  Quidra version printed in the README REPL transcript.
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
2. Implement and test ordinary changes directly on `develop`.
3. Keep source, language specification, tests, examples, manifest data, and
   packaging behavior consistent.
4. Do not merge ordinary unfinished development into `main`.
5. Do not create release tags from `develop` or any other unreleased ref.

## Releasing Quidra

When explicitly instructed to release the core repository, perform these steps
in order.

1. Fetch the latest remote `develop` and `main` HEADs immediately before
   release work. Inspect their actual difference. Never overwrite newer remote
   work with an older checkout or remembered SHA.
2. Confirm that all work intended for the release is already present in
   `develop`.
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
   there. Nothing is ever tagged from `develop` or an unverified commit,
   because only a push to `main` starts this.
8. That workflow must finish successfully and create the GitHub Release and all
   supported platform artifacts. A release is not complete while it is failing
   or incomplete.
9. Verify that the GitHub Release tag, `main` version metadata, packaged
   `quidra --version`, and published assets all report the same `X.Y.Z`.
10. Bring any release-time `main` changes back into `develop` if necessary,
    but do not advance Core's version independently. Core, Math, NN, Vision,
    Video, and DNN must keep the same `MAJOR.MINOR.PATCH` value on `develop`.
11. Verify the CI of that post-release `develop` state.
12. Leave `main` at the released version and continue ordinary work on
    `develop`. Report the released tag, the exact `main` SHA, and the release
    and CI result. A next development version is selected only by the coordinated
    first-party version-advance procedure below.

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

Benchmarks are not part of the release procedure. Do not start, rerun, or
regenerate benchmark measurements merely because a release is being performed,
even when semantics or performance-sensitive behavior changed. Run benchmarks
only when the release instruction explicitly requests benchmark execution.
An already completed formal benchmark result may be published or synchronized
during the release without starting a new benchmark run.

## Coordinated multi-repository release order

Core and all first-party computational packages use one lockstep release
version: Core, Math, NN, Vision, Video, and DNN must all carry the same
`MAJOR.MINOR.PATCH` value. They do not version independently. An unqualified
Quidra release covers Core, Math, NN, Vision, Video, DNN, Playground, and
Website and is performed in this order:

```text
1. Quidra Core
   -> complete Core release and verify the immutable Core tag/assets

2. Quidra Math
   -> require the same-version released Core tag
   -> complete the Math release at the same `vX.Y.Z` and verify its immutable package tag/assets

3. Quidra NN
   -> require the same-version released Core and Math tags
   -> complete the NN release at the same `vX.Y.Z` and verify its immutable package tag/assets

4. Quidra Vision
   -> require the same-version released Core and Math tags
   -> complete the Vision release at the same `vX.Y.Z` and verify its immutable package tag/assets

5. Quidra Video
   -> require the same-version released Core and Math tags
   -> complete the Video release at the same `vX.Y.Z` and verify its immutable package tag/assets

6. Quidra DNN
   -> require the same-version released Core, Math, and NN tags
   -> complete the DNN release at the same `vX.Y.Z` and verify its immutable package tag/assets

7. Quidra Playground
   -> publish Playground only after the preceding release states are stable
   -> fast-forward Playground `main` to its verified `develop`
   -> verify the production deployment uses the intended released Core/package state

8. Quidra Website
   -> update the site to the released Core/Math/NN/Vision/Video/DNN state and release date
   -> re-verify examples, release links, and package references
   -> synchronize an already completed formal benchmark result only when one is intended for publication; never trigger a benchmark run as part of release
   -> require Website `develop` CI to pass, publish the verified state to `main`, and deploy it
   -> verify https://quidra-lang.com serves the intended release and its key links/assets work
```

Do not begin a dependent step merely because an earlier repository was merged
to `main`; wait until that repository's release/deployment workflow has
finished successfully and verify its published state first.

## Coordinated first-party version advance

After the coordinated release is fully published and verified, do not bump only
one repository to the next development version. When a next release-train version
is selected, update Core, Math, NN, Vision, Video, and DNN to that exact same
`MAJOR.MINOR.PATCH` value as one coordinated metadata operation. Regenerate each
derived `quidra.package`/Core metadata artifact, update first-party compatibility
ranges when required, and finish only when all six remote `develop` branches
agree again and the Core first-party compatibility workflow is green. Temporary
cross-repository CI failures during that multi-repository transition are not a
valid final state.

Do not publish Math, NN, Vision, Video, or DNN until every declared first-party
dependency has an immutable tag with the exact same `vX.Y.Z`. NN, Vision, and
Video require same-version Core and Math tags; DNN requires same-version Core,
Math, and NN tags. Every first-party computational package uses the same release
version as Core. Its declared first-party compatibility ranges must admit that
shared version, while the release workflow independently requires and checks out
the exact same-version dependency tags.
This remains true even when a repository has no code changes for that release:
the shared version is a property of the Quidra first-party package set,
not an independent per-repository cadence. The package installer consumes
library release tags only; it never installs a library from `main` or
`develop`.

Playground follows the package releases. Its production `main` resolves Core
`main`, and its WebAssembly frontend and native runner must identify the same
Core commit. Follow the Playground deployment procedure in its `README.md`,
then verify the production page/runner version and package set match.

Website is deliberately last. Follow `quidra-lang/website`'s release-update
procedure: point the site at the released Core version/date, re-verify all
published Quidra examples with that immutable compiler, update
Math/NN/Vision/Video/DNN and benchmark references when applicable, require
Website CI to pass, publish the verified `develop` state to `main`, deploy
the production site, and verify the live `https://quidra-lang.com` content and
important release/download/docs/Playground links. A coordinated release is not
complete until this live-site verification passes.

For a repository-specific release request, run only the named repository's
procedure, while still respecting any prerequisite release dependency described
above.

## History invariant

```text
main       = latest released stable source (or bootstrap history before the first release)
develop    = next release development
vX.Y.Z     = immutable released source
```

Before the first release of a newly created repository, no stable source exists; the bootstrap-only `main` branch must not be treated as one.

