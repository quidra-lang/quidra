#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "quidra/native_extension.h"
#include "runtime_counters.hpp"
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
using quidra::counters::Id;
struct Tensor {
    void* value{};
    explicit Tensor(void* raw) : value(raw) {}
    ~Tensor() { quidra_tensor_drop(value); }
    Tensor(const Tensor&) = delete;
    Tensor& operator=(const Tensor&) = delete;
};
std::uint64_t saves() { return quidra::counters::value(Id::AutogradSavedTensors); }
void delta(std::uint64_t before, std::uint64_t expected, const char* reason) {
    if (saves() - before != expected) {
        std::fprintf(stderr, "%s: saved delta=%llu expected=%llu\n", reason,
            static_cast<unsigned long long>(saves() - before), static_cast<unsigned long long>(expected));
        throw std::runtime_error("saved tensor count mismatch");
    }
}
int unused_backward(const void* const*, std::uint64_t, const void*, void* const*,
                    std::uint64_t, const void*, std::uint64_t) { return 0; }
}
int main(int argc, char** argv) {
    try {
        const bool gpu = argc == 2 && std::string_view(argv[1]) == "--fake";
        if (argc > 2 || (argc == 2 && !gpu)) return 2;
        quidra_runtime_set_args(argc, argv);
        quidra::counters::set_enabled(true);
        quidra::counters::reset();
        long long shape[]{1, 2};
        Tensor value(quidra_tensor_create(shape, QCORE_DTYPE_FLOAT32,
            quidra::abi::tensor_fill_mode::ones, gpu, 0, 1, 1));
        auto before = saves();
        Tensor tracked(quidra_tensor_track(value.value, 2, 1));
        Tensor squared(quidra_tensor_binary(tracked.value, tracked.value, nullptr,
            quidra::abi::scalar_side::none, quidra::abi::tensor_binary_opcode::multiply, 3, 1));
        delta(before, 2, "leaf and binary values, repeated parent");
        before = saves();
        float scalar = 2;
        Tensor scaled(quidra_tensor_binary(tracked.value, nullptr, &scalar,
            quidra::abi::scalar_side::right, quidra::abi::tensor_binary_opcode::multiply, 4, 1));
        delta(before, 1, "scalar metadata is not a second saved tensor");
        before = saves();
        Tensor same(quidra_tensor_track(tracked.value, 5, 1));
        Tensor cloned(quidra_tensor_clone(tracked.value));
        delta(before, 0, "existing graph references are not new saves");
        Tensor retracked(quidra_tensor_retrack(tracked.value, 6, 1));
        delta(before, 1, "retrack retains a fresh leaf value");
        before = saves();
        long long view_shape[]{2, 1, 2};
        Tensor reshaped(quidra_tensor_reshape(tracked.value, view_shape, 7, 1));
        Tensor transposed(quidra_tensor_transpose(reshaped.value, 0, 1, 8, 1));
        delta(before, 2, "shared view snapshots");
        before = saves();
        long long empty_shape[]{1, 0}, empty_view_shape[]{2, 0, 1};
        Tensor empty(quidra_tensor_create(empty_shape, QCORE_DTYPE_FLOAT32,
            quidra::abi::tensor_fill_mode::ones, gpu, 0, 9, 1));
        Tensor empty_tracked(quidra_tensor_track(empty.value, 10, 1));
        Tensor empty_view(quidra_tensor_reshape(empty_tracked.value, empty_view_shape, 11, 1));
        delta(before, 2, "zero-element logical snapshots");
        before = saves();
        Tensor custom(quidra_tensor_untrack(tracked.value, 12, 1));
        const void* inputs[]{tracked.value};
        const void* saved[]{tracked.value, tracked.value};
        if (qcore_tensor_attach_custom_autograd_with_saved(custom.value, inputs, 1, saved, 2,
                unused_backward, nullptr, 0) != 0) throw std::runtime_error("custom attachment failed");
        delta(before, 3, "custom output plus explicit snapshots");
        before = saves();
        Tensor failed(quidra_tensor_untrack(tracked.value, 13, 1));
        const void* bad_saved[]{tracked.value, nullptr};
        if (qcore_tensor_attach_custom_autograd_with_saved(failed.value, inputs, 1, bad_saved, 2,
                unused_backward, nullptr, 0) != -5) throw std::runtime_error("invalid custom save accepted");
        delta(before, 1, "failed attachment counts only completed explicit retention");
        before = saves();
        long long scalar_shape[]{0};
        Tensor initial(quidra_tensor_create(scalar_shape, QCORE_DTYPE_FLOAT32,
            quidra::abi::tensor_fill_mode::ones, gpu, 0, 14, 1));
        Tensor leaf(quidra_tensor_track(initial.value, 15, 1));
        Tensor loss(quidra_tensor_binary(leaf.value, leaf.value, nullptr,
            quidra::abi::scalar_side::none, quidra::abi::tensor_binary_opcode::multiply, 16, 1));
        delta(before, 2, "scalar forward snapshots");
        before = saves();
        void* targets[]{leaf.value}; unsigned char kinds[]{0};
        quidra_tensor_backward_many(loss.value, targets, kinds, 1, false, 17, 1);
        delta(before, 0, "ordinary backward temporaries are not saved tape tensors");
        quidra_tensor_clear_grad(leaf.value, 18, 1);
        quidra_tensor_backward_many(loss.value, targets, kinds, 1, true, 19, 1);
        delta(before, 5, "tracked seed, two products and gradient sum");
        before = saves();
        quidra::counters::set_enabled(false);
        Tensor off(quidra_tensor_retrack(tracked.value, 20, 1));
        delta(before, 0, "disabled observer");
        std::puts("runtime logical saved tensor snapshots: ok");
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
