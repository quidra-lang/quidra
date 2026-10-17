# Meaning-counting rules

The meaning count in MPT is **the number of distinct facts a source fragment
guarantees**, evaluated with a declared set of questions. This review does not
count lexical tokens, the number of
possible interpretations, or how preferable a particular behavior is.
A known reference relationship can earn the same point as a known independent
value relationship.

## Fix the measurement before counting

Record the following before comparing scores:

- **Visible scope:** the exact fragment and any declarations, bodies, or other
  context the reviewer may use. A standalone line, a header, and a whole program
  are different scoring units.
- **Semantic basis:** the language/version and authoritative specification;
  relevant library contracts, checker settings, and foreign-function assumptions.
- **Task and domain:** the inputs, required behavior, and permitted failures.
  A desired outcome is not evidence that hidden code guarantees it.
- **Observation point:** for example, immediately after successful initialization,
  on entry to a function, on normal return, or throughout a binding's lifetime.
- **Question set and granularity:** a finite list of properties, with fact IDs,
  selected before scoring any language.

Assume the surrounding program is complete, valid, and error-free for the stated
domain. Do not penalize an excerpt because its body or imports are omitted.
This assumption does not reveal a hidden declaration, implementation, or class
layout. A header with an undisplayed body is not an empty function.

A guarantee must follow from the visible code and the recorded semantic basis.
It must hold for the compatible implementations and inputs within the stated
scope. Record conditional guarantees as such: "on normal return" does not promise
termination, and an initial value does not promise that the value never changes.
Do not infer an enforced contract merely from a comment, a type hint, a successful
test, or the absence of observed errors.

## Counting unit and ledger

**One meaning = one distinct, testable fact whose answer is guaranteed in the
chosen scope.**

For each question, record the subject, property, observation point, answer,
evidence, and score:

| Result | Score |
| --- | ---: |
| A definite answer follows from the code and semantic basis | 1 |
| The answer depends on hidden context, or the claimed guarantee is not established | 0 |

For "is the passing model determined?", a guaranteed value model and a guaranteed
reference model both score **1**. An unknown model scores **0**. Do not subtract a
point because an argument is shared, or award a point merely because independent
values are preferred.

"Distinct" does not mean "impossible to infer from another fact." A type inferred
from a literal is as countable as an explicitly written type. Identify each fact
by its subject, property, and observation point or interval. Do not count several
descriptions of the same fact. A lifetime contract remains one fact when inspected
at several lines within that lifetime.

Use the following categories to choose questions consistently:

| Category | Examples of properties to ask about |
| --- | --- |
| Bindings and data | Result name; indexed initial values; exact sequence length |
| Computation | Operation performed; input used; comparison threshold; reduction |
| Control flow | Iteration bounds and step; condition tested; branch-selection rule |
| Types | Initial value type; declared type contract; numeric representation |
| Shape | Rank; axis extent or binding; relationship between extents |
| Values and sharing | Binding/passing model; identity of reachable mutable storage |
| State and effects | A specified write permission; guaranteed mutation or non-mutation |
| Failure | Specified bounds, range, error propagation, or exhaustiveness behavior |

The categories are not automatic points. A comparison on built-in integers can
convey an operation, an input, and a threshold as three selected facts. It does
not determine the comparison's result if the input is unknown. An overloaded
operation needs the applicable contract before its behavior can be credited.

### Keep the granularity stable

- Count a name binding once, not once per spelling or later use of the name.
- State whether a type question concerns the current value or a binding's type
  contract. Knowing a Python list's initial contents does not impose a lifetime
  element-type restriction.
- If a row counts `int[]` as one type fact, do not silently split that row into
  container kind, element type, signedness, and width for another language.
  A more detailed common question set is possible, but it is a different ledger.
- Count each selected indexed value once. If the ledger already says which value
  occupies each index, do not add another point for that same ordering.
  Exact length can be separate: six specified positions alone do not exclude a
  seventh position.
