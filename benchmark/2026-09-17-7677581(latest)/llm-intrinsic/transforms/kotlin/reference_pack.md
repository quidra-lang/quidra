# Intrinsic Reference Pack — Language H

This is the complete description of Language H. Nothing about it is assumed to be known
to you. Every grammar word of the language is a pseudo-word defined by section P11; use
only the spellings this pack gives you.

---

## P1. Program shape

A complete program is one compilation unit. Procedures are declared directly at the top
level of that unit: no enclosing type is written, and no enclosing type is permitted to
be required.

The entry point is a procedure named `main` that takes no parameters:

```
dunisi main() {
    ...statements...
}
```

`dunisi` introduces a procedure declaration, `main` is the fixed name of the entry point,
`()` is its empty parameter list, and the block holds the body.

Execution begins at the first statement of the entry point body and stops when the body
ends. Reaching the end of the body normally yields exit status 0.

Blocks are delimited by `{` and `}`. A whole task may be written inside the entry point
body; no other procedure is required.

## P2. Lexical rules

- An identifier begins with a letter or `_` and continues with letters, digits or `_`.
  Identifiers you invent are unconstrained beyond that, and are never rewritten by
  anything this pack does.
- Upper and lower letters are distinguished.
- Whole tokens are what matter. Indentation carries no meaning.
- **A statement ends at the end of its line.** No terminator character is written. Two
  statements may share a line only when separated by `;`, which this task never needs.
- A whole-number literal is a run of decimal digits. Its type follows section P4: plain
  digits give the 32-bit type, and a trailing `L` gives the 64-bit type.
- A text literal is written between a pair of `"` marks on one line: `"SUM "`. A backslash
  starts an escape sequence inside a text literal.
- **Do not write the character `$` inside a text literal.** It is reserved there and this
  pack documents no meaning to give it. Join a computed value to text with the `+`
  operator instead (section P5). That is the only route from a number to text this pack
  documents and the only one this task needs.
- A comment runs from `//` to the end of the line, or from `/*` to a matching `*/`.

## P3. Declarations

A named binding is introduced by a binding word, the name, `=`, and an initial value.
**The type is not written: it is taken from the initial value.**

```
rinuso limit = 1000
ruroge total = 0L
```

- `rinuso` introduces an **immutable** binding. Its value is fixed at the point of
  binding; a later assignment to that name is rejected before the program is built.
- `ruroge` introduces a **mutable** binding. A later `name = expression` replaces its
  value, and the replacement must have the same type as the initial value.

Choose `ruroge` whenever a name accumulates across iterations, and `rinuso` whenever a
name is computed once per pass and not replaced.

A procedure is introduced by `dunisi`, its name, a parenthesized parameter list, and a
block — the entry point of section P1 is the only procedure this task needs.

## P4. Types used by the task

| Written as | Meaning |
|---|---|
| `0`, `7`, `1000` | 32-bit signed whole number, roughly -2.1e9 to 2.1e9 |
| `0L`, `7L` | 64-bit signed whole number, roughly -9.2e18 to 9.2e18 |
| `""`, `"SUM "` | text |

Three rules about whole numbers that this task depends on:

1. **Overflow is silent.** Arithmetic on the 32-bit type that leaves the 32-bit range
   wraps around and produces a wrong number. It is not reported, and the program still
   builds and runs. Whenever an intermediate product can exceed the 32-bit range, write
   the initial value with the `L` suffix so the whole computation uses the 64-bit type.
2. **The two whole-number types do not mix.** `a % 2 == 0` is rejected before the program
   is built when `a` holds the 64-bit type, because `0` holds the 32-bit type. Write
   `a % 2 == 0L`. Comparison and equality both require the two sides to hold the same
   type.
3. A binding's type comes from its initial value and never changes afterwards, so
   `ruroge total = 0` is a 32-bit accumulator and `ruroge total = 0L` is a 64-bit one.

Text and a whole number **do** combine, but only through `+`, and only as described by
section P5.

## P5. Expressions and operators

Precedence, tightest first. Operators on one row associate left to right, except `=`,
which associates right to left.

