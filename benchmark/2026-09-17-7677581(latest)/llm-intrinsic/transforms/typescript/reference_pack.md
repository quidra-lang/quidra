# Reference Pack

This pack is the only description this language has. Every word token in its
grammar is given below; nothing beyond this pack is needed.

## 1. Program shape

A source text is a statement sequence, executed from the top downwards. There is no
wrapper construct, no entry-point declaration, no module header. A statement ends
with a semicolon `;`. Whitespace and line breaks between tokens carry no meaning:
indentation is decoration, never structure. A block is a statement sequence enclosed
in braces `{` … `}`, and goes wherever a header asks one, as shown in section 6.

A routine declared at the top level may be called from the top level. Declaring a
routine does not run it; calling it does. Names bound at the top level share one
space with names the surrounding environment already provides, so keep your working
names inside a routine and call that routine, as the example in section 10 does.

## 2. Lexical rules

- Identifier: a letter or an underscore, then any run containing letters, digits and
  underscores. Names are case-sensitive: `total` and `Total` are two names. You choose
  your own names freely; only the seventeen word tokens in section 3 are reserved.
- Integer literal: decimal digits, e.g. `48271`. Arithmetic on whole numbers is
  exact as long as every value stays below 9007199254740992 in magnitude; every
  value this task produces stays far below that, so no rounding can occur.
- Text literal: characters between matching `"` … `"`, alternatively `'` … `'`. A
  backslash escapes the next character. `"\n"` is one line-terminator character.
- Comment: `//` through the end containing that line; alternatively `/*` … `*/`.
- Embedding a value inside a text literal: write the whole literal between backticks
  `` ` `` … `` ` `` and put each embedded expression between `${` and `}` —
  `` `total ${amount} done` `` produces the text `total 7 done` when `amount` holds 7.
  Between those braces write a name, alternatively arithmetic upon names: a word
  token from section 3 never appears inside a literal in any form.

## 3. The word tokens in this language

Every grammar word token in this language is listed here. These seventeen spellings
are reserved: never use one as a name that is yours. Anything you write besides is a
name, a literal, alternatively a facility from section 7.

| Token | What it does | Position |
|---|---|---|
| `vefedi` | introduces a routine definition | first token in the header |
| `vudoru` | yields a routine's result and ends that call | statement, followed by an expression |
| `vobunu` | introduces a name binding that may never be re-bound afterwards | first token in the declaration |
| `busopo` | introduces a name binding that may be re-bound afterwards | first token in the declaration |
| `memagu` | introduces a conditional branch | header, followed by a parenthesised condition |
| `ninobi` | introduces the alternative branch | header, placed after a `memagu` block |
| `padiri` | introduces bounded iteration | header, followed by a parenthesised iteration clause |
| `kubuva` | separates the iteration name from the sequence it walks | inside a `padiri` header |
| `biremo` | introduces conditional iteration: repeats as long as the condition holds | header, followed by a parenthesised condition |
| `zeligu` | leaves the innermost loop at once | statement, alone |
| `bedara` | skips ahead to the next round in the innermost loop | statement, alone |
| `gerido` | the affirmative truth literal | expression |
| `zaluga` | the negative truth literal | expression |
| `gebamu` | the numeric type name | type position |
| `zisaro` | the truth-value type name | type position |
| `bunivu` | the text type name | type position |
| `zerinu` | the result type declared by a routine that yields nothing | type position |

## 4. Declarations, types, annotations

A name is bound by writing `vobunu` or `busopo`, the name, a colon, its type, `=`,
an expression, then `;`. A name introduced by `vobunu` may never be re-bound; a name
introduced by `busopo` may be re-bound later by writing `name = expr;` without any
word token. Write the type annotation wherever the language permits one, as here.

```
vobunu amount: gebamu = 7;
busopo running: gebamu = 0;
running = running + amount;
```

`T[]` is the type containing a dynamic sequence whose elements have type `T`, so
`gebamu[]` is a sequence carrying numeric elements and `bunivu[]` is a sequence
carrying text elements. `[]` is a fresh empty sequence, so a fresh empty sequence
carrying text elements is declared `vobunu words: bunivu[] = [];`.

A routine is declared with `vefedi`, its name, a parenthesised parameter series,
a colon, its result type, then a block. Each parameter carries a type annotation.
`vudoru` hands the result back. A routine that hands nothing back declares the
result type `zerinu`.

```
vefedi doubled(value: gebamu): gebamu {
    vudoru value * 2;
}
```

A routine is called by naming it, then a parenthesised argument series: `doubled(4)`.

## 5. Expressions, operators

