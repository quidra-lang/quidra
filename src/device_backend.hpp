#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quidra::device {

enum class DnnMode { Fast, Deterministic };

enum class Backend {
    Nvidia,
    Amd,
    Metal,
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    Test,
#endif
};

struct Info {
    int index{};
    Backend backend{Backend::Nvidia};
    int backend_index{};
    std::string name;
    std::string driver;
    std::string runtime;
};

struct Buffer;
struct Module;

struct LaunchDimensions {
    unsigned x{1};
    unsigned y{1};
    unsigned z{1};
};

const std::vector<Info>& devices();
const Info* find(int index);
std::string backend_name(Backend backend);

void set_dnn_mode(DnnMode mode);
DnnMode dnn_mode();

Buffer* allocate(int index, std::size_t bytes, std::string& error);
void release(Buffer* buffer);
bool copy_from_host(Buffer* buffer, std::size_t offset, const void* source,
                    std::size_t bytes, std::string& error);
bool copy_to_host(const Buffer* buffer, std::size_t offset, void* destination,
                  std::size_t bytes, std::string& error);
bool copy_device_to_device(Buffer* destination, std::size_t destination_offset,
                           const Buffer* source, std::size_t source_offset,
                           std::size_t bytes, std::string& error);
bool zero(Buffer* buffer, std::size_t offset, std::size_t bytes,
          std::string& error);
int buffer_device(const Buffer* buffer);

Module* load_ptx(int index, const std::string& ptx, std::string& error);
Module* load_hip_source(int index, const std::string& source, std::string& error);
void release(Module* module);
bool launch(Module* module, const char* kernel,
            LaunchDimensions grid, LaunchDimensions block,
            void** arguments, std::string& error);

// Backend-neutral tensor compute primitives. All operations keep results on the
// same gpu(n); unsupported backend/dtype combinations fail explicitly.
bool compute_fill_ones(Buffer* output, int dtype, std::size_t count,
                       std::string& error);
bool compute_gather(Buffer* output, const Buffer* source, int dtype,
                    const std::uint64_t* source_indices, std::size_t count,
                    std::string& error);
bool compute_binary(Buffer* output, const Buffer* left, std::size_t left_offset,
                    const Buffer* right, std::size_t right_offset,
                    const void* scalar, int scalar_side, int dtype,
                    int operation, std::size_t count, std::string& error);
bool compute_unary(Buffer* output, const Buffer* input, std::size_t input_offset,
                   int dtype, int operation, std::size_t count,
                   std::string& error);
bool compute_cast(Buffer* output, const Buffer* input, std::size_t input_offset,
                  int source_dtype, int target_dtype, std::size_t count,
                  std::string& error);
bool compute_matmul(Buffer* output, const Buffer* left, const Buffer* right,
                    int dtype, std::size_t m, std::size_t k, std::size_t n,
                    std::string& error);
bool compute_dot(const Buffer* left, const Buffer* right, int dtype,
                 std::size_t count, void* host_result, std::string& error);
bool compute_reduce(const Buffer* input, int dtype, int operation,
                    std::size_t count, void* host_result, std::string& error);
bool compute_mean(const Buffer* input, int dtype, std::size_t count,
                  double& result, std::string& error);

bool compute_mean_to(Buffer* output, const Buffer* input, int dtype,
                     std::size_t count, std::string& error);
bool compute_last_reduce_broadcast(Buffer* output, const Buffer* input, int dtype,
                                   std::size_t count, std::size_t width,
                                   int operation, std::string& error);
bool compute_affine(Buffer* output, const Buffer* input, const Buffer* weight,
                    const Buffer* bias, int dtype, std::size_t batches,
                    std::size_t features_in, std::size_t features_out,
                    std::string& error);
bool compute_conv2d(Buffer* output, const Buffer* input, const Buffer* weight,
                    const Buffer* bias, int dtype,
                    std::size_t batches, std::size_t channels_in,
                    std::size_t height, std::size_t width,
                    std::size_t channels_out, std::size_t kernel_h,
                    std::size_t kernel_w, std::size_t output_h,
                    std::size_t output_w, std::size_t stride,
                    std::size_t padding, std::string& error);
