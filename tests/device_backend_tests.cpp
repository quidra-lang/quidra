#include "device_backend.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using quidra::device::Backend;
using quidra::device::Buffer;

using BufferPtr = std::unique_ptr<Buffer, void(*)(Buffer*)>;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "device backend test failure: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

BufferPtr buffer(int device, std::size_t bytes, std::string& error) {
    return BufferPtr(
        quidra::device::allocate(device, bytes, error),
        [](Buffer* value) { quidra::device::release(value); });
}

template <typename T>
void upload(Buffer* target, const std::vector<T>& values, std::string& error) {
    require(
        quidra::device::copy_from_host(
            target, 0, values.data(), values.size() * sizeof(T), error),
        error);
}

template <typename T>
std::vector<T> download(const Buffer* source, std::size_t count, std::string& error) {
    std::vector<T> values(count);
    require(
        quidra::device::copy_to_host(
            source, 0, values.data(), values.size() * sizeof(T), error),
        error);
    return values;
}

} // namespace

int main() {
#ifdef _WIN32
    _putenv_s("QUIDRA_TEST_FAKE_GPU_COUNT", "2");
#else
    setenv("QUIDRA_TEST_FAKE_GPU_COUNT", "2", 1);
#endif

    const auto& infos = quidra::device::devices();
    std::vector<int> test_indices;
    for (const auto& info : infos) {
        if (info.backend == Backend::Test) test_indices.push_back(info.index);
    }
    require(test_indices.size() >= 2, "expected two fake GPU devices");
    const int gpu0 = test_indices[0];
    const int gpu1 = test_indices[1];
    std::string error;
    require(quidra::device::synchronize(gpu0, error), error);
    require(quidra::device::synchronize_all(error), error);

    auto left = buffer(gpu0, 4 * sizeof(float), error);
    auto right = buffer(gpu0, 4 * sizeof(float), error);
    auto output = buffer(gpu0, 4 * sizeof(float), error);
    require(left && right && output, error);
    upload<float>(left.get(), {1.0F, 2.0F, 3.0F, 4.0F}, error);
    upload<float>(right.get(), {4.0F, 3.0F, 2.0F, 1.0F}, error);

    require(
        quidra::device::compute_binary(
            output.get(), left.get(), 0, right.get(), 0, nullptr, 0,
            10, 1, 4, error),
        error);
    const auto added = download<float>(output.get(), 4, error);
    require(added == std::vector<float>({5.0F, 5.0F, 5.0F, 5.0F}),
            "float32 binary kernel result mismatch");

    auto int_left = buffer(gpu0, sizeof(std::int8_t), error);
    auto int_output = buffer(gpu0, sizeof(std::int8_t), error);
    require(int_left && int_output, error);
    upload<std::int8_t>(int_left.get(), {127}, error);
    const std::int8_t one = 1;
    error.clear();
    require(
        !quidra::device::compute_binary(
            int_output.get(), int_left.get(), 0, nullptr, 0, &one, 2,
            2, 1, 1, error),
        "checked integer overflow unexpectedly succeeded");
    require(error.find("overflow") != std::string::npos,
            "checked integer overflow diagnostic missing");

    auto wide_float = buffer(gpu0, sizeof(double), error);
    auto narrow_float = buffer(gpu0, sizeof(float), error);
    require(wide_float && narrow_float, error);
    upload<double>(wide_float.get(), {1.0e100}, error);
    error.clear();
    require(
        !quidra::device::compute_cast(
            narrow_float.get(), wide_float.get(), 0, 9, 10, 1, error),
        "out-of-range float64 to float32 GPU cast unexpectedly succeeded");
    require(error.find("target range") != std::string::npos,
            "float narrowing range diagnostic missing");

    upload<double>(wide_float.get(), {0.1}, error);
    error.clear();
    require(
        quidra::device::compute_cast(
            narrow_float.get(), wide_float.get(), 0, 9, 10, 1, error),
        error);
    const float rounded = download<float>(narrow_float.get(), 1, error)[0];
    require(rounded > 0.099F && rounded < 0.101F,
            "float narrowing should allow deterministic precision loss");

    auto other_device = buffer(gpu1, 4 * sizeof(float), error);
    require(static_cast<bool>(other_device), error);
    upload<float>(other_device.get(), {1.0F, 1.0F, 1.0F, 1.0F}, error);
    error.clear();
    require(
        !quidra::device::compute_binary(
            output.get(), left.get(), 0, other_device.get(), 0, nullptr, 0,
            10, 1, 4, error),
        "cross-device kernel unexpectedly succeeded");
    require(!error.empty(), "cross-device kernel diagnostic missing");

    std::cout << "device backend kernels: ok\n";
    return 0;
}