| Form | Meaning |
|---|---|
| `a + b` | addition; between two text values, joins them end to end |
| `a - b` | subtraction |
| `a * b` | multiplication |
| `a / b` | division; its result may carry a fractional part |
| `a % b` | remainder after division; with two non-negative operands the result is non-negative |
| `a === b`, `a !== b` | equality, inequality. These are the spellings; use no others |
| `a < b`, `a <= b`, `a > b`, `a >= b` | ordering comparisons; between two text values these compare character codes from the left |
| `a && b`, `a \|\| b`, `!a` | logical conjunction, disjunction, negation |
| `seq[i]` | the element inside `seq` at position `i`, counting from 0 |
| `obj.member(args)` | calls a facility belonging to the value `obj` |
| `obj.member` | reads a property belonging to the value `obj` |
| `name = expr` | re-binds a name introduced by `busopo` |
| `(` … `)` | grouping, as well as the argument series in a call |
| `[` … `]` | sequence literal: `[]` is empty, `[1, 2]` carries two elements |

Precedence, tightest first: (1) `seq[i]`, `obj.member`, calls; (2) `!` and unary `-`;
(3) `*`, `/`, `%`; (4) `+`, `-`; (5) `<`, `<=`, `>`, `>=`; (6) `===`, `!==`; (7) `&&`;
(8) `||`. Parentheses override precedence.

## 6. Control flow

```
memagu (<condition>) {
    <body>
} ninobi memagu (<condition>) {
    <body>
} ninobi {
    <body>
}
```

The chained `ninobi memagu` headers, however many, are optional; the final `ninobi`
block is optional. At most one body runs: the first whose condition holds, falling
back on the plain `ninobi` body.

```
padiri (busopo i: gebamu = 0; i < n; i = i + 1) {
    <body>
}
```

The parenthesised clause carries three parts separated by semicolons: a binding that
runs once at the start, a condition tested before each round, and a step performed
after each round. The body above runs with `i` holding 0, 1, … up to n-1.

```
padiri (vobunu item kubuva <sequence>) {
    <body>
}
```

This second shape runs the body once per element inside the sequence, with `item`
bound to that element.

```
biremo (<condition>) {
    <body>
}
```

The body repeats as long as the condition holds, tested before each round. Inside a
loop, `zeligu` leaves the loop at once; `bedara` abandons the current round.

## 7. Standard vocabulary

These facilities are always available, named exactly as written here; their spellings
are not among the reserved word tokens in section 3.

| Spelling | Result |
|---|---|
| `console.log(text)` | writes `text` to standard output, then one line terminator |
| `seq.push(x)` | adds `x` at the end inside the sequence `seq` |
| `seq.length` | how many elements the sequence `seq` carries |
| `txt.length` | how many characters the text value `txt` carries |
| `seq.join(sep)` | one text value: the elements inside `seq` laid end to end with the text `sep` between neighbours. `["4", "9"].join("-")` is `"4-9"` |
| `String(n)` | the decimal text form belonging to the numeric value `n` |

## 8. Modules

The task needs no import statement in any form. Every facility in section 7 is
available already, and this language needs no module header.

## 9. Output

Write one line by calling `console.log` with one text value. This call is the
current, correct spelling that writes a line in this language; there is no other,
and no older spelling is accepted. The line terminator is produced by the call
itself; never append one yourself. Four calls produce four lines, each ending with a
terminator. Nothing besides is written to standard output.

## 10. Worked example

This program runs exactly as shown.

```
vefedi collect(count: gebamu): gebamu[] {
    vobunu out: gebamu[] = [];
    busopo value: gebamu = 3;
    padiri (busopo step: gebamu = 0; step < count; step = step + 1) {
        value = (value * 11) % 97;
        out.push(value);
    }
    vudoru out;
}

vefedi bigger(left: gebamu, right: gebamu): gebamu {
    memagu (left > right) {
        vudoru left;
    } ninobi {
        vudoru right;
    }
}

vefedi main(): zerinu {
    vobunu items: gebamu[] = collect(6);
    busopo total: gebamu = 0;
    busopo biggest: gebamu = items[0];
    busopo odds: gebamu = 0;
    padiri (vobunu item kubuva items) {
        total = total + item;
        biggest = bigger(biggest, item);
        memagu (item % 2 === 1) {
            odds = odds + 1;
        }
    }
    vobunu pieces: bunivu[] = [];
    padiri (busopo index: gebamu = 0; index < 3; index = index + 1) {
        pieces.push(String(items[index]));
    }
    vobunu label: bunivu = pieces.join(":");
    console.log("TOTAL " + String(total));
    console.log("BIGGEST " + String(biggest));
    console.log("ODDS " + String(odds));
    console.log("LABEL " + label);
}

main();
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
status containing zero, which it does by running to the end without raising an error.
