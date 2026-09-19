# Intrinsic Reference Pack — Language F

This is the complete description of Language F. Nothing about it is assumed to be known
to you. Every grammar word of the language is a pseudo-word defined in section P11; use
only the spellings this pack gives you.

---

## P1. Program shape

A complete program is one compilation unit. It holds exactly one named enclosing type,
and all code lives inside that type.

The enclosing type is written as three tokens and a block:

```
tenuro balinu Main {
    ...members...
}
```

`tenuro` is the visibility marker, `balinu` introduces the named enclosing type, and
`Main` is the name of the type. Name the enclosing type `Main`.

The entry point is a member of that type, written:

```
tenuro zobolu sadora main(String[] args) {
    ...statements...
}
```

`zobolu` marks the member as belonging to the type itself rather than to an instance;
`sadora` is the result-less designation, meaning this member yields no value; `main` is
the fixed name of the entry point; `String[] args` is its fixed parameter list, which
you write even though the task supplies no arguments.

Execution begins at the first statement of the entry point body and stops when the body
ends. Reaching the end of the body normally yields exit status 0.

Statements end with `;`. Blocks are delimited by `{` and `}`. A whole task may be
written inside the entry point body; no other member is required.

## P2. Lexical rules

- An identifier begins with a letter, `_` or `$`, and continues with letters, digits,
  `_` or `$`. Identifiers you invent are unconstrained beyond that, and are never
  rewritten by anything in this pack.
- Upper and lower letters are distinguished.
- Whole tokens are what matter. Whitespace and line breaks separate tokens and carry no
  other meaning; indentation carries no meaning.
- An integer literal is a run of decimal digits, optionally preceded by `-`. A literal
  whose value does not fit the 32-bit type carries the suffix `L`.
- A text literal is written between a pair of `"` marks on one line: `"SUM "`. A backslash
  starts an escape sequence inside a text literal.
- A comment runs from `//` to the end of the line, or from `/*` to `*/`.
- Embedding a value inside a text literal: this language has no such construct. Join a
  value to text with the `+` operator instead (section P5).

## P3. Declarations

A named value is introduced by writing its type, then the name, then `=` and an initial
value:

```
metolu total = 0;
nasisa counter = 0;
String label = "";
```

Names introduced this way are mutable: a later `name = expression;` replaces the value.
The type is written explicitly at the declaration and is never inferred.

A member is introduced by its markers, its result designation, its name, a parenthesized
parameter list, and a block — the entry point in section P1 is the only member this task
needs.

## P4. Types used by the task

| Spelling | Meaning |
|---|---|
| `nasisa` | 32-bit signed integer, roughly -2.1e9 to 2.1e9 |
| `metolu` | 64-bit signed integer, roughly -9.2e18 to 9.2e18 |
| `String` | text |
| `sadora` | the result-less designation, used where a member yields no value |

An expression combining a `nasisa` with a `metolu` yields a `metolu`, and a `nasisa`
value may be assigned into a `metolu` name with no conversion written by you. Arithmetic
is exact within the stated range and is not arbitrary precision: choose `metolu` whenever
an intermediate product can exceed the 32-bit range.

## P5. Expressions and operators

Precedence, tightest first. Operators on one row associate left to right, except `=`,
which associates right to left.

| Level | Operators | Meaning |
|---|---|---|
| 1 | `(` `)` | grouping |
| 2 | `.` and `name(...)` | member access, call |
| 3 | `name++` | postfix increment: uses the value, then adds 1 |
| 4 | `*` `/` `%` | multiply; integer division truncating toward zero; remainder |
| 5 | `+` `-` | add, subtract; `+` also joins text |
| 6 | `<` `<=` `>` `>=` | ordered comparison, yielding a truth value |
| 7 | `==` `!=` | equality, inequality, yielding a truth value |
| 8 | `=` | assignment |

Two rules this task depends on:

