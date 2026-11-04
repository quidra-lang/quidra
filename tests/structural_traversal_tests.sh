#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/order.qui" <<'QUI'
class Pair()
    private int first := 1
    int[] rest := [2, 3]

    return
class Model()
    Pair left := Pair()
    (int, int[]) right := (4, [5, 6])

    return
reflect.Each<int> integers<M>(M &model)
    return reflect.each<int>(&model)
Model model := Model()
for: int &value := &integers(&model)[i]
    print("{i}:{value}{NL}")
    value = value + 10
for value := reflect.each<int>(model)[]
    print("{value}{NL}")
for path := reflect.each_paths<int>(model)[]
    print(path + NL)
QUI
cat > "$TMP/order.expected" <<'OUT'
0:1
1:2
2:3
3:4
4:5
5:6
11
12
13
14
15
16
left.first
left.rest[0]
left.rest[1]
right.0
right.1[0]
right.1[1]
OUT
"$QUIDRA" build "$TMP/order.qui" -o "$TMP/order"
"$TMP/order" > "$TMP/order.actual"
cmp "$TMP/order.expected" "$TMP/order.actual"
cat > "$TMP/inference.qui" <<'QUI'
class Slot<T> this(local T initial)
    T value

    this.value = initial
reflect.Each<Slot<T>> slots<T, M>(M &model)
    return reflect.each<Slot<T>>(&model)
class Model()
    private Slot<real32> first := Slot<real32>(2.0)
    Slot<real32>[] rest := [Slot<real32>(3.0)]

    return
Model model := Model()
for: Slot<real32> &slot := &slots(&model)[i]
    print("{i} {slot.value == real32(2.0) or slot.value == real32(3.0)}{NL}")
QUI
"$QUIDRA" build "$TMP/inference.qui" -o "$TMP/inference"
"$TMP/inference" > "$TMP/inference.actual"
printf '0 true\n1 true\n' > "$TMP/inference.expected"
cmp "$TMP/inference.expected" "$TMP/inference.actual"
cat > "$TMP/control.qui" <<'QUI'
int[][] values := [[1, 2], [3, 4]]
for: int &value := &reflect.each<int>(&values)[i]
    if i == 0
        continue
    value = value + 10
    if i == 2
        break
print("{values[0][0]} {values[0][1]} {values[1][0]} {values[1][1]}{NL}")
for _ = reflect.each<string>(values)[]
    print("unreachable" + NL)
QUI
"$QUIDRA" build "$TMP/control.qui" -o "$TMP/control"
"$TMP/control" > "$TMP/control.actual"
printf '1 12 13 4\n' > "$TMP/control.expected"
cmp "$TMP/control.expected" "$TMP/control.actual"
cat > "$TMP/binding.qui" <<'QUI'
class Model()
    int[] first := [1, 2]
    int[] second := [3, 4]

    return
reflect.Each<int> second(int ignored, Model &model)
    return reflect.each<int>(&model.second)
reflect.Each<int> copied(Model model)
    return reflect.each<int>(&model.first)
Model model := Model()
for int &value := &second(99, &model)[]
    model.first = [10]
    value = value + 20
for int &value := &copied(model)[]
    value = 80
print("{model.first[0]} {model.second[0]} {model.second[1]}{NL}")
QUI
"$QUIDRA" build "$TMP/binding.qui" -o "$TMP/binding"
"$TMP/binding" > "$TMP/binding.actual"
printf '10 23 24\n' > "$TMP/binding.expected"
cmp "$TMP/binding.expected" "$TMP/binding.actual"
cat > "$TMP/pattern.qui" <<'QUI'
class Model()
    (int, int) first := (1, 2)
    (int, int)[] rest := [(3, 4), (5, 6)]

    return
Model model := Model()
for: (int &left, int &right) := &reflect.each<(int, int)>(&model)[i]
    left = left + 10
    right = right + 20
    if i == 1
        break
for (int left, int right) := reflect.each<(int, int)>(model)[]
    print("{left} {right}{NL}")
QUI
"$QUIDRA" build "$TMP/pattern.qui" -o "$TMP/pattern"
"$TMP/pattern" > "$TMP/pattern.actual"
printf '11 22\n13 24\n5 6\n' > "$TMP/pattern.expected"
cmp "$TMP/pattern.expected" "$TMP/pattern.actual"
cat > "$TMP/reference-root.qui" <<'QUI'
class Model()
    int[] values := [1, 2, 3]

    return
class Holder this(local Model &model_initial)
    Model &model
    int own := 9

    &this.model = &model_initial
int first(Holder &holder)
    for int &value := &reflect.each<int>(&holder.model.values)[]
        return value
    return 0
Model model := Model()
Holder holder := Holder(&model)
for: int &value := &reflect.each<int>(&holder.model)[i]
    if i == 0
        continue
    value = value + 10
    break
print("{first(&holder)}{NL}")
for int &value := &reflect.each<int>(&holder.model.values)[]
    value = value + 1
