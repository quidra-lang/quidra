#!/usr/bin/env python3
"""
Write version.txt for a benchmark run.

Run this at BENCHMARK START, so the evaluated version is recorded before any
measurement — and again at filing time, which appends the "filed at" block.
The distinction matters: a run filed weeks later describes the version it
EVALUATED, not the version in the tree when it was filed.

    python3 write_version.py <repo_root> <run_dir> [--filing]
"""
import json, os, subprocess, sys


def git(repo, *a):
    try:
        return subprocess.run(["git", "-C", repo, *a], capture_output=True,
                              text=True, timeout=60).stdout.strip()
    except Exception:
        return ""


def manifest_version(repo, ref=None):
    try:
        if ref:
            txt = git(repo, "show", f"{ref}:quidra.manifest.json")
        else:
            txt = open(os.path.join(repo, "quidra.manifest.json")).read()
        return json.loads(txt).get("compiler_version", "unknown")
    except Exception:
        return "unknown"


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    repo, run = sys.argv[1], sys.argv[2]
    filing = "--filing" in sys.argv

    # The evaluated commit is encoded in the run directory name:
    #   YYYY-MM-DD-<short>   or   YYYY-MM-DD-<short>(latest)
    # The spec allows the literal "(latest)" suffix on the newest completed run,
    # so it must be stripped before the name is read as a commit.
    run_name = os.path.basename(run.rstrip("/"))
    short = run_name.split("-")[-1]
    if short.endswith("(latest)"):
        short = short[: -len("(latest)")]
    full = git(repo, "rev-parse", short) or short
    ver = manifest_version(repo, short)

    lines = [
        "# Quidra benchmark run — evaluated version", "",
        f"run_id                  {run_name}",
        "", "## Evaluated target (what these results describe)", "",
        f"quidra_version          {ver}",
        f"quidra_commit           {full}",
        f"quidra_commit_short     {short}",
        f"quidra_commit_date      {git(repo, 'show', '-s', '--format=%ci', short)}",
        f"quidra_commit_subject   {git(repo, 'show', '-s', '--format=%s', short)}",
        # NB: `git describe --abbrev=0` returns the newest tag REACHABLE FROM the
        # commit, which is not the release the commit belongs to when the tag was
        # created later on the same line. The manifest's compiler_version at that
        # commit is authoritative; the tag is recorded only as corroboration.
        f"quidra_tag_reachable    {git(repo, 'describe', '--tags', '--abbrev=0', short) or '(none)'}"
        f"   (informational; manifest version above is authoritative)",
    ]

    if filing:
        head = git(repo, "rev-parse", "--short", "HEAD")
        n = git(repo, "rev-list", "--count", f"{short}..HEAD") or "0"
        diff = git(repo, "diff", "--shortstat", f"{short}..HEAD", "--", "src/", "include/")
        cur = manifest_version(repo)
        lines += [
            "", "## Repository state when this run was FILED (not what was evaluated)", "",
            f"filed_at_commit         {head}",
            f"filed_at_version        {cur}",
            f"filed_at_date           {git(repo, 'show', '-s', '--format=%ci', 'HEAD')}",
            f"commits_since_evaluated {n}",
            f"impl_diff_since         {diff.strip() or '(none)'}",
        ]
        if n not in ("0", ""):
            lines += [
                "", "## IMPORTANT", "",
                f"These results describe quidra {ver} at {short}. They do NOT describe {cur}.",
                "Any statement of the form \"Quidra scores X\" in this run means",
                f"\"Quidra {ver} at {short} scores X\".",
            ]

    out = os.path.join(run, "version.txt")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"wrote {out}  (evaluated {ver} @ {short}" +
          (f", filed at {manifest_version(repo)})" if filing else ")"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