| Level | Operators | Meaning |
|---|---|---|
| 1 | `(` `)` | grouping |
| 2 | `.` and `name(...)` | member access, call |
| 3 | `*` `/` `%` | multiply; whole division truncating toward zero; remainder |
| 4 | `+` `-` | add, subtract; `+` also joins text |
| 5 | `..` | builds the ascending range of whole numbers from its left operand up to and **including** its right operand |
| 6 | `<` `<=` `>` `>=` | ordered comparison, yielding a truth value |
| 7 | `==` `!=` | equality, inequality, yielding a truth value |
| 8 | `=` | assignment; a statement, never part of a larger expression |

Three rules this task depends on:

- `a % b` on non-negative operands yields the non-negative remainder.
- `+` where the **left** operand is text converts the right operand to its decimal text
  form and joins the two: `"SUM " + total` produces `SUM ` followed by the digits of
  `total`. Starting a join from the text literal `""` therefore converts a number to text:
  `"" + total`.
- `0..49` builds the fifty whole numbers 0 through 49; both endpoints are produced.

## P6. Control flow

A conditional branch:

```
pozebe (best < counter) {
    best = counter
}
```

`pozebe` introduces the branch; the condition is written inside round brackets; the block
runs only when the condition holds.

An alternative branch is introduced by `nifise` after the conditional's closing brace:

```
pozebe (counter == 0) {
    label = "" + counter
} nifise {
    label = label + "-" + counter
}
```

A chained alternative is `nifise pozebe (<condition>) { ... }`, repeated as needed.

Bounded iteration over a range:

```
kepafa (i pibemo 0..49) {
    ...statements...
}
```

`kepafa` introduces the iteration. Inside its round brackets, `pibemo` separates the name
bound to each successive element from the sequence that supplies them. The block runs once
per element, with the name bound to that element. The name is introduced by the iteration
itself — no binding word precedes it — it is immutable inside the block, and it is visible
only inside the block.

## P7. Aggregates — not required by this task

## P8. Standard vocabulary

| Spelling | Parameters | Result |
|---|---|---|
| `println(x)` | one text value, or one whole number | none; writes one line |

That is the only standard-vocabulary entry this task needs, and the spelling above is the
current one: write it exactly. Sequence construction, ordering, length, splitting and
comparison facilities are not needed — every value the task asks of you can be accumulated
during one bounded iteration.

## P9. Imports and namespaces

No module or namespace declaration is required, and none may be required. `println` is
reachable from the entry point without any declaration at the top of the unit.

## P10. Output

`println(x)` writes the text form of `x` and then a line terminator. Four calls produce
four terminated lines, including the last. No other line terminator is written by hand,
and nothing is left buffered when the program ends.

## P11. Rules specific to this evaluation

Every grammar word of the language is one of the seven pseudo-words below. No other
spelling of these seven jobs exists, and a program using some other spelling is rejected
before it is built.

| Pseudo-word | Job it does | Position |
|---|---|---|
| `dunisi` | introduces a procedure declaration | before the procedure's name |
| `kepafa` | introduces bounded iteration over a sequence | before the round brackets holding the binding and the sequence |
| `nifise` | introduces the alternative branch of a conditional | after the conditional's closing brace |
| `pibemo` | separates the iteration's bound name from the sequence supplying its elements | between the two, inside the iteration's round brackets |
| `pozebe` | introduces a conditional branch | before the round brackets holding the condition |
| `rinuso` | introduces an immutable named binding | before the name it binds |
| `ruroge` | introduces a mutable named binding | before the name it binds |

Names that are not listed above — `main` and `println` — are written exactly as this pack
spells them. Names you invent yourself are yours to choose and are never rewritten.

## P12. Worked example

A complete program written by Language H: it generates 50 terms of a multiplicative
sequence, accumulates four summaries during one pass, and writes four lines.

```
dunisi main() {
    ruroge state = 7L
    ruroge sum = 0L
    ruroge max = 0L
    ruroge evens = 0
    ruroge joined = ""
    kepafa (i pibemo 0..49) {
        state = (state * 48271) % 2147483647
        rinuso term = state % 1000
        sum = sum + term
        pozebe (term > max) {
            max = term
        }
        pozebe (term % 2 == 0L) {
            evens = evens + 1
        }
        pozebe (i < 5) {
            pozebe (i == 0) {
                joined = joined + term
            } nifise {
                joined = joined + "-" + term
            }
        }
    }
    println("SUM " + sum)
    println("MAX " + max)
    println("EVENS " + evens)
    println("JOINED " + joined)
}
```

Its standard output, exactly:

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

and its exit status is 0.
