# Reference Pack — Language A

You have never seen this language. Everything you need is below. Every word token that
carries grammatical meaning has been replaced by a pseudo-word; the table of section 11
is the only place those pseudo-words are defined.

## 1. Program shape

A compilation unit is a plain text file. Statements written at the outermost level are
the entry point: they execute top to bottom when the program starts. There is no
enclosing wrapper, no unit header, no module header.

A statement ends at the end of its line. There is no statement terminator character.

A block is delimited by indentation: exactly four spaces per level. Dedentation closes a
block. Tabs are rejected by the compiler. A construct that opens a block ends its own
line, then its body follows, indented one level deeper.

Execution stops with status zero when the last top-level statement completes.

## 2. Lexical rules

An identifier starts with a letter as well as possibly an underscore, then continues with
letters, digits, underscores. Identifiers are case sensitive.

Integer literals are decimal digits only. There are no base prefixes. A literal with a
decimal point is a floating literal.

A text literal is delimited by double quote characters. There are no backslash escape
sequences: a backslash is an ordinary character. Embedding a value inside a text literal:
write the expression within braces, as `"total {counter}"`. Doubled braces `{{` plus `}}`
produce literal braces.

A comment runs from `//` to the end of the line.

Reserved words may never be reused as user names. Names you choose yourself are ordinary
identifiers; the pseudo-words of section 11 are reserved, so never reuse one as a variable
name.

## 3. Declarations

A binding is declared by writing its type, then its name, then `=`, then its value:

```
lisutu counter = 0
```

Writing `gudevi` ahead of the type makes the binding immutable, so it can never be
written again. Without `gudevi` the binding may be assigned later with `name = value`.
A writable binding has no separate introducer word: the type itself introduces it.

A function is declared by writing its result type, then its name, then a parenthesized
parameter list, then a block. Each parameter is a type followed by a name. `riripo`
yields the result:

```
lisutu doubled(lisutu value)
    riripo value * 2
```

A function whose result type is `vipike` yields nothing.

## 4. Types used by this task

| Spelling | Meaning |
|---|---|
| `lisutu` | 64-bit signed integer; every arithmetic step is overflow checked |
| `febole` | immutable text |
| `kitipe` | truth value; its two literals are `tegago` as well as `samibu` |
| `vipike` | the result-less designation of a function that yields nothing |

Integer arithmetic never wraps: an operation whose exact result exceeds the range of the
type is a deterministic runtime failure with status 101, never a silent wrap.

Operand types must agree exactly. A numeric literal takes the type of its context when it
is representable there. A whole-number value is converted to another whole-number type by
calling that destination type, as `lisutu(value)`; the conversion is range checked.

## 5. Expressions plus operators

Arithmetic: `+` `-` `*` `/` `%` where `/` is truncating integer division between two
integers, `%` is the remainder. Comparison: `==` `!=` `<` `<=` `>` `>=`. Logical:
`rususo` (conjunction), `pozamo` (disjunction), `doluko` (negation, written ahead of its
operand). Conditions must be truth values; there is no coercion of a number to a
condition.

Assignment is `=`. The compound forms `+=` `-=` `*=` `/=` `%=` evaluate their target
once. Text values are joined with `+`. Member access is `.`, a call is `name(arguments)`,
indexing is `values[position]`.

Precedence, tightest first:

| Level | Operators |
|---:|---|
| 1 | `.` member access, `(...)` call, `[...]` indexing |
| 2 | unary `-`, `doluko` |
| 3 | `*` `/` `%` |
| 4 | `+` `-` |
| 5 | `<` `<=` `>` `>=` |
| 6 | `==` `!=` |
| 7 | `rususo` |
| 8 | `pozamo` |

Parentheses group. `state * 48271 % 2147483647` therefore means
`(state * 48271) % 2147483647`.

## 6. Control flow

A conditional is `kimeke <condition>` followed by an indented block. A chained
alternative carrying its own condition is `vubote <condition>`; a final unconditional
alternative is `togapi`. Each opens a block.

```
kimeke value > limit
    limit = value
vubote value == limit
    ties = ties + 1
togapi
    others = others + 1
```

