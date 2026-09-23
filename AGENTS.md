# Instructions for coding agents

- "Start the benchmark" means: follow `benchmark/README.md` exactly. The only
  thing that starts paid inference is a push to `develop` that changes
  `benchmark/.run-production`; request `benchmark/.run-smoke` first on the same
  commit and wait for `benchmark-smoke` to pass. Never edit either marker for
  any other reason, and never push to `develop` while a `benchmark-production`
  run is in progress.
- `benchmark/master_prompt.md` is the contract every run follows;
  `benchmark/cache/README.md` says what keys the certified cache. Do not edit
  `benchmark/template/config/primary.json` unless re-measuring every cached
  unit is intended.
- Commits and pull requests are attributed to the repository owner only: no
  co-author trailer and no "generated with" line.
