#include <quidra/native_extension.h>
#include <cstdint>
#include <limits>

extern "C" std::int64_t qtest_tensor_array_count(const void* values) {
    return static_cast<std::int64_t>(qcore_tensor_array_count(values));
}
extern "C" std::int32_t qtest_tensor_array_bump(void* values, std::int32_t mode) {
    const auto count = qcore_tensor_array_count(values);
    for (std::uint64_t index = 0; index < count; ++index) {
        auto* value = qcore_tensor_array_element(values, index);
        if (qcore_tensor_dtype(value) != QCORE_DTYPE_FLOAT32) return 1;
        auto* data = static_cast<float*>(qcore_tensor_cpu_data(value));
        if (!data) return 1;
        for (std::uint64_t element = 0; element < qcore_tensor_element_count(value); ++element)
            data[element] = mode == 1 ? std::numeric_limits<float>::quiet_NaN() :
                            mode == 2 ? std::numeric_limits<float>::infinity() : data[element] + 1.0f;
    }
    return 0;
}
extern "C" std::int32_t qtest_tensor_array_readonly(const void* values) {
    (void)qcore_tensor_array_element(const_cast<void*>(values), 0);
    return 0;
}
extern "C" std::int32_t qtest_tensor_array_bounds(const void* values) {
    (void)qcore_tensor_array_element_const(values, qcore_tensor_array_count(values));
    return 0;
}
extern "C" std::int32_t qtest_tensor_array_wrong_handle(const void* tensor) {
    (void)qcore_tensor_array_count(tensor);
    return 0;
}
