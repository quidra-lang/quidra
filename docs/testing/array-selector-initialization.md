# Initialization safety for array selection

Array gathering and boolean masking read every element of the selector. Knowing
its length does not establish that its elements have values. For example:

```quidra
int[2] values = [10, 20]
int[2] indices
int[] selected = values[indices] // UNINITIALIZED: no index has a value
```

The same rule applies to `nat` indices and `bool` masks. A tracked fixed-length
selector with even one element that has never been initialized is rejected at
compile time. Fully initialized selectors remain valid; gathering preserves
index order and duplicates, and masking preserves source order.

This uses the existing array initialization policy rather than introducing a
new reference-effect rule. When an alias or reference argument makes element
initialization unknown to the checker, the generated element reads retain
runtime initialization checks. Reading an uninitialized index or mask element
must stop with `UNINITIALIZED`, not silently behave as zero or false. The source
array's existing initialization checks are unchanged.

Empty selectors read no elements. `values.indices()` reads length metadata, not
the values stored in the array, so it remains valid for allocated uninitialized
storage.

## Regression commands

```sh
cmake --build build --target quidra quidra_array_selector_initialization_tests
ctest --test-dir build --output-on-failure -R 'quidra_array_selector_(initialization_tests|runtime)$'
```

The C++ suite uses the production lexer, parser and checker to cover 35 accepted
and rejected programs. The Python suite checks then executes 18 native programs,
covering aliased and reference-parameter selectors before, during and after
initialization. Runtime-negative cases require the exact failure category and
exit status after an observable prefix, so a compiler failure cannot pass them.
