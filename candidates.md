# Language design candidates

These are proposed language changes or implementation investigations that are not yet part of the completed specification.

> Candidates are temporary tracking notes. Remove each entry once the underlying issue or design question is resolved.

## Candidate decision bar

Prefer a candidate only when it makes Quidra easier to predict for both humans and machines without creating a second way to express the same meaning. In particular:

- preserve one stable semantic role per spelling;
- keep `auto` honest: bare inference keeps every normal-success alternative but deliberately consumes only `error` through fail-fast; retaining that failure channel must cost the explicit `| error` token;
- keep recoverable failure, ordinary absence, and runtime safety violations distinct;
- make source-order, visibility, ownership/authority, and shape behavior mechanically checkable;
- avoid compatibility shims or syntax that would have to remain forever only because a candidate once existed;
- define enough edge cases that acceptance can be implemented and regression-tested without inventing semantics during implementation.

No active candidates are currently tracked.
