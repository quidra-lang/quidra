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

    float reduced = 0.0F;
    require(
        quidra::device::compute_reduce(
            output.get(), 10, 1, 4, &reduced, error),
        error);
    require(reduced == 20.0F, "reduction kernel result mismatch");

    double mean = 0.0;
    require(quidra::device::compute_mean(output.get(), 10, 4, mean, error), error);
    require(mean == 5.0, "mean kernel result mismatch");

    auto matrix_left = buffer(gpu0, 4 * sizeof(float), error);
    auto matrix_right = buffer(gpu0, 4 * sizeof(float), error);
    auto matrix_output = buffer(gpu0, 4 * sizeof(float), error);
    require(matrix_left && matrix_right && matrix_output, error);
    upload<float>(matrix_left.get(), {1.0F, 2.0F, 3.0F, 4.0F}, error);
    upload<float>(matrix_right.get(), {1.0F, 0.0F, 0.0F, 1.0F}, error);
    require(
        quidra::device::compute_matmul(
            matrix_output.get(), matrix_left.get(), matrix_right.get(),
            10, 2, 2, 2, error),
        error);
    require(
        download<float>(matrix_output.get(), 4, error) ==
            std::vector<float>({1.0F, 2.0F, 3.0F, 4.0F}),
        "matmul kernel result mismatch");

    auto affine_input = buffer(gpu0, 2 * sizeof(float), error);
    auto affine_weight = buffer(gpu0, 2 * sizeof(float), error);
    auto affine_bias = buffer(gpu0, sizeof(float), error);
    auto affine_output = buffer(gpu0, sizeof(float), error);
    require(affine_input && affine_weight && affine_bias && affine_output, error);
    upload<float>(affine_input.get(), {1.0F, 1.0F}, error);
    upload<float>(affine_weight.get(), {2.0F, 3.0F}, error);
    upload<float>(affine_bias.get(), {4.0F}, error);
    require(
        quidra::device::compute_affine(
            affine_output.get(), affine_input.get(), affine_weight.get(),
            affine_bias.get(), 10, 1, 2, 1, error),
        error);
    require(download<float>(affine_output.get(), 1, error)[0] == 9.0F,
            "affine kernel result mismatch");

    auto image_input = buffer(gpu0, 4, error);
    auto image_output = buffer(gpu0, 4, error);
    require(image_input && image_output, error);
    upload<std::uint8_t>(image_input.get(), {0, 7, 8, 255}, error);
    require(
        quidra::device::compute_image_threshold(
            image_output.get(), image_input.get(), 4, 8, 1, 9, error),
        error);
    require(
        download<std::uint8_t>(image_output.get(), 4, error) ==
            std::vector<std::uint8_t>({1, 1, 9, 9}),
        "image threshold kernel result mismatch");

    auto parameter = buffer(gpu0, sizeof(float), error);
    auto gradient = buffer(gpu0, sizeof(float), error);
    auto first = buffer(gpu0, sizeof(float), error);
    auto second = buffer(gpu0, sizeof(float), error);
    require(parameter && gradient && first && second, error);
    upload<float>(parameter.get(), {1.0F}, error);
    upload<float>(gradient.get(), {2.0F}, error);
    upload<float>(first.get(), {0.0F}, error);
    upload<float>(second.get(), {0.0F}, error);
    require(
        quidra::device::compute_moment_update(
            parameter.get(), gradient.get(), first.get(), second.get(),
            10, 1, 0.1, 0.9, 0.999, 1.0e-8, 0.1, 0.001, error),
        error);
    const float updated = download<float>(parameter.get(), 1, error)[0];
    require(updated > 0.899F && updated < 0.901F,
            "moment-update kernel result mismatch");

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
