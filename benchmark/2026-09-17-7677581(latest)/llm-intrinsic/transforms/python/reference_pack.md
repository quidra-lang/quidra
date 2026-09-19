# Reference Pack

This pack is the only description of the language you are writing. Every word token
of its grammar is given below; nothing beyond this pack is needed.

## 1. Program shape

A source text is a run of statements starting at column zero, executed from the top
downwards. There is no wrapper construct, no entry-point declaration, no module
header. One statement occupies one line; no terminator character closes it.

A block is introduced by a header line whose last character is a colon `:`. The body
of that block is the run of following lines indented further than the header. The
block ends at the first later line whose indentation returns to the header's level.
Indentation is significant: it is the only way blocks are delimited. Use four spaces
per level throughout, never tab characters.

```
<header line>:
    <body statement>
    <body statement>
<statement after the block>
```

## 2. Lexical rules

- Identifier: a letter, otherwise an underscore, then any run of letters, digits,
  underscores. Names are case-sensitive. You choose your own names freely; only the
  twenty word tokens of section 3 are reserved.
- Integer literal: decimal digits, e.g. `48271`. Integers are exact arbitrary-size
  whole numbers; they never wrap around, never overflow.
- Text literal: characters between matching `"` … `"`, alternatively `'` … `'`. A backslash
  escapes the next character. `"\n"` is one line-terminator character.
- Comment: `#` through the end of that line.
- Embedding a value inside a text literal: write the letter `f` immediately before
  the opening quote, then put the expression between braces —
  `f"total {amount} done"` produces the text `total 7 done` when `amount` holds 7.
  Between the braces write a name, otherwise arithmetic upon names: a word token of
  section 3 never appears inside a literal of any kind.
- Whitespace between tokens is free, except at the start of a line, where it carries
  the block structure described above.

## 3. The word tokens of this language

Every grammar word token of this language is listed here. These twenty spellings are
reserved: never use one as a name of your own. Anything you write besides is a name,
a literal, otherwise a library facility of section 7.

| Token | What it does | Position |
|---|---|---|
| `tukoki` | introduces a function definition | first token of the header line |
| `lodira` | yields a function result; ends that call | statement, followed by an expression |
| `muveze` | introduces a conditional branch | header line, followed by a condition, then `:` |
| `lupire` | introduces a chained alternative branch | header line, after a `muveze` block |
| `nuragi` | introduces the final alternative branch | header line, after a `muveze` block, no condition |
| `zomagu` | introduces bounded iteration over a sequence | first token of the header line |
| `kufumo` | separates the iteration name from the sequence it walks | inside a `zomagu` header |
| `vigifo` | introduces conditional iteration: repeats as long as the condition holds | header line, followed by a condition, then `:` |
| `zobumo` | leaves the innermost loop at once | statement, alone |
| `tezumi` | skips ahead to the next round of the innermost loop | statement, alone |
| `nonida` | logical conjunction of two conditions | infix operator |
| `zevepa` | logical disjunction of two conditions | infix operator |
| `fasami` | logical negation of one condition | prefix operator |
| `nogelu` | the affirmative boolean literal | expression |
| `rogido` | the negative boolean literal | expression |
| `digami` | the integer type name | type position |
| `sedagi` | the boolean type name | type position |
| `galozi` | the text type name; applied to an integer it hands back that integer's decimal text form | type position, otherwise called like a function |
| `vorito` | the dynamic-sequence type name | type position |
| `tivuni` | the absent value; also the result type of a function that hands back nothing | type position, otherwise expression |

## 4. Declarations, types, annotations

A name is bound by writing it, then `=`, then an expression. No word token introduces
a binding. Re-binding the same name later is allowed.

```
amount = 7
```

A type annotation may be written after the name, separated by a colon:

```
amount: digami = 7
words: vorito[galozi] = []
```

`vorito[X]` is the type of a dynamic sequence whose elements have type `X`. The
annotation is descriptive; write one wherever the language permits, as above.

A function is declared with `tukoki`, the name, a parenthesised parameter series,
then `->`, the result type, then `:`. Each parameter carries a type annotation. The body
is the indented block. `lodira` hands back the result. A function that hands back
nothing declares the result type `tivuni`.

```
tukoki doubled(value: digami) -> digami:
    lodira value * 2

tukoki announce(message: galozi) -> tivuni:
    print(message)
```

A function is called by naming it, then a parenthesised argument series: `doubled(4)`.

## 5. Expressions, operators

