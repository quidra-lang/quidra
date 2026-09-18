#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace quidra::device {

enum class Backend {
    Nvidia,
    Amd,
    Metal,
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

} // namespace quidra::device
