# Reference Pack — Language C

This is the complete description of Language C that you are given. Everything you need
to write the requested program is here. Nothing outside this document describes the
language, and the language is not named anywhere.

## P1. Program shape

A source file is a sequence of top-level items. Two kinds of item matter here:

- module directives, written at the very top of the file;
- function definitions.

Execution begins at the function named `konazo`. It takes no parameters and produces a
whole-number status code. A status code of `0` means success.

```
derane konazo() {
    <statements>
}
```

Every statement ends with a semicolon `;`. A block is a run of statements wrapped in
braces `{` and `}`. Whitespace and line breaks are not significant; indentation is
decorative.

## P2. Lexical rules

- Names are made of letters, digits and underscore, and never start with a digit.
  Names are case-sensitive.
- Whole-number literals are written as plain digits: `0`, `50`, `2147483647`.
- Text literals are wrapped in double quotes: `"SUM "`. A backslash escapes the next
  character inside them.
- A single-line note starts with `//` and runs to the end of the line. A spanning note
  is wrapped in `/*` and `*/`.
- Embedding a value inside a text literal: (this language has no construct in this
  category — build text by joining values instead, see P8).

## P3. Declarations

A named value is introduced by writing its type, then its name, then optionally `=` and
an initial value:

```
derane counter = 0;
litasa litasa tally = 0;
```

A declared name may be reassigned later with `=`. Prefixing the type with `febuvo`
makes the binding immutable after its initial value:

```
febuvo derane limit = 50;
```

A function definition is a result type, a name, a parenthesised parameter list, and a
block. `kelugo` hands a value back to the caller and leaves the function.

## P4. Types used by this task

| Spelling | What it holds |
|---|---|
| `derane` | a whole number of the basic width; ample past 2,000,000,000 |
| `litasa litasa` | a whole number of the wide width; the word is written twice; ample past 9,000,000,000,000,000,000 |
| `std::string` | a text value of any length |

Arithmetic that can pass 2,000,000,000 must use the wide width, so the running total
and the generator state in this task are declared `litasa litasa`.

## P5. Expressions and operators

| Operator | Meaning |
|---|---|
| `( )` | grouping, and the call of a function |
| `::` | reaches a name inside a namespace, as in `std::string` |
| `-x` | negation |
| `*` `/` `%` | product, truncating quotient, remainder |
| `+` `-` | sum, difference — and `+` also joins two text values |
| `<<` | sends a value to an output stream (P10) |
| `<` `>` `<=` `>=` | ordering comparisons, yielding a truth value |
| `==` `!=` | sameness comparisons, yielding a truth value |
| `=` | assignment; the left side must be a declared name |

Precedence, tightest first. Operators on one row bind equally and group left to right,
except `=`, which groups right to left.

```
1.  ( )   ::
2.  -x
3.  *   /   %
4.  +   -
5.  <<
6.  <   >   <=   >=
7.  ==   !=
8.  =
```

So `state * 48271 % 2147483647` means `(state * 48271) % 2147483647`, and
`a + b == c` means `(a + b) == c`. Parentheses override all of this.

The remainder operator `%` on non-negative operands yields a non-negative result.

## P6. Control flow

**Conditional.** `rigumo` takes a parenthesised truth value and a block. `pemafo`
introduces the alternative block and is optional. Chaining is done by writing `pemafo`
immediately ahead of a further `rigumo`.

```
rigumo (a > b) {
    <statements>
} pemafo {
    <statements>
}
```

**Bounded iteration.** `sobisi` takes three parenthesised parts separated by semicolons
— an initializer, a continuation test, and a step — and then a block. The initializer
runs once; the block repeats while the test holds; the step runs after each pass.

```
sobisi (derane i = 0; i < 50; i = i + 1) {
    <statements>
}
```

**Result.** `kelugo <value>;` hands a value back and leaves the function at once.