- Do not inflate an exact value such as `-3` with arbitrary weaker restatements
  such as "negative", "nonzero", and "less than ten".
- An enforced property and the mechanism enforcing that property are one fact,
  not two. Record static checking, runtime checking, or a trusted contract as
  evidence. A different specified failure outcome can be a separate property.
- Do not expand a collection-wide type constraint into another point for each
  element, or award points for redundant declarations, comments, or dead code.

## Separate passing, internal sharing, and effects

These questions concern different relationships. Select and define them
explicitly rather than treating all of them as "copying":

| Question | What a definite answer establishes |
| --- | --- |
| What is the binding or passing model? | A value binding, a reference to caller storage, or the language's defined equivalent |
| Is reachable mutable storage shared? | A specified identity or independence relationship between the source and destination, including relevant nested fields |
| What writes are permitted? | The specified state an operation is allowed to change |
| Does a write actually occur? | A write always occurs, or no write occurs, under the stated conditions |
| Is physical storage duplicated? | A specified allocation/storage fact, only if the language or applicable contract guarantees it |

Do not count terminology twice: "call by sharing" and "passing an object
reference by value" cannot supply two points for the same passing-model row.
A separately specified relationship between reachable objects may be counted;
it is not a second point merely for renaming the passing mechanism.

### Python

Python uses the same argument-passing rule for integers, lists, and other
objects: a parameter receives an object reference as a value. Assigning another
object to that parameter name does not rebind the caller's variable. Mutating a
shared mutable object can be visible to the caller. The difference between an
integer and a list is mutability, not a switch between two passing modes.

Likewise, `edited = original` binds `edited` to the same object. A known binding
model therefore earns its point even when the object's type is hidden.
Ordinary Python type annotations alone do not enforce argument or return types.

See the Python documentation on [function calls][python-calls],
[assignment][python-assignment], and [type hints][python-typing].

### C++ and Quidra

For an ordinary C++ function, an isolated `analyze(x)` does not reveal whether
its selected parameter is `T`, `T&`, or another form. A visible
`void analyze(org::Mat img)` does establish
a by-value parameter, assuming `org::Mat` denotes an ordinary class type.
That class might still contain pointers or shared buffers; passing the class by
value does not establish independence of all its internal data.

Quidra parameters without `&` receive values. Its value semantics establish
observable value behavior, not an unconditional physical-copy count. Class values
may contain reference fields, and copying such a holder preserves those fields'
targets. Consequently, `void analyze(org.Mat img)` determines value passing but
does not, with the class definition hidden, prove that all reachable data is
unshared. Check the relevant class contract before awarding that separate point.

See the C++ rules for [calls][cpp-calls] and [initialization][cpp-init], and the
[Quidra language specification][quidra-spec].

### Permission is not an actual write

A write-permission contract can establish a separate fact if it answers a
preselected question about the permitted state. Count that answer consistently,
including a contract that permits the whole specified state domain. Do not add
a point merely because generic state-change rules exist, or treat unrestricted
permission as proof that a write actually occurs.

For Quidra, an unmarked call without `&` arguments cannot write pre-existing
state covered by its mutation-marker rules. A marked `.analyze()` permits
covered writes; it does not promise that a write occurs. If the question asks
whether a call always writes or never writes that state, permission alone leaves
the answer unknown.

Name the state being measured. Quidra's marker rules exclude fresh local state
and external I/O such as printing, path-based filesystem access, process, HTTP,
environment, and time operations. Explicit `&` arguments expose their targets
for writing. The checker also trusts extern declarations. Thus "does not write
covered pre-existing state" is narrower than "has no externally observable
effects"; the latter must not receive a point from an unmarked header alone.

## Worked example: initial array

Score this fragment immediately after successful initialization:

```quidra
int[] original = [-3, 1, -2, 4, 0, 5]
```

