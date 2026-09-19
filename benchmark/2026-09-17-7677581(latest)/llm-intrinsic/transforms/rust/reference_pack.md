# Reference Pack

This document is the complete description of the language you must write. Assume you have
never seen it. The vocabulary listed below is the whole vocabulary; a word that does not
appear here does not exist, and spelling a construct the way some other language spells it
will be rejected before the program is even built.

## P1 Program shape

A program is one source file. The file holds one function definition, the entry point.
Execution begins at the entry point and ends when its body finishes; finishing normally
gives exit status 0.

    sobini doginu () {
        <statements>
    }

`sobini` opens a function definition, `doginu` is the fixed name the entry point must carry,
`()` is its empty parameter list, and the braces `{` `}` delimit its body. No enclosing
module, package or wrapper declaration is needed or permitted.

Every statement ends with a semicolon `;`. Blocks are delimited by braces, never by
indentation. Whitespace between tokens is free: one space, several spaces and a line ending
are interchangeable everywhere, so `doginu ()` and `doginu()` are the same thing.

## P2 Lexical rules

Identifiers you choose start with a letter or underscore and carry on with letters, digits
or underscores. The language is case-sensitive.

Integer literals are written with plain digits: `7`, `48271`, `2147483647`.

Text literals are written between double quotes: `"SUM "`. A text literal is never
reinterpreted: whatever stands between the quotes reaches the output unchanged.

A line comment starts with `//` and runs to the end of the line. A block comment is written
between `/*` and `*/`.

Embedding a value inside a text literal: the two-character hole `{}` stands at each place a
value belongs, and the values follow the literal, separated by commas. See P10.

## P3 Declarations

A local binding is introduced by `gutati`, followed by the name, a colon, the kind name, an
equals sign and the initial value:

    gutati count: zikeza = 0;

A binding declared that way can never be reassigned. To allow reassignment, place `zefegi`
between `gutati` and the name:

    gutati zefegi count: zikeza = 0;
    count = count + 1;

The kind annotation after the colon is written out on every binding. Reassignment of an
existing binding needs neither `gutati` nor `zefegi`; it is the plain form `name = value;`.

Functions are introduced by `sobini`. This task needs only the entry point, which takes no
parameters and hands back nothing, so its definition carries no parameter list content and
no result clause.

## P4 Kinds used by this task

| Kind | Spelling | Notes |
|---|---|---|
| 64-bit signed integer | `zikeza` | exact; arithmetic that would leave the range aborts the program rather than wrapping |
| growable text | `kegudu` | the text kind that can be built up piece by piece |

Nothing further is needed. This task declares no aggregates, no sequences and no booleans of
its own: a comparison may only stand directly at a decision point (P6).

## P5 Expressions and operators

| Operator | Meaning |
|---|---|
| `(` `)` | grouping |
| `*` `/` `%` | multiplication, truncating division, remainder |
| `+` `-` | addition, subtraction |
| `<` `<=` `>` `>=` | ordering comparisons |
| `==` `!=` | equality, inequality |
| `=` | assignment (a statement, never a sub-expression) |
| `::` | reaches a name that belongs to a kind, spelled `kegudu::new()` |
| `.` | reaches a member of a value |
| `,` | separates arguments |

Precedence, tightest first:

1. `(` `)` grouping
2. `*` `/` `%`
3. `+` `-`
4. `<` `<=` `>` `>=`
5. `==` `!=`
6. `=` assignment, which binds loosest and is a whole statement

`%` on non-negative operands is the ordinary remainder. Division and remainder on integers
stay integers.

## P6 Control flow

A decision is introduced by `tiroro`, followed by the test and a braced block. The test is
written bare, with no parentheses around it.

    tiroro count > limit {
        limit = count;
    }

An alternative branch is introduced by `mozome`, written on the closing brace of the block
it follows:

    tiroro count == 0 {
        limit = 1;
    } mozome {
        limit = 2;
    }