for int &value := &reflect.each<int>(&holder)[]
    value = value + 100
print("{model.values[0]} {model.values[1]} {model.values[2]} {holder.own}{NL}")
QUI
"$QUIDRA" build "$TMP/reference-root.qui" -o "$TMP/reference-root"
"$TMP/reference-root" > "$TMP/reference-root.actual"
printf '1\n2 13 4 109\n' > "$TMP/reference-root.expected"
cmp "$TMP/reference-root.expected" "$TMP/reference-root.actual"
cat > "$TMP/reference-escape.qui" <<'QUI'
class Model()
    int[] values := [1, 2, 3]

    return
class Holder this(local Model &model_initial)
    Model &model

    &this.model = &model_initial
reflect.Each<M> whole<M>(M &root)
    return reflect.each<M>(&root)
Holder from_named()
    Model model := Model()
    for Model &value := &whole(&model)[]
        return Holder(&value)
    return Holder(&model)
Holder from_direct()
    Model model := Model()
    for Model &value := &reflect.each<Model>(&model)[]
        return Holder(&value)
    return Holder(&model)
Holder named := from_named()
Holder direct := from_direct()
named.model.values[0] = 7
print("{named.model.values[0]} {named.model.values[1]} {named.model.values[2]}{NL}")
print("{direct.model.values[0]} {direct.model.values[1]} {direct.model.values[2]}{NL}")
QUI
"$QUIDRA" build "$TMP/reference-escape.qui" -o "$TMP/reference-escape"
"$TMP/reference-escape" > "$TMP/reference-escape.actual"
printf '7 2 3\n1 2 3\n' > "$TMP/reference-escape.expected"
cmp "$TMP/reference-escape.expected" "$TMP/reference-escape.actual"
cat > "$TMP/reference-member-traversal.qui" <<'QUI'
class Leaf()
    private autograd.Target state := autograd.target()
    void .clear()
        .state.clear_grad()
    bool has_grad()
        return state.has_grad()
class Model()
    Leaf value := Leaf()
reflect.Each<Leaf> leaves<M>(M &model)
    return reflect.each<Leaf>(&model)
class Holder self(local Model &initial)
    Model &model
    &model = &initial
    void .clear_direct()
        for Leaf &leaf := &reflect.each<Leaf>(&model)[]
            .leaf.clear()
    void .clear_named()
        for Leaf &leaf := &leaves(&model)[]
            .leaf.clear()
Model model := Model()
Holder holder := Holder(&model)
.holder.clear_direct()
.holder.clear_named()
print("{model.value.has_grad()}{NL}")
QUI
"$QUIDRA" build "$TMP/reference-member-traversal.qui" -o "$TMP/reference-member-traversal"
"$TMP/reference-member-traversal" > "$TMP/reference-member-traversal.actual"
printf 'false\n' > "$TMP/reference-member-traversal.expected"
cmp "$TMP/reference-member-traversal.expected" "$TMP/reference-member-traversal.actual"
cat > "$TMP/reference-incomplete-target.qui" <<'QUI'
class Item()
    int value
class Holder(local Item &initial)
    Item &item := &initial
Item item := Item()
Holder holder := Holder(&item)
QUI
cat > "$TMP/reference-unbound-target.qui" <<'QUI'
class Item()
    int value := 1
class Holder()
    Item &item
Holder holder := Holder()
print(holder.item.value)
QUI
for case in reference-incomplete-target reference-unbound-target; do
    if "$QUIDRA" check "$TMP/$case.qui" --json > "$TMP/$case.json"; then exit 1; fi
    python3 - "$TMP/$case.json" "$case" <<'PYREF'
import json,sys
expected='UNINITIALIZED' if sys.argv[2]=='reference-incomplete-target' else 'UNBOUND_REFERENCE'
assert any(d['code']==expected for d in json.load(open(sys.argv[1]))['diagnostics'])
PYREF
done
# Binding initialization does not prove collection element initialization.
cat > "$TMP/element-read.qui" <<'QUI'
int[] values := array(2)
for &value := &values[]
    print(value)
QUI
cat > "$TMP/pattern-read.qui" <<'QUI'
(int, int)[] values := array(2)
for (int &left, int &right) := &values[]
    print(left)
QUI
cat > "$TMP/traversal-read.qui" <<'QUI'
int[] values := array(2)
for int &value := &reflect.each<int>(&values)[]
    print(value)
QUI
for case in element-read pattern-read traversal-read; do
    "$QUIDRA" build "$TMP/$case.qui" -o "$TMP/$case"
    set +e
    "$TMP/$case" > "$TMP/$case.out" 2> "$TMP/$case.err"
    status=$?
    set -e
    [ "$status" -eq 101 ]
    grep -q 'runtime error\[UNINITIALIZED\]' "$TMP/$case.err"
done
printf 'structural traversal cases passed\n'
