# Reference Pack — Condition I1

This is the only description of the language you will receive. Everything you need to
write the requested program is below. Nine grammar words of this language have been
replaced by invented words; the invented words are the real spelling as far as you are
concerned, and the table in section 10 defines every one of them.

## 1. Program shape

A source file is a single compilation unit. Its very first line names the unit:

```
nemetu lemuma
```

Every program in this task set uses the unit name `lemuma`. After that line come the
module bindings (section 8), then the declarations.

Execution begins at the function named `lemuma`. It takes no parameters and yields no
result. When control falls off the end of its body the process ends with status 0.

```
mizufi lemuma () {
	...statements...
}
```

A block is delimited by `{` and `}`. Statements are separated by line ends; you do not
write a terminator at the end of an ordinary statement.

**One hard rule about line ends.** The opening `{` of a block must sit on the *same line*
as the header that introduces it — `mizufi lemuma () {`, never `mizufi lemuma ()` followed
by `{` on the next line. A line end closes the statement before it, so the second form is a
build failure.

**Two build-time strictnesses that are easy to trip over.** They are errors, not warnings:

* a named binding that is declared and never read afterwards fails the build;
* a module brought in by `pitudo` and never used fails the build.

Declare nothing you do not use, and bring in nothing you do not call.

## 2. Lexical rules

* **Identifiers** are letters, digits and `_`, and do not begin with a digit. Upper and lower
  letters are distinct. You choose your own identifier names freely; they are not part of
  the substitution above.
* **Whole-number literals** are written in the ordinary decimal way: `7`, `48271`,
  `2147483647`.
* **Text literals** are enclosed in double quotes: `"SUM "`, `"-"`, `""`. A backslash
  escapes the next character inside them. Text literals may not span a line end.
* **Comments** run from `//` to the end of the line, or from `/*` to `*/`.
* Whitespace outside a literal is insignificant except that a line end closes a statement
  (section 1).
* **Embedding a value inside a text literal:** (this language has no construct in this
  category — build text by joining pieces with `+`, section 5).

## 3. Declarations

A named, assignable binding is introduced with `vuzuvi`, in the order
*keyword, name, kind, `=`, initial value*:

```
vuzuvi total nebala = 0
vuzuvi label sopona = ""
```

Writing the kind is required here. A binding may be reassigned afterwards with a plain
`=` statement:

```
total = total + 1
```

A function is introduced with `mizufi`, in the order *keyword, name, parenthesised
parameter list, body*. The only function this task needs is the entry point, which has an
empty parameter list and yields no result.

## 4. Kinds used by this task

| Kind | Written | What it holds |
|---|---|---|
| whole number | `nebala` | a signed 64-bit whole number, exactly, with no rounding |
| text | `sopona` | a sequence of characters |

`nebala` arithmetic is exact at every magnitude this task produces.

## 5. Expressions and operators

| Operator | Meaning | Applies to |
|---|---|---|
| `*` | product | `nebala` |
| `%` | remainder after truncating division | `nebala` |
| `+` | sum; also joins two `sopona` values end to end | `nebala`, `sopona` |
| `-` | difference | `nebala` |
| `<` `>` | strictly less, strictly greater | `nebala` |
| `<=` `>=` | not greater, not less | `nebala` |
| `==` `!=` | equal, unequal | `nebala`, `sopona` |
| `=` | assignment; a statement, never part of an expression | both kinds |

Precedence, tightest first. Parentheses `(` `)` override it.

| Level | Operators |
|---:|---|
| 1 (tightest) | `*` `%` |
| 2 | `+` `-` |
| 3 | `<` `<=` `>` `>=` `==` `!=` |

So `a + b * c` is `a + (b * c)`, and `(a * b) % c` needs its parentheses written.

There is no separate text-joining operator: `+` between two `sopona` values produces
their concatenation.

## 6. Control flow

**Conditional branch.** `feguzi`, then a condition, then a block. The condition is *not*
parenthesised.

```
feguzi n > limit {
	limit = n
}
```

**Bounded iteration.** `lunuso`, then three clauses separated by `;` — a setup statement,
a condition tested before each pass, and a step statement run after each pass — then a
block.