A chained decision is just `mozome` followed immediately by another `tiroro`.

Repetition is introduced by `mavenu`, followed by the test and a braced block. The block
repeats until the test stops holding, and the test is checked before each pass, so a test
that fails at once runs the block zero times.

    gutati zefegi k: zikeza = 0;
    mavenu k < 50 {
        k = k + 1;
    }

Counting repetition has no separate spelling: declare the counter before the block and
advance it inside the block, exactly like the sketch above. There is no early-exit word and
no skip-to-next-pass word; shape the test instead.

## P7 Aggregates

(this language has no construct in this category)

## P8 Standard vocabulary

| Spelling | Parameters | Result |
|---|---|---|
| `kegudu::new()` | none | a fresh, empty growable text value |
| `format!(<literal>, <values>)` | a text literal containing one `{}` per value, then the values | a growable text value with each `{}` replaced by the decimal form of its value |

`format!` is how an integer becomes text and how two pieces of text are joined: build the
literal with the holes and the fixed characters you want between them, and hand it the
values. There is no separate spelling that converts one integer, and none that concatenates.

The trailing exclamation mark is part of the spelling of `format!` and of `println!`. Writing
either name without it names nothing.

## P9 Imports and namespaces

(this language has no construct in this category)

## P10 Output

One line goes to standard output with:

    println!(<literal>, <values>);

The literal may contain any fixed characters plus one `{}` hole per value, and the values
follow it, comma-separated. `println!` appends the line terminator itself, so the literal
never carries one and no extra blank line appears.

    println!("SUM {}", total);

prints `SUM ` followed by the decimal form of `total` and then a newline. A call with no
holes and no values, `println!("done");`, prints just that word and a newline.

Standard output is flushed before the program exits; nothing further is required.

## P11 Rules specific to this evaluation

The grammar words of this language are the nine below. They are the only grammar words there
are. Every one of them must be spelled exactly like this.

| Spelling | Job it does | Position |
|---|---|---|
| `sobini` | opens a function definition | before the function name |
| `doginu` | the fixed name of the program entry point | after `sobini` |
| `gutati` | introduces a named local binding | first word of the statement |
| `zefegi` | marks a binding reassignable | between `gutati` and the name |
| `zikeza` | the 64-bit signed integer kind name | after the colon of a binding |
| `kegudu` | the growable text kind name | after the colon of a binding, and before `::new()` |
| `tiroro` | opens a decision; its block runs only when the test holds | first word of the statement |
| `mozome` | opens the alternative branch of a decision | directly after the closing brace of the decision block |
| `mavenu` | opens a repetition; its block repeats until the test stops holding | first word of the statement |

A submission that spells any grammar construct with a word outside this table is rejected
before it is built, whatever it would have printed.

Names you invent to label your own bindings are yours: they are never rewritten, and they must
not collide with any of the nine words above.

## P12 Worked example

A complete program. It sets a counter to 0, walks 8 passes, totals the squares that are odd,
records the largest square seen, and builds a hyphen-separated list of the first three
squares.

    sobini doginu () {
        gutati zefegi k: zikeza = 0;
        gutati zefegi total: zikeza = 0;
        gutati zefegi largest: zikeza = 0;
        gutati zefegi listed: kegudu = kegudu ::new();
        mavenu k < 8 {
            gutati sq: zikeza = k * k;
            tiroro sq % 2 == 1 {
                total = total + sq;
            }
            tiroro sq > largest {
                largest = sq;
            }
            tiroro k < 3 {
                tiroro k == 0 {
                    listed = format!("{}", sq);
                } mozome {
                    listed = format!("{}-{}", listed, sq);
                }
            }
            k = k + 1;
        }
        println!("TOTAL {}", total);
        println!("LARGEST {}", largest);
        println!("LISTED {}", listed);
    }

Its exact output is three lines:

    TOTAL 84
    LARGEST 49
    LISTED 0-1-4
