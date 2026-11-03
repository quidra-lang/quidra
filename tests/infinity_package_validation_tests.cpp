#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "quidra/native_extension.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

// A legacy finite-preserving declaration is NOT a guarantee for infinite inputs.
int main(int argc, char** argv) {
    const bool legacy_nan = argc == 2 && std::string_view(argv[1]) == "legacy-nan";
    const bool normal_nan = argc == 2 && std::string_view(argv[1]) == "normal-nan";
    const char* symbol = "infinity.test.package";
    const auto flags = legacy_nan ? QCORE_OP_FINITE_PRESERVING : 0u;
    if (qcore_op_register_flags(symbol, flags) != 0) return 2;
    long long shape[]{1, 1};
    void* tensor = quidra_tensor_create(shape, QCORE_DTYPE_FLOAT32,
        quidra::abi::tensor_fill_mode::ones, false, 0, 1, 1);
    auto* data = static_cast<float*>(qcore_tensor_cpu_data(tensor));
    if (!data) return 2;
    data[0] = legacy_nan || normal_nan ? std::numeric_limits<float>::quiet_NaN()
                                     : std::numeric_limits<float>::infinity();
    if (legacy_nan || normal_nan) { std::fputs("before", stdout); std::fflush(stdout); }
    quidra_tensor_validate_package(tensor, symbol, "infinity.test", 1, 1);
    quidra_managed_release(tensor, reinterpret_cast<void*>(&quidra_tensor_drop));
    if (legacy_nan || normal_nan) return 3; // Must terminate with status 101 before here.
    if (qcore_op_register_flags("infinity.test.copy", QCORE_OP_NAN_PRESERVING) != 0) return 4;
    if (qcore_op_register_flags("infinity.test.invalid", 0x80000000u) == 0) return 5;
    std::puts("Infinity package validation: ok");
    return 0;
}
