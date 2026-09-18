# Releasing Quidra

This is the authoritative release procedure for humans and coding agents.

## Permanent branch model

Normal development uses two permanent branches:

- \`develop\`: the active development line and the only normal source of a release.
- \`main\`: released, production-ready history.

Temporary feature branches may be used for isolated work. They are merged into
\`develop\`; they are never released directly.

The current \`feature\` branch is a one-time integration exception. It is not
part of the normal release model. Once its intended work has been reconciled
into \`develop\`, future releases must follow \`develop -> main -> tag\`.

Published package/runtime installation must use immutable release tags named
\`vMAJOR.MINOR.PATCH\`, never \`main\`, \`develop\`, or a temporary branch.

## Command intent for coding agents

When the user says **"release Quidra"**, **"release the core"**, or
**"follow the release procedure"**, perform the full procedure below. Do not
stop after writing a plan, opening a PR, merging a branch, or creating a tag.
Continue through release verification and post-release \`develop\` preparation.
Ask only when a real merge conflict or an ambiguous/destructive choice cannot
be resolved from the repository.

Always fetch the current remote refs first. Never release from a stale local SHA,
never reset away newer remote work, and never move an existing release tag.

## Release procedure

1. Fetch the latest remote \`develop\`, \`main\`, tags, and existing releases.
2. Treat \`develop\` as the release candidate. Do not automatically merge a
   temporary feature branch at this stage; feature integration is a separate
   development task that must already have been completed.
3. Compare \`develop\` with the latest release and choose the release version
   according to SemVer:
   - breaking compatibility: major,
   - backward-compatible functionality: minor,
   - backward-compatible fixes/documentation only: patch.
   If \`quidra.manifest.json:compiler_version\` still names an already-published
   release, update it on \`develop\` to the chosen release version before testing.
4. Confirm every version-bearing source agrees with the chosen exact
   \`MAJOR.MINOR.PATCH\` value and that \`v<version>\` does not already exist.
5. Run the complete local/available test suite and all required GitHub Actions
   for the exact \`develop\` commit. Fix failures on \`develop\` and rerun them.
   Do not release a partially failing matrix.
6. Re-fetch \`develop\` and \`main\` immediately before integration. Merge the
   tested \`develop\` content into \`main\` without discarding independent
   changes from either branch. Never force-push \`main\`.
7. Verify the resulting remote \`main\` HEAD contains exactly the tested release
   content and the expected version.
8. Create the immutable tag \`v<version>\` on that exact \`main\` commit and
   push it.
9. The tag-triggered release workflow must complete successfully. Verify the
   GitHub Release exists and that every supported platform artifact expected by
   the workflow was published. A failed or incomplete workflow means the release
   is not complete.
10. Keep \`develop\` permanent. Synchronize the released \`main\` back into
    \`develop\` if needed, then change \`compiler_version\` on \`develop\` to the
    next development baseline. If no later version has been specified, use the
    next patch version as the baseline; it may be raised to a minor/major version
    later when the next release scope is known.
11. Push the post-release \`develop\` version change and verify its CI.
12. Report the released tag, exact \`main\` SHA, release/CI result, and the new
    \`develop\` version.

## First-party library ordering

Quidra core releases come before releases of first-party packages that depend on
new core behavior.

After a new Quidra release exists, DNN and Vision may update their
\`requires.quidra\` range to that released core version, test against the exact
core release tag, and then follow their own \`RELEASING.md\` procedure. Never
publish a library requirement that points only at an unreleased core branch.

## Immutability

Never retarget, delete, or recreate a published release tag to change its
contents. If a published release needs correction, make a new SemVer release.
