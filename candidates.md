# Language design candidates

These are proposed language changes, not current specification.

> Candidates are temporary tracking notes. Remove each entry once the underlying issue or design question is resolved.

## Separate array equality from tensor comparisons

**Status:** Candidate

- Array `==` / `!=` remain whole-value comparisons returning one `bool`; `a != b` is exactly `!(a == b)`.
- Tensor `==`, `!=`, `<`, `<=`, `>`, and `>=` are elementwise and return a same-shaped boolean tensor.
- Whole-tensor predicates should use explicit reductions such as `all` / `any`, rather than changing comparison-operator meaning.

## Implementation candidates

### Indexed array element class-field initialization false positive

**Status:** Candidate bug

- Reading a class field through an indexed array element, for example `points[1].x`, has been reported to produce `UNINITIALIZED` even when that element's field is initialized.
- Reproduce the array -> index -> class -> field path, fix definite-initialization propagation if confirmed, and add a regression test.

### File I/O and numeric parsing performance

**Status:** Candidate performance investigation

- A benchmark audit reported a file-processing case around 12.49 s and roughly 1.6 GB peak memory.
- Profile allocation and lifetime behavior around line splitting and repeated `int.parse` before changing semantics or adding a specialized fast path.
- Preserve the benchmark workload contract and verify any optimization against the same output and error behavior.