| Form | Meaning |
|---|---|
| `a + b` | integer addition; between two text values, joins them end to end |
| `a - b` | subtraction |
| `a * b` | multiplication |
| `a % b` | remainder after division; with two non-negative operands the result is non-negative |
| `a // b` | division of whole numbers, rounded downwards |
| `a == b`, `a != b` | equality, inequality |
| `a < b`, `a <= b`, `a > b`, `a >= b` | ordering comparisons; between two text values these compare character codes from the left |
| `a nonida b`, `a zevepa b`, `fasami a` | logical conjunction, disjunction, negation |
| `seq[i]` | the element of `seq` at position `i`, counting from 0 |
| `obj.member(args)` | calls a facility that belongs to the value `obj` |
| `name = expr` | binds, as section 4 describes |
| `name += expr` | shorthand: bind `name` to `name + expr` |
| `(` … `)` | grouping, as well as the argument series of a call |
| `[` … `]` | sequence literal: `[]` is the empty sequence, `[1, 2]` has two elements |

Precedence, tightest first:

1. `seq[i]`, `obj.member`, calls
2. unary `-`
3. `*`, `//`, `%`
4. `+`, `-`
5. `==`, `!=`, `<`, `<=`, `>`, `>=`
6. `fasami`
7. `nonida`
8. `zevepa`

Parentheses override precedence. Arithmetic on integers is exact at every size.

## 6. Control flow

```
muveze <condition>:
    <body>
lupire <condition>:
    <body>
nuragi:
    <body>
```

The `lupire` blocks, however many, are optional; the `nuragi` block is optional. At
most one of the bodies runs: the first whose condition holds, falling back on the
`nuragi` body.

```
zomagu <name> kufumo <sequence>:
    <body>
```

The body runs once per element of the sequence, with `<name>` bound to that element.
To walk the whole numbers 0 up to n-1, walk `range(n)` (section 7).

```
vigifo <condition>:
    <body>
```

The body repeats as long as the condition holds, tested before each round. Inside a
loop, `zobumo` leaves the loop at once; `tezumi` abandons the current round.

## 7. Standard vocabulary

These facilities are always available, named exactly as written here.

| Spelling | Result |
|---|---|
| `print(text)` | writes `text` to standard output, then one line terminator. This is the current, correct spelling of writing a line; there is no other. |
| `range(n)` | the whole numbers 0 through n-1, walkable by `zomagu` |
| `len(x)` | the number of elements of a sequence, otherwise the number of characters of a text value |
| `seq.append(x)` | adds `x` at the end of the sequence `seq`; hands back nothing |
| `sep.join(parts)` | one text value: the elements of the sequence `parts`, each a text value, laid end to end with the text `sep` between neighbours. `"-".join(["4", "9"])` is `"4-9"` |
| `galozi(n)` | the decimal text form of the integer `n` |
| `[]` | a fresh empty sequence |

## 8. Modules

The task needs no import statement of any kind. Every facility of section 7 is
available already.

## 9. Output

Write one line by calling `print` with one text value. The line terminator is added
by `print` itself; never append one yourself. Four calls produce four lines, each
ending with a terminator. Nothing besides is written to standard output.

## 10. Worked example

This program runs exactly as shown.

```
tukoki collect(count: digami) -> vorito[digami]:
    out: vorito[digami] = []
    value: digami = 3
    zomagu step kufumo range(count):
        value = (value * 11) % 97
        out.append(value)
    lodira out


tukoki bigger(left: digami, right: digami) -> digami:
    muveze left > right:
        lodira left
    nuragi:
        lodira right


tukoki main() -> tivuni:
    items: vorito[digami] = collect(6)
    total: digami = 0
    biggest: digami = items[0]
    odds: digami = 0
    zomagu item kufumo items:
        total = total + item
        biggest = bigger(biggest, item)
        muveze item % 2 == 1:
            odds = odds + 1
    pieces: vorito[galozi] = []
    zomagu index kufumo range(3):
        pieces.append(galozi(items[index]))
    label: galozi = ":".join(pieces)
    print("TOTAL " + galozi(total))
    print("BIGGEST " + galozi(biggest))
    print("ODDS " + galozi(odds))
    print("LABEL " + label)


main()
```

Its standard output is exactly these four lines:

```
TOTAL 346
BIGGEST 93
ODDS 4
LABEL 33:72:16
```

## 11. Submitting

Submit one source text, complete on its own, inside a single fenced code block. The
harness writes it to the entry file, builds it, then runs it; you never state a file
name, a command, nor any build option. Your program reads no input. It must leave a
status of zero, which it does by running to the end without raising an error.
