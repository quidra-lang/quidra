# Standard borrowed Iterator

The approved API is the standard generic class `Iterator<T>` with
`bool .operator()(T &value)`. A successful call initializes or replaces the
output with the next element; exhaustion returns false without changing the
output. Each iterator owns its cursor and borrows its source array.

`Iterator(&values)` infers the element type from the array. Type positions,
including a user method's `Iterator<int> iterator()` result, require explicit
generic arguments. The factory returns a fresh independent cursor.

A single value collection loop `for value := object[]` calls `object.iterator()`
once, then calls the resulting iterator until false. Its body sees initialized
element values without a dummy/default initializer. Preserve break, continue,
empty iteration, exact element types and existing array/bin/range behavior.
The iterator method is not an index operator; ordinary `object[index]` remains
independent. Reference iteration needs a different contract and is not added.

Borrowed source storage must remain valid. Reject iterator returns that borrow
a function-local array. Prevent source replacement or length changes from
silently invalidating active iteration. Ordinary reference fields retain their
existing owning-storage semantics.

Implement and push directly on develop. Update compiler tests, documentation,
examples and affected first-party tooling; introduce no unrelated changes.