These are the only control-flow words this language offers you here. There is no other
word-shaped loop or branch form available in this document.

## P7. Aggregates

(this language has no construct in this category — this task needs none)

## P8. Standard vocabulary

These names come from the standard library. They are spelled exactly as shown.

| Spelling | What it does |
|---|---|
| `std::string` | the text type; a value declared this way starts out empty |
| `std::to_string(n)` | yields the decimal text of a whole number `n` |
| `a + b` | joins two text values, or a text value and a text literal, yielding text |
| `std::cout` | the standard output stream |
| `std::endl` | sends a line terminator to a stream and flushes it |

## P9. Modules

A module directive is a `#` written at the start of a line, immediately followed by the
word `nusabo`, then a space, then the module name in angle brackets. These two
directives make everything in P8 reachable and belong at the top of the file:

```
#nusabo <iostream>
#nusabo <string>
```

## P10. Output

One line of text is written by sending its pieces to the standard output stream with
`<<`, ending with `std::endl`:

```
std::cout << "SUM " << total << std::endl;
```

Each `<<` appends the next piece. Whole numbers are rendered in decimal. `std::endl`
supplies the line terminator, so no newline character is written by hand. This is the
current spelling of the output facility in this language; there is no other form you
need, and no error-propagation marker is required on the entry point.

## P11. Rules specific to this evaluation

Nine word-shaped tokens of this language have been replaced, one-to-one, by the
pseudo-words below. These spellings are the real ones, from your point of view: write them
exactly as given. Any other word-shaped grammar token you may have seen elsewhere is not part
of this language and will be rejected.

| Pseudo-word | Job it does | Position |
|---|---|---|
| `rigumo` | introduces a conditional branch | ahead of `( test )` and a block |
| `pemafo` | introduces the alternative branch | between the two blocks |
| `sobisi` | introduces bounded iteration | ahead of `( init ; test ; step )` |
| `kelugo` | hands a function result back and leaves the function | statement position |
| `nusabo` | directive word that pulls a module in by name | immediately after a line-leading `#` |
| `derane` | the basic whole-number type name | ahead of the name being declared |
| `litasa` | width qualifier that widens the whole-number type; written twice to reach the wide form | ahead of the name being declared |
| `konazo` | the fixed name of the program entry point | function name position |
| `febuvo` | marks a binding immutable after its initial value | ahead of the type |

Ordinary names you choose yourself — variables, your own functions — are untouched by
this table and you spell them however you like, provided they are not one of the nine
words above.

## P12. Worked example

A complete program, unrelated to your task. It walks a small whole-number recurrence
six times, keeps a running total, tracks the greatest value seen, counts the values
that are odd, and joins every value it produced into one text value separated by
slashes. It uses every construct this document describes and nothing more.

```
#nusabo <iostream>
#nusabo <string>

derane konazo() {
    febuvo derane steps = 6;
    litasa litasa value = 3;
    litasa litasa total = 0;
    litasa litasa biggest = -1;
    litasa litasa odds = 0;
    std::string chain;
    sobisi (derane i = 0; i < steps; i = i + 1) {
        value = (value * 7) % 101;
        total = total + value;
        rigumo (value > biggest) {
            biggest = value;
        }
        rigumo (value % 2 == 1) {
            odds = odds + 1;
        }
        rigumo (i == 0) {
            chain = std::to_string(value);
        } pemafo {
            chain = chain + "/" + std::to_string(value);
        }
    }
    std::cout << "TOTAL " << total << std::endl;
    std::cout << "BIGGEST " << biggest << std::endl;
    std::cout << "ODDS " << odds << std::endl;
    std::cout << "CHAIN " << chain << std::endl;
    kelugo 0;
}
```

This program builds and runs, exits with status `0`, and writes exactly these four
lines:

```
TOTAL 193
BIGGEST 53
ODDS 3
CHAIN 21/46/19/32/22/53
```
