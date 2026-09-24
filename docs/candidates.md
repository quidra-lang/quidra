# Language design candidates

These are proposed language changes, not current specification.

## Separate array equality from tensor comparisons

**Status:** Candidate

- Array `==` / `!=` remain whole-value comparisons returning one `bool`; `a != b` is exactly `!(a == b)`.
- Tensor `==`, `!=`, `<`, `<=`, `>`, and `>=` are elementwise and return a same-shaped boolean tensor.
- Whole-tensor predicates should use explicit reductions such as `all` / `any`, rather than changing comparison-operator meaning.
