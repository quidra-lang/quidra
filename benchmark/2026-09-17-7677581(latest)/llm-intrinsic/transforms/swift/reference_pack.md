# Reference Pack — Condition I1

This is the only description of the language you will receive. Everything you need to
write the requested program is below. Seven grammar words of this language have been
replaced by invented words; the invented words are the real spelling as far as you are
concerned, and the table in section 10 defines every one of them.

## 1. Program shape

A source file is a single compilation unit. There is **no unit declaration and no
entry-point declaration**: the statements written at the outermost level of the file are
the program, and they run from top to bottom. When the last one finishes, the process ends
with status 0.

```
bukuda counter: derozi = 0
counter = counter + 1
print("\(counter)")
```

A block is delimited by `{` and `}`. Statements are separated by line ends; you do not
write a terminator at the end of an ordinary statement. Two statements may share a line
only when a `;` stands between them.

The opening `{` of a block may sit on the same line as the header that introduces it or on
the next line; both build. This pack always writes it on the same line.

**One hard rule about spacing.** Around a two-operand operator the spacing must be
**symmetric**: write `a + b` or write `a+b`, but never `a +b` and never `a+ b`. Space on
one side only makes the compiler read the operator as a one-operand sign, and the build
fails with a message about statements needing a separator. This applies to every operator
in section 5.

A binding that is declared and never read afterwards is only a warning here, not an error,
so nothing forces you to use everything you declare.

## 2. Lexical rules

* **Identifiers** are letters, digits and `_`, and do not begin with a digit. Upper and
  lower letters are distinct. You choose your own identifier names freely; they are not
  part of the substitution above.
* **Whole-number literals** are written in the ordinary decimal way: `7`, `48271`,
  `2147483647`.
* **Text literals** are enclosed in double quotes: `"SUM "`, `"-"`, `""`. A backslash
  escapes the next character inside them. An ordinary text literal may not span a line end.
* **Comments** run from `//` to the end of the line, or from `/*` to `*/`.
* Whitespace outside a literal is insignificant except that a line end closes a statement
  (section 1) and that operator spacing must be symmetric (section 1).
* **Embedding a value inside a text literal:** write `\(` *expression* `)` inside the
  quotes. The expression is evaluated and its ordinary written form is spliced into the
  text at that point:

  ```
  print("SUM \(total)")
  ```

  A whole number spliced this way appears in plain decimal, with no separators and no sign
  when it is not negative. This is the way to turn a number into text in this language;
  there is no separate conversion call you need.

## 3. Declarations

A named binding is introduced by one of two words, followed by the name, a `:`, its kind,
`=`, and the initial value:

```
bukuda total: derozi = 0
tutovo step: derozi = 1
bukuda label: radipi = ""
```

* `bukuda` introduces a binding you may **reassign** later with a plain `=` statement.
* `tutovo` introduces a binding whose value is **fixed** once given; assigning to it again
  is a build error.

Reassignment is a statement, never part of an expression:

```
total = total + 1
```

Writing the `:` *kind* part is optional when the initial value already makes the kind
obvious, but writing it is always allowed and this pack always writes it.

## 4. Kinds used by this task

| Kind | Written | What it holds |
|---|---|---|
| whole number | `derozi` | a signed 64-bit whole number, exactly, with no rounding |
| text | `radipi` | a sequence of characters |

`derozi` arithmetic is exact at every magnitude this task produces. It is **checked**: a
result that will not fit in 64 bits stops the program instead of wrapping around. Nothing
in this task comes near that limit.

## 5. Expressions and operators

| Operator | Meaning | Applies to |
|---|---|---|
| `*` | product | `derozi` |
| `%` | remainder after truncating division | `derozi` |
| `+` | sum; also joins two `radipi` values end to end | `derozi`, `radipi` |
| `-` | difference | `derozi` |
| `<` `>` | strictly less, strictly greater | `derozi` |
| `<=` `>=` | not greater, not less | `derozi` |
| `==` `!=` | equal, unequal | `derozi`, `radipi` |
| `a..<b` | the ascending run of whole numbers from `a` up to but **excluding** `b` | `derozi` |
| `=` | assignment; a statement, never part of an expression | both kinds |