Bounded iteration is `betede <name> kurodi <sequence>` followed by an indented block. The
loop binder is a fresh name of the loop. Condition-driven repetition is
`zitoge <condition>` followed by an indented block. `bupena` leaves the nearest loop;
`gisade` advances the nearest loop to its next step.

## 7. Aggregates

A user-defined aggregate type is declared with `pevoku`, whose body holds typed fields.
This task needs no aggregate type.

## 8. Standard vocabulary

These names are ordinary library names, spelled as the language itself spells them.

| Spelling | Parameters | Result |
|---|---|---|
| `print(value)` | one scalar value: a number, a truth value, a text value | writes that value, then a line terminator, to standard output |
| `write(value)` | as above | writes that value with no line terminator |
| `range(start, stop)` | two integers | the ascending step-one sequence from `start` up to but excluding `stop`; `range(stop)` starts at zero |
| `len(values)` | one sequence | how many elements it holds |

`print` is the current spelling of the line-writing facility of this language version;
there is no older alternative spelling to prefer.

## 9. Imports plus namespaces

Standard names such as those of section 8 are always visible. This task requires no
module load at all, so no `mutitu` declaration is needed.

## 10. Output

One line of text is written by `print(value)`, which appends the line terminator itself.
To write a label beside a computed value, interpolate it:

```
print("TOTAL {total}")
```

Two text values are joined with `+`, so a label may also be built up piecewise:
`print("TOTAL " + "{total}")`.

## 11. Rules specific to this evaluation

Every word token below has been replaced. Use the pseudo-word; the real spelling of this
language is never valid here. Ordinary user identifiers are yours to choose freely.

| Pseudo-word | Job it does | Position |
|---|---|---|
| `kimeke` | introduces a conditional branch | ahead of the condition, opens a block |
| `vubote` | chained alternative branch carrying its own condition | at the level of `kimeke` |
| `togapi` | the final unconditional alternative branch | at the level of `kimeke` |
| `betede` | introduces bounded iteration | ahead of the loop binder |
| `kurodi` | separates the loop binder from the sequence walked | between them |
| `zitoge` | introduces condition-driven repetition | ahead of the condition |
| `bupena` | leaves the innermost loop | statement |
| `gisade` | advances the innermost loop one step | statement |
| `riripo` | yields a function result | statement, ahead of the expression |
| `gudevi` | marks a binding that can never be written again | ahead of the type |
| `lisutu` | the 64-bit signed integer type name | ahead of the name it types |
| `febole` | the text type name | ahead of the name it types |
| `kitipe` | the truth-value type name | ahead of the name it types |
| `vipike` | the result-less designation of a function | as a result type |
| `tegago` | the affirmative truth literal | expression |
| `samibu` | the negative truth literal | expression |
| `rususo` | logical conjunction | between two conditions |
| `pozamo` | logical disjunction | between two conditions |
| `doluko` | logical negation | ahead of its operand |
| `pevoku` | introduces an aggregate type declaration | opens a block |
| `muzomu` | marks a member that replaces an inherited one | ahead of that member |
| `dibego` | names the parent implementation inside a member | expression prefix |
| `silute` | introduces multi-alternative dispatch over a value | opens a block |
| `kesudu` | propagates a failure result out of the enclosing function | ahead of an expression |
| `mutitu` | loads an external module | top level |

The last six rows exist so the table is complete; this task requires no construct from them.

## 12. Worked example

This program builds five terms of a small recurrence, accumulating a total, the largest
term, a count of even terms, plus a joined text of the terms.

```
lisutu stepped(lisutu seed)
    riripo seed * 11 % 13

lisutu carry = 4
lisutu total = 0
lisutu largest = 0
lisutu evens = 0
febole chain = ""
betede i kurodi range(0, 5)
    carry = stepped(carry)
    total = total + carry
    kimeke carry > largest
        largest = carry
    kimeke carry % 2 == 0
        evens = evens + 1
    kimeke i == 0
        chain = "{carry}"
    togapi
        chain = chain + ":" + "{carry}"
print("TOTAL {total}")
print("LARGEST {largest}")
print("EVENS {evens}")
print("CHAIN {chain}")
```

It writes exactly these four lines, then exits with status zero:

```
TOTAL 29
LARGEST 12
EVENS 2
CHAIN 5:3:7:12:2
```
