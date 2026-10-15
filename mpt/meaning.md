# Meaning-counting rules

This document defines how an AI reviewer counts *independent, verifiable meanings*
expressed or guaranteed by source code. It does not count lexical tokens.

## Counting unit

**One meaning = one distinct, independently testable semantic fact or guarantee.**
Count facts conveyed explicitly by the program and facts guaranteed by the
language's documented semantics. A fact can concern a value, an operation,
a relationship, a type, a shape, a state change, or an enforced constraint.

Use these categories consistently across languages:

| Category | Examples of separately countable meanings |
| --- | --- |
| Data | Each specified literal value; input binding; sequence order; sequence length |
| Computation | Addition, reduction, comparison, transformation, filtering, output |
| Control flow | Iteration start, exclusive/inclusive end, step, branch condition, chosen branch |
| Types | Element type, scalar representation, signedness, fixed or arbitrary precision |
| Shape | Rank; each independent extent/binding; relationships between extents |
| State and effects | Independent copy, reference alias, write permission, observable mutation |
| Failure and checking | A distinct enforced bound, range, shape, error, or exhaustiveness guarantee |

An operation and its operands can carry separate information. For example,
`x > 0` specifies a comparison, the input `x`, and the threshold `0`.
A numeric input sequence with 12 specified values contains 12 separate value
facts; its order and length can be additional facts.

## Worked example: shape-constrained tensor

For the type-and-binding fragment:

```text
tensor<real32>{nat N, N} matrix
```

Count **five** meanings:

1. The element type is `real32`.
2. The rank is 2.
3. The first axis size is bound to `N`.
4. The second axis size is constrained to the same `N`.
5. The shape relationship is enforced by type/shape checking.

This is a type-notation illustration; an executable declaration that binds a
previously unknown `N` also needs a suitable initializer under Quidra's rules.
The variable name alone does not add a sixth independent shape fact.

## Avoid double counting

- Count a fact only once in the same scoring unit, even if several tokens state
  it, or a later expression restates an already established constraint.
- Do not automatically award one meaning per token, variable use, punctuation
  mark, or line of code. A new named result can be a new binding fact; mere
  repetition of its name is not another fact.
- If a compiler infers a type or shape already fixed by another declaration,
  count the resulting fact once, not again because it was repeated in syntax.
- Count a safety guarantee only if the language actually enforces it. Distinguish
  static checks, runtime checks, and unverified intentions or comments.
- Do not repeatedly award the same language-wide guarantee for every line.
  Count a particular check again only when it protects an independent operation
  or constraint relevant to the task.
- Do not reward filler text, redundant declarations, extra comments, or dead
  code. Comments may help understanding, but do not independently establish
  executable semantics.

## Comparing programs

1. Fix the task, input domain, required output, and permitted failure behavior.
2. Split each implementation into matching logical sections (not necessarily
   the same number of physical lines).
3. List each independent fact with a short explanation and assign it **1**.
4. Give equivalent algorithms the same base meanings. Add language-specific
   guarantees only when they are real, independently testable differences.
5. Record any disputed fact separately rather than silently granting it.
6. Sum the section meaning counts for each program. Use the same rules for every
   language, whether its semantics are explicit or implicit in the syntax.

Example section ledger:

| Section | Countable facts | Meanings |
| --- | --- | ---: |
| Filter | Source, comparison, threshold, per-element test, selected order | 5 |
| Reduce | Source, summation, scalar result | 3 |
| **Total** | | **8** |

A meaning count is a documented human/AI judgment, not an objective bit count.
The reviewer should expose the ledger so another reviewer can challenge or
reproduce every counted fact.
