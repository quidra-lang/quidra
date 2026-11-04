#!/usr/bin/env bash
set -euo pipefail
QUIDRA="$1"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
export QUIDRA_CACHE_DIR="$TMP/cache"
cat > "$TMP/copies.template" <<'QUI'
cli args
    int64 device = argument(default = -1)
class Leaf this()
    private autograd.Target target := autograd.target()

    return
    tensor<@TYPE@> .tracked(tensor<@TYPE@> value)
        return value.track(&this.target)
    bool ready()
        return this.target.has_grad()
    @TYPE@ gradient()
        return this.target.gradient<@TYPE@>().cpu().item()
    void .clear()
        .this.target.clear_grad()
class Model()
    Leaf[] leaves := [Leaf(), Leaf()]

    return
tensor<@TYPE@> raw := tensor.ones<@TYPE@>([])
if args.device >= 0
    raw = raw.gpu(nat(args.device))
autograd.Target original := autograd.target()
autograd.Target before := original
tensor<@TYPE@> x := raw.track(&original)
tensor<@TYPE@> loss := x * x
loss.backward(&original)
print(original.has_grad() and not before.has_grad())
print(NL)
autograd.Target after := original
print(after.has_grad() and after.gradient<@TYPE@>().cpu().item() == @TYPE@(2.0))
print(NL)
print(after.gradient<@TYPE@>().device() == int(args.device))
print(NL)
.after.clear_grad()
print(original.has_grad() and not after.has_grad())
print(NL)
loss.backward(&after)
print(not after.has_grad())
print(NL)
tensor<@TYPE@> fresh := raw.track(&after)
(fresh * fresh).backward(&after)
.original.clear_grad()
print(not original.has_grad() and after.has_grad())
print(NL)
Model model := Model()
tensor<@TYPE@> left := .model.leaves[0].tracked(raw)
tensor<@TYPE@> right := .model.leaves[1].tracked(raw)
(left * right).backward(&model)
print(model.leaves[0].ready() and model.leaves[1].ready())
print(NL)
Model copy := model
.copy.leaves[0].clear()
print(model.leaves[0].ready() and not copy.leaves[0].ready())
print(NL)
.copy.leaves[1].clear()
(left * right).backward(&copy)
print(not copy.leaves[0].ready() and not copy.leaves[1].ready())
print(NL)
Leaf[] leaves_copy := model.leaves
.leaves_copy[0].clear()
print(model.leaves[0].ready() and not leaves_copy[0].ready() and leaves_copy[1].gradient() == @TYPE@(1.0))
print(NL)
QUI
for dtype in real16 real16b real32 real64; do
    sed "s/@TYPE@/$dtype/g" "$TMP/copies.template" > "$TMP/$dtype.qui"
    "$QUIDRA" build "$TMP/$dtype.qui" -o "$TMP/$dtype"
    for i in {1..10}; do printf 'true\n'; done > "$TMP/expected"
    "$TMP/$dtype" -1 > "$TMP/cpu.actual"
    cmp "$TMP/expected" "$TMP/cpu.actual"
    if QUIDRA_TEST_FAKE_GPU_COUNT=1 "$QUIDRA" gpu 2>/dev/null | grep -Fq 'backend: TEST'; then
        QUIDRA_TEST_FAKE_GPU_COUNT=1 "$TMP/$dtype" 0 > "$TMP/fake.actual"
        cmp "$TMP/expected" "$TMP/fake.actual"
    fi
    if "$QUIDRA" gpu 2>/dev/null | grep -Fq 'GPU 0'; then
        if [[ "$dtype" == real64 ]] && "$QUIDRA" gpu 2>/dev/null | grep -Fq 'backend: Metal'; then
            # Metal stores and transfers real64, but its compute kernels
            # explicitly refuse binary64 rather than narrow the value.
            status=0
            "$TMP/$dtype" 0 > "$TMP/device.actual" 2> "$TMP/device.err" || status=$?
            [[ "$status" == 101 && ! -s "$TMP/device.actual" ]]
            grep -Fq 'runtime error[TENSOR]' "$TMP/device.err"
            grep -Fq 'tensor arithmetic is not supported on Metal for tensor dtype 9' "$TMP/device.err"
        else
            "$TMP/$dtype" 0 > "$TMP/device.actual"
            cmp "$TMP/expected" "$TMP/device.actual"
        fi
    fi
done
printf 'Target copy independence cases passed\n'