| ID | Guaranteed fact | Score |
| --- | --- | ---: |
| A1 | The initialized binding is named `original` | 1 |
| A2 | The binding's declared type is `int[]` | 1 |
| A3 | Its first element is `-3` | 1 |
| A4 | Its second element is `1` | 1 |
| A5 | Its third element is `-2` | 1 |
| A6 | Its fourth element is `4` | 1 |
| A7 | Its fifth element is `0` | 1 |
| A8 | Its sixth element is `5` | 1 |
| A9 | Its length is exactly `6` | 1 |
| **Total** | **Nine selected facts** | **9** |

A question set omitting length scores **8**, not 9. Neither version claims that
the elements and length remain unchanged later. An initial-type question can
also be answered by an unannotated literal in another language; it must not be
silently replaced with a lifetime type-contract question.

## Worked example: standalone assignment

Now score only this line, with `edited` a fresh binding and the declaration and
value of `original` hidden:

```quidra
int[] edited = original
```

| ID | Question | Answer and evidence | Score |
| --- | --- | --- | ---: |
| B1 | Is the destination binding's name determined? | `edited` | 1 |
| B2 | What specific type contract is enforced for the destination binding? | `int[]` | 1 |
| B3 | What is the source binding's complete type contract? | Its declaration is hidden | 0 |
| B4 | Is the assignment's binding/value model determined? | A value binding, with no reference declarator | 1 |
| **Total** | | **Three of four selected facts** | **3** |

B2 and B3 ask for specific type contracts on the bindings, not merely the
current value's type or a relationship such as "the same type as the source".
For B3, include all declared union alternatives, not just the success value
consumed by the destination. In Quidra, `original` could have type
`int[] | error`: a successful value can initialize `int[]` through contextual
fail-fast handling. Assuming an error-free execution does not exclude that
declared union type.

The same four questions illustrate why known reference behavior earns a point.
For these standalone fragments, assume ordinary fresh destination bindings and
a hidden source data binding; in J, `original` is a noun.

| Language | Visible fragment | B1 | B2 | B3 | B4 | Total |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Python | `edited = original` | 1 | 0 | 0 | 1 | **2** |
| J | `edited =: original` | 1 | 0 | 0 | 1 | **2** |
| Julia | `edited = original` | 1 | 0 | 0 | 1 | **2** |
| C++ | `std::vector<int> edited = original;` | 1 | 1 | 0 | 1 | **3** |
| Quidra | `int[] edited = original` | 1 | 1 | 0 | 1 | **3** |

For B2, the untyped Python, J, and Julia destinations have no contract enforcing
a specific data type; this is why they score 0. For B3, the source declaration
is hidden and its complete contract is not established. An absent specific
contract and an unknown contract are different reasons for 0. Knowing that two
current values have the same type does not identify a specific type contract.

C++ also permits a source of another class type with a suitable conversion, so
the destination type alone does not fix the source type. A constructor's own
reference parameter does not make `edited` a reference variable. For J and
Julia, use their documented name-assignment semantics for B4; do not infer a
physical deep copy from that point. See [J assignment][j-assignment] and
[Julia assignment][julia-assignment].

These are fragment scores for the stated questions, not permanent language
scores or a claim that the full programs have identical contracts.

## Worked example: function header

Assume `org.Mat` is an ordinary class type. Its definition and the function body
are hidden. Score only this Quidra header:

```quidra
void analyze(org.Mat img)
```

| ID | Question | Answer and evidence | Score |
| --- | --- | --- | ---: |
| C1 | Is the function's declared name determined? | `analyze` | 1 |
| C2 | Is its result contract determined? | `void`: no data result on normal return | 1 |
| C3 | Is the first parameter's name determined? | `img` | 1 |
| C4 | Is the first parameter's type determined? | `org.Mat` | 1 |
| C5 | Is the parameter-passing model determined? | Value passing; no `&` | 1 |
| C6 | Is the internal sharing relationship between the parameter and the caller's argument determined at entry? | The class may contain reference fields; its layout is hidden | 0 |
| C7 | Is it guaranteed to always write or never write covered pre-existing state? | Never: no mutation marker and no `&` parameter | 1 |
| **Total** | | **Six of seven selected facts** | **6** |

