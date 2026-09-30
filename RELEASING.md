# Releasing Quidra

The release procedure lives in
[docs/development.md](docs/development.md).

That document is the single authority for the branch model, the release steps,
the required release checks, the post-release `develop` baseline, and the
ordering between a core release and the first-party libraries. This file exists
only so the procedure is discoverable from the repository root; it deliberately
restates none of it, because two copies of a release procedure drift.

When the user says "release Quidra" or gives another unqualified release
request, execute the coordinated procedure in
[docs/development.md](docs/development.md). A repository-specific request such
as "release Core" uses the repository-specific scope defined by that canonical
document while still respecting synchronized-version prerequisites.

First-party packages have their own equivalent release documentation, in
dependency order:

- Math: [quidra-lang/math](https://github.com/quidra-lang/math) `docs/development.md`
- NN: [quidra-lang/nn](https://github.com/quidra-lang/nn) `docs/development.md`
- Vision: [quidra-lang/vision](https://github.com/quidra-lang/vision) `docs/development.md`
- Video: [quidra-lang/video](https://github.com/quidra-lang/video) `docs/development.md`
- DNN: [quidra-lang/dnn](https://github.com/quidra-lang/dnn) `docs/development.md`

The legacy [quidra-lang/numerics](https://github.com/quidra-lang/numerics)
repository is migration/history only and is not part of coordinated releases.