bool compute_normalize_inference(
    Buffer* output, const Buffer* input, const Buffer* scale,
    const Buffer* bias, const Buffer* mean, const Buffer* variance,
    int dtype, std::size_t count, std::size_t features,
    std::size_t inner, double epsilon, std::string& error);

bool compute_abs_backward(Buffer* output, const Buffer* gradient,
                          const Buffer* input, int dtype,
                          std::size_t count, std::string& error);
bool compute_mean_backward(Buffer* output, const Buffer* gradient_scalar,
                           int dtype, std::size_t count, std::string& error);
bool compute_max_last_backward(Buffer* output, const Buffer* gradient,
                               const Buffer* input, int dtype,
                               std::size_t count, std::size_t width,
                               std::string& error);
bool compute_affine_backward(
    Buffer* input_gradient, Buffer* weight_gradient, Buffer* bias_gradient,
    const Buffer* gradient, const Buffer* input, const Buffer* weight,
    int dtype, std::size_t batches, std::size_t features_in,
    std::size_t features_out, std::string& error);

bool compute_conv2d_backward(
    Buffer* input_gradient, Buffer* weight_gradient, Buffer* bias_gradient,
    const Buffer* gradient, const Buffer* input, const Buffer* weight,
    int dtype, std::size_t batches, std::size_t channels_in,
    std::size_t height, std::size_t width, std::size_t channels_out,
    std::size_t kernel_h, std::size_t kernel_w, std::size_t output_h,
    std::size_t output_w, std::size_t stride, std::size_t padding,
    std::string& error);
bool compute_normalize_training(
    Buffer* output, Buffer* cache, Buffer* running_mean, Buffer* running_variance,
    const Buffer* input, const Buffer* scale, const Buffer* bias,
    int dtype, std::size_t count, std::size_t features, std::size_t inner,
    std::size_t samples, double momentum, double epsilon, std::string& error);
bool compute_normalize_backward(
    Buffer* input_gradient, Buffer* scale_gradient, Buffer* bias_gradient,
    const Buffer* gradient, const Buffer* input, const Buffer* scale,
    const Buffer* cache, int dtype, std::size_t count, std::size_t features,
    std::size_t inner, std::size_t samples, std::string& error);
bool compute_random_mask(
    Buffer* output, Buffer* mask, const Buffer* input, int dtype,
    std::size_t count, std::uint64_t initial_state, std::uint64_t cutoff,
    double scale, std::string& error);
bool compute_moment_update(
    Buffer* parameter, const Buffer* gradient, Buffer* first, Buffer* second,
    int dtype, std::size_t count, double rate, double beta1, double beta2,
    double epsilon, double correction1, double correction2,
    std::string& error);

bool compute_image_geometry(
    Buffer* output, const Buffer* input, int dtype,
    std::size_t channels, std::size_t input_height, std::size_t input_width,
    std::size_t output_height, std::size_t output_width,
    int operation, std::size_t parameter0, std::size_t parameter1,
    std::string& error);
bool compute_image_grayscale(
    Buffer* output, const Buffer* input, std::size_t channels,
    std::size_t height, std::size_t width, std::string& error);
bool compute_image_threshold(
    Buffer* output, const Buffer* input, std::size_t count,
    std::uint8_t cutoff, std::uint8_t low, std::uint8_t high,
    std::string& error);
bool compute_image_blur(
    Buffer* output, const Buffer* input, std::size_t channels,
    std::size_t height, std::size_t width, std::size_t radius,
    std::string& error);
bool compute_image_filter(
    Buffer* output, const Buffer* input, const Buffer* kernel,
    std::size_t channels, std::size_t height, std::size_t width,
    std::size_t kernel_height, std::size_t kernel_width,
    std::int64_t divisor, std::int64_t offset, std::string& error);
bool compute_image_morphology(
    Buffer* output, const Buffer* input, int dtype,
    std::size_t channels, std::size_t height, std::size_t width,
    std::size_t radius, bool dilate, std::string& error);

} // namespace quidra::device
