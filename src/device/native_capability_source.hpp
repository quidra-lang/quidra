#pragma once
#include "device/capabilities.hpp"
#include <cstddef>

namespace quidra::device {
enum class NativeCapabilityBackend { Cuda,Hip };
// Borrowed scalar driver functions: neither the adapter nor these metadata
// queries select/create contexts, queues, memory, events or GPU work.
struct NativeCapabilitySource {
    NativeCapabilityBackend backend{};
    int device{};
    int (*attribute)(int*,int,int){};
    int (*memory)(std::size_t*,int){};
};
inline DeviceCapabilities collect_native_capabilities(const NativeCapabilitySource& source) {
    DeviceCapabilities caps;
    if(source.device<0) return caps;
    const bool cuda=source.backend==NativeCapabilityBackend::Cuda;
    if(!cuda && source.backend!=NativeCapabilityBackend::Hip) return caps;
    const auto known=[&](CapabilityField field) {caps.available|=static_cast<std::uint32_t>(field);};
    // Stable scalar enum ABI, independently checked against vendor headers.
    // CUDA: max threads1, shared8, warp10, multiprocessors16, capability75/76.
    // HIP: max threads56, multiprocessors63, shared74, warp87.
    const auto read=[&](int cuda_property,int hip_property,std::uint32_t& value,CapabilityField field) {
        int result{};
        if(source.attribute && source.attribute(&result,cuda ? cuda_property : hip_property,source.device)==0 && result>0) {
            value=static_cast<std::uint32_t>(result); known(field);
        }
    };
    read(1,56,caps.max_threads_per_group,CapabilityField::MaxThreadsPerGroup);
    read(16,63,caps.compute_units,CapabilityField::ComputeUnits);
    read(8,74,caps.shared_bytes_per_group,CapabilityField::SharedBytesPerGroup);
    read(10,87,caps.simd_width,CapabilityField::SimdWidth);
    std::size_t bytes{};
    if(source.memory && source.memory(&bytes,source.device)==0 && bytes) {
        caps.memory_bytes=bytes; known(CapabilityField::MemoryBytes);
    }
    if(cuda && source.attribute) {
        int major{},minor{};
        if(source.attribute(&major,75,source.device)==0 && major>0 &&
           source.attribute(&minor,76,source.device)==0 && minor>=0) {
            caps.architecture="compute_"+std::to_string(major)+"."+std::to_string(minor);
            known(CapabilityField::Architecture);
        }
    }
    // HIP gfx architecture needs a supported typed-property SDK adapter;
    // deprecated scalar architecture slots and copied layouts are not used.
    // CUDA/HIP UVA is not a physical unified-memory fact.
    return caps;
}
} // namespace quidra::device