- `a % b` on non-negative operands yields the non-negative remainder.
- `+` where one operand is text converts the other to its decimal text form and joins the
  two: `"SUM " + total` produces `SUM ` followed by the digits of `total`. That is the
  only route from an integer to text this task needs, and it is an operator, not a name.

## P6. Control flow

A conditional branch:

```
mepone (counter > best) {
    best = counter;
}
```

`mepone` introduces the branch; the condition is written in round brackets; the block
runs only when the condition holds.

An alternative branch is introduced by `bilite` after the conditional's closing brace:

```
mepone (counter == 0) {
    label = label + counter;
} bilite {
    label = label + "-" + counter;
}
```

A chained alternative is `bilite mepone (<condition>) { ... }`, repeated as needed.

Bounded iteration:

```
lukuki (nasisa i = 0; i < 50; i++) {
    ...statements...
}
```

`lukuki` takes three parts separated by `;`: an initialiser run once before the first
pass; a condition tested before each pass, the iteration stopping as soon as it does not
hold; and a step run after each pass. A name introduced in the initialiser is visible
only inside the iteration.

## P7. Aggregates

(not required by this task)

## P8. Standard vocabulary

| Spelling | Parameters | Result |
|---|---|---|
| `System.out.println(x)` | one text value, or one integer value | none; writes one line |

That is the only standard-vocabulary entry this task needs, and the spelling above is
the current one: write it exactly. Sequence construction, ordering, length, splitting and
comparison facilities are not needed — every value the task asks of you can be
accumulated inside one bounded iteration.

## P9. Imports and namespaces

No module or namespace declaration is required. `System` is reachable from the entry
point without any declaration at the top of the unit.

## P10. Output

`System.out.println(x);` writes the text form of `x` and then a line terminator. Four
calls produce four terminated lines, including the last. No other line terminator is
written by hand, and nothing is left buffered at the end of the program.

## P11. Rules specific to this evaluation

Every grammar word of the language is one of the nine pseudo-words below. No other
spelling of these nine jobs exists, and a program using some other spelling is rejected
before it is built.

| Pseudo-word | Job it does | Position |
|---|---|---|
| `balinu` | introduces the named enclosing type of the compilation unit | before the type's name |
| `bilite` | introduces the alternative branch of a conditional | after the conditional's closing brace |
| `lukuki` | introduces bounded iteration over an initialiser, a condition and a step | before the round brackets holding the three parts |
| `mepone` | introduces a conditional branch | before the round brackets holding the condition |
| `metolu` | the 64-bit signed integer type name | before the name it declares |
| `nasisa` | the 32-bit signed integer type name | before the name it declares |
| `sadora` | the result-less designation of a member that yields no value | before the member's name |
| `tenuro` | visibility marker required on the enclosing type and on the entry point | first marker of the declaration |
| `zobolu` | marks a member as belonging to the enclosing type itself rather than to an instance; required on the entry point | after the visibility marker |

Names that are not in this table — `Main`, `main`, `String`, `args`, `System`, `out`,
`println` — are written exactly as this pack spells them. Names you invent yourself are
yours to choose and are never rewritten.

## P12. Worked example

A complete program in Language F: it generates 50 terms of a multiplicative sequence,
accumulates four summaries in one pass, and writes four lines.

```
tenuro balinu Main {
    tenuro zobolu sadora main(String[] args) {
        metolu state = 7;
        metolu sum = 0;
        metolu max = 0;
        nasisa evens = 0;
        String joined = "";
        lukuki (nasisa i = 0; i < 50; i++) {
            state = (state * 48271) % 2147483647;
            metolu term = state % 1000;
            sum = sum + term;
            mepone (term > max) {
                max = term;
            }
            mepone (term % 2 == 0) {
                evens = evens + 1;
            }
            mepone (i < 5) {
                mepone (i == 0) {
                    joined = joined + term;
                } bilite {
                    joined = joined + "-" + term;
                }
            }
        }
        System.out.println("SUM " + sum);
        System.out.println("MAX " + max);
        System.out.println("EVENS " + evens);
        System.out.println("JOINED " + joined);
    }
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
