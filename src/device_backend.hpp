#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quidra::device {

enum class DnnMode { Fast, Deterministic };

// Members are named after the canonical backend ids in project.toml rather than
// after the vendors, so the identifier a human reads here is the same one that
// appears in machine-readable metadata. `backend_display_name` holds the
// separate, human-facing spelling.
enum class Backend {
    Cuda,
    Hip,
    Metal,
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    Test,
#endif
};

struct Info {
    int index{};
    Backend backend{Backend::Cuda};
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

// Human-facing name, matching the [[backends]] display value in project.toml.
std::string backend_display_name(Backend backend);

void set_dnn_mode(DnnMode mode);
DnnMode dnn_mode();

bool synchronize(int index, std::string& error);
bool synchronize_all(std::string& error);

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
bool compute_gather_backward(Buffer* input_gradient, const Buffer* output_gradient,
                             int dtype, const std::uint64_t* source_indices,
                             std::size_t source_count, std::size_t output_count,
                             std::string& error);
bool compute_compare_all(const Buffer* left, std::size_t left_offset,
                         const Buffer* right, std::size_t right_offset,
                         int dtype, int operation, std::size_t count,
                         bool& result, std::string& error);
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
bool compute_binary_backward(
    Buffer* left_gradient, Buffer* right_gradient,
    const Buffer* gradient, const Buffer* left, const Buffer* right,
    int dtype, int operation, std::size_t count, std::string& error);
bool compute_scalar_backward(
    Buffer* output, const Buffer* gradient, const Buffer* input,
    int dtype, int operation, bool scalar_left, double scalar,
    std::size_t count, std::string& error);
bool compute_abs_backward(Buffer* output, const Buffer* gradient,
                          const Buffer* input, int dtype,
                          std::size_t count, std::string& error);
bool compute_mean_backward(Buffer* output, const Buffer* gradient_scalar,
                           int dtype, std::size_t count, std::string& error);
bool compute_max_last_backward(Buffer* output, const Buffer* gradient,
                               const Buffer* input, int dtype,
                               std::size_t count, std::size_t width,
                               std::string& error);
bool compute_min_last_backward(Buffer* output, const Buffer* gradient,
                               const Buffer* input, int dtype,
                               std::size_t count, std::size_t width,
                               std::string& error);

} // namespace quidra::device