```
lunuso i = 0; i < 50; i = i + 1 {
	...body...
}
```

The control binding `i` must already have been declared with `vuzuvi` before the loop;
the setup clause assigns to it. The loop runs while the condition holds.

## 7. Standard vocabulary

These names are **not** substituted; they are written exactly as shown.

| Written | Parameters | Result |
|---|---|---|
| `fmt.Println(t)` | one `sopona` value `t` | writes `t`, then a line terminator, to standard output; yields nothing |
| `strconv.FormatInt(v, 10)` | a `nebala` value `v` and the base `10` | the decimal text of `v`, as a `sopona` |

`fmt.Println` is the current spelling of the one-line output facility in this toolchain;
there is no older or alternative spelling to consider, and it writes the line terminator
itself, so do not append one. `strconv.FormatInt` is how a whole number becomes text; its
second argument is the number base, written `10`.

## 8. Modules

Each outside module is brought into scope by its own `pitudo` line, placed after the
`nemetu` line and before the declarations. The module's name is written as a text literal.

```
pitudo "fmt"
pitudo "strconv"
```

A name from a module is then written *module*`.`*name*, as in section 7. Bring in only
the modules you actually call (section 1).

## 9. Output

One line of standard output is produced by `fmt.Println(t)`, where `t` is a single `sopona`
value. The trailing line terminator is supplied by the call. Build the line first, then
write it:

```
fmt.Println("TOTAL " + strconv.FormatInt(total, 10))
```

## 10. Substituted words (condition I1)

Every word in this table replaces one grammar word of the underlying language. These are
the only substituted words; everything beyond them is either standard vocabulary
(section 7) or a name you chose yourself.

| Word | Position | What it does |
|---|---|---|
| `nemetu` | first line of the file, followed by the unit name | declares the compilation unit's name |
| `pitudo` | after the `nemetu` line, followed by a text literal | brings an outside module into scope |
| `lemuma` | after `nemetu`; also the name of the entry function | the fixed name of the top-level unit and of the entry point |
| `mizufi` | precedes a function name | introduces a function definition |
| `vuzuvi` | precedes a binding name | introduces a named, assignable binding |
| `nebala` | after a binding name, before `=` | the 64-bit signed whole-number kind |
| `sopona` | after a binding name, before `=` | the text kind |
| `lunuso` | precedes three `;`-separated clauses and a block | introduces bounded iteration |
| `feguzi` | precedes a condition and a block | introduces a conditional branch |

## 11. Worked example

A complete program. It walks a whole-number sequence, accumulates a total, tracks the
greatest element, counts the elements divisible by two, and joins the leading five
elements into one piece of text. It uses every construct described above and no more.

```
nemetu lemuma

pitudo "fmt"
pitudo "strconv"

mizufi lemuma () {
	vuzuvi state nebala = 7
	vuzuvi sum nebala = 0
	vuzuvi largest nebala = 0
	vuzuvi evens nebala = 0
	vuzuvi joined sopona = ""
	vuzuvi i nebala = 0
	lunuso i = 0; i < 50; i = i + 1 {
		state = (state * 48271) % 2147483647
		vuzuvi term nebala = state % 1000
		sum = sum + term
		feguzi term > largest {
			largest = term
		}
		feguzi term % 2 == 0 {
			evens = evens + 1
		}
		feguzi i < 5 {
			feguzi i > 0 {
				joined = joined + "-"
			}
			joined = joined + strconv.FormatInt(term, 10)
		}
	}
	fmt.Println("SUM " + strconv.FormatInt(sum, 10))
	fmt.Println("MAX " + strconv.FormatInt(largest, 10))
	fmt.Println("EVENS " + strconv.FormatInt(evens, 10))
	fmt.Println("JOINED " + joined)
}
```

It writes exactly these four lines, and ends with status 0:

```
SUM 25632
MAX 935
EVENS 24
JOINED 897-558-614-577-405
```

## 12. Size of this pack

Recorded by measurement, not by estimate: 247 lines, 8394 characters.