Precedence, tightest first. Parentheses `(` `)` override it.

| Level | Operators |
|---:|---|
| 1 (tightest) | `*` `%` |
| 2 | `+` `-` |
| 3 | `..<` |
| 4 | `<` `<=` `>` `>=` `==` `!=` |

So `a + b * c` means `a + (b * c)`, and `(a * b) % c` needs its parentheses written.

There is no separate text-joining operator: `+` between two `radipi` values produces their
concatenation.

## 6. Control flow

**Conditional branch.** `bigoze`, then a condition, then a block. The condition is *not*
parenthesised, and the block braces are never omitted.

```
bigoze n > limit {
    limit = n
}
```

**Bounded iteration.** `vumodu`, then a fresh name, then `piripo`, then the run of values
to walk, then a block. The name is bound anew on each pass and is fixed within the body;
it does not have to be declared beforehand, and it is not visible after the block.

```
vumodu i piripo 0..<50 {
    ...body...
}
```

That header walks `i` over 0, 1, 2, … 49 — fifty passes, the upper end excluded.

## 7. Standard vocabulary

This name is **not** substituted; it is written exactly as shown.

| Written | Parameters | Result |
|---|---|---|
| `print(t)` | one value `t` | writes `t`, then a line terminator, to standard output; yields nothing |

`print` is the current spelling of the one-line output facility in this toolchain. There is
no older or alternative spelling to consider, no instance or handle to obtain first, and no
error to propagate from it; it writes the line terminator itself, so do not append one.

## 8. Modules

This task needs **no** module brought into scope. Everything section 7 names, and every
kind section 4 names, is available in a plain source file with nothing written above it.

## 9. Output

One line of standard output is produced by `print(t)`, where `t` is a single `radipi`
value. The trailing line terminator is supplied by the call. Build the line first, or embed
the values directly (section 2):

```
print("TOTAL \(total)")
```

## 10. Substituted words (condition I1)

Every word in this table replaces one grammar word of the underlying language. These are
the only substituted words; everything beyond them is either standard vocabulary
(section 7) or a name you chose yourself.

| Word | Position | What it does |
|---|---|---|
| `bukuda` | precedes a binding name | introduces a binding that may be reassigned |
| `tutovo` | precedes a binding name | introduces a binding whose value is fixed once given |
| `derozi` | after a binding name and `:` | the 64-bit signed whole-number kind |
| `radipi` | after a binding name and `:` | the text kind |
| `bigoze` | precedes a condition and a block | introduces a conditional branch |
| `vumodu` | begins a bounded-iteration header | introduces bounded iteration |
| `piripo` | between the bound name and the run of values | separates the two halves of that header |

## 11. Worked example

A complete program. It walks a whole-number sequence, accumulates a total, tracks the
greatest element, counts the elements divisible by two, and joins the leading five elements
into one piece of text. It uses every construct described above and no more.

```
bukuda state: derozi = 7
bukuda total: derozi = 0
bukuda largest: derozi = 0
bukuda evens: derozi = 0
bukuda joined: radipi = ""
vumodu i piripo 0..<50 {
    state = (state * 48271) % 2147483647
    tutovo term: derozi = state % 1000
    total = total + term
    bigoze term > largest {
        largest = term
    }
    bigoze term % 2 == 0 {
        evens = evens + 1
    }
    bigoze i < 5 {
        bigoze i > 0 {
            joined = joined + "-"
        }
        joined = joined + "\(term)"
    }
}
print("SUM \(total)")
print("MAX \(largest)")
print("EVENS \(evens)")
print("JOINED \(joined)")
```

It writes exactly these four lines, and ends with status 0:

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

## 12. Size of this pack

Recorded by measurement, not by estimate: 236 lines, 8496 characters.
