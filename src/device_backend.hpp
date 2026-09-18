#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quidra::device {

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

} // namespace quidra::device