C6 asks about all relevant reachable mutable storage, not just the parameter
binding. A fully specified shared, independent, or mixed relationship is a definite
answer; a hidden class layout leaves it unknown.

C7 uses the restricted state domain described above and assumes extern contracts
are honored. If the question instead includes all external I/O, that guarantee
is not established and this ledger scores **5**. Do not silently change the
meaning of "external state" between languages or comparisons.

The analogous C++ header `void analyze(org::Mat img)` scores **5** on these
seven questions: C1-C5 are determined, while its hidden class and body leave
C6-C7 unknown. A by-value declaration alone does not promise non-mutation.

## Worked example: shape-constrained tensor

For the type-and-binding fragment:

```text
tensor<real32>{nat N, N} matrix
```

| ID | Guaranteed fact | Score |
| --- | --- | ---: |
| S1 | The binding is named `matrix` | 1 |
| S2 | The element type is `real32` | 1 |
| S3 | The rank is 2 | 1 |
| S4 | The first axis size is bound to `N` | 1 |
| S5 | The second axis size equals `N` | 1 |
| **Total** | **One binding fact and four type/shape facts** | **5** |

The checker enforcing the shape relationship is evidence for S5, not a sixth
fact. If the scoring scope is only type and shape, omit S1 and report **4**.

This is a type-notation illustration. An executable declaration that binds a
previously unknown `N` also needs a suitable initializer under Quidra's rules.

## Local scores and whole-program scores

Count each fact only once within one scoring unit. Several tokens, declarations,
or proofs of the same fact do not multiply it. A language-wide rule may justify
different facts about independent operations, but restating that rule for every
line is not an additional fact.

Changing visible context requires re-evaluation. If the initial-array declaration
is visible before the assignment, B3 is now known and the assignment's four-row
ledger scores **4**. In the combined program, however, B3 repeats A2. Allocate
that type fact once, giving **9 + 4 - 1 = 12** distinct facts for the union of
these two selected ledgers.

Do not simply add scores produced for isolated fragments. For a whole-program
measurement:

1. Establish one visible scope and one fact ledger for the whole program.
2. Re-evaluate the selected questions using that scope.
3. Merge repeated facts, keeping their IDs or recording their equivalence.
4. Allocate each surviving fact to exactly one logical section.
5. Sum those disjoint section counts and the section token counts.

The whole-program numerator is the number of distinct guaranteed facts in that
whole-program ledger. A separately reported local-fragment MPT may use a
different scope, but it must be labeled and must not be added to overlapping
local scores.

## Comparing programs and reporting results

1. Fix equivalent tasks, input domains, outputs, and permitted failure behavior.
2. Choose matching logical sections and a common set of semantic questions;
   physical line counts need not match.
3. Credit explicit and inferred guarantees equally. Check the actual language
   and library contracts instead of assuming a familiar-looking syntax has the
   same meaning.
4. Record every answer with evidence and a score of 1 or 0. Explain unknowns and
   disputed guarantees rather than silently awarding them.
5. Deduplicate the whole-program ledger before calculating MPT as described in
   [the measurement procedure](README.md). Record the code and documentation
   revisions used, including any external checking configuration.

The count is a reproducible review judgment under a finite rubric, not an
absolute count of every logical consequence or an objective information measure.
Show the question set and ledger so another reviewer can reproduce or challenge
the score.

[python-calls]: https://docs.python.org/3/tutorial/controlflow.html#defining-functions
[python-assignment]: https://docs.python.org/3/reference/simple_stmts.html#assignment-statements
[python-typing]: https://docs.python.org/3/library/typing.html
[cpp-calls]: https://eel.is/c++draft/expr.call
[cpp-init]: https://eel.is/c++draft/dcl.init.general
[j-assignment]: https://www.jsoftware.com/help/dictionary/d001.htm
[julia-assignment]: https://docs.julialang.org/en/v1/manual/variables/#man-assignment-expressions
[quidra-spec]: ../docs/spec/language.md
