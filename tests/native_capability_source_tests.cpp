#include <cstddef>
#include <cstdio>
#include <cstdint>
#if __has_include("device/native_capability_source.hpp")
#include "device/native_capability_source.hpp"
namespace {
// Independent native driver semantics, including properties unrelated to
// the capability under test. Values model real scalar driver responses.
bool fail_warp{},fail_memory{};
unsigned queries{};
int cuda_attribute(int* result,int property,int device) {
    ++queries;
    if(device!=9) return 101;
    switch(property) {
    case 1: *result=1024; return 0;             // max threads per block
    case 5: *result=2147483647; return 0;       // max grid x
    case 8: *result=49152; return 0;            // shared memory per block
    case 10: if(fail_warp) return 1; *result=32; return 0; // warp lanes
    case 16: *result=48; return 0;              // multiprocessors
    case 75: *result=8; return 0;               // compute capability major
    case 76: *result=6; return 0;               // compute capability minor
    default: return 1;
    }
}
int hip_attribute(int* result,int property,int device) {
    ++queries;
    if(device!=3) return 101;
    switch(property) {
    case 56: *result=512; return 0;             // max threads per block
    case 63: *result=120; return 0;             // multiprocessors
    case 74: *result=65536; return 0;           // shared memory per block
    case 87: if(fail_warp) return 1; *result=64; return 0; // wave lanes
    case 23: *result=9; return 0;               // deprecated CUDA-style major; not gfx architecture
    default: return 1;
    }
}
int memory(std::size_t* result,int device) {
    if(fail_memory || (device!=9 && device!=3)) return 1;
    *result=std::size_t{8}<<30; return 0;
}
int check(bool value,const char* message) {
    if(!value) std::fprintf(stderr,"FAIL: %s\n",message);
    return value ? 0 : 1;
}
}
int main() {
    using namespace quidra::device;
    int failures{};
    auto caps=collect_native_capabilities({NativeCapabilityBackend::Cuda,9,cuda_attribute,memory});
    failures+=check(caps.max_threads_per_group==1024 && caps.compute_units==48 &&
                    caps.shared_bytes_per_group==49152 && caps.simd_width==32 &&
                    caps.memory_bytes==(std::uint64_t{8}<<30) && caps.architecture=="compute_8.6",
                    "CUDA semantic properties are mapped to the correct fields");
    failures+=check(caps.has(CapabilityField::MemoryBytes) && caps.has(CapabilityField::Architecture) &&
                    !caps.has(CapabilityField::UnifiedMemory),"UVA/integrated inference is not a unified-memory query");
    caps=collect_native_capabilities({NativeCapabilityBackend::Hip,3,hip_attribute,memory});
    failures+=check(caps.max_threads_per_group==512 && caps.compute_units==120 &&
                    caps.shared_bytes_per_group==65536 && caps.simd_width==64 &&
                    !caps.has(CapabilityField::Architecture),"HIP native fields do not invent gfx architecture");
    fail_warp=fail_memory=true;
    caps=collect_native_capabilities({NativeCapabilityBackend::Cuda,9,cuda_attribute,memory});
    failures+=check(!caps.has(CapabilityField::SimdWidth) && !caps.has(CapabilityField::MemoryBytes) &&
                    caps.has(CapabilityField::MaxThreadsPerGroup),"failed optional query affects only its field");
    const auto before=queries;
    caps=collect_native_capabilities({NativeCapabilityBackend::Cuda,9,nullptr,nullptr});
    failures+=check(caps.available==0 && queries==before,"absent optional symbols perform no attribute queries");
    caps=collect_native_capabilities({NativeCapabilityBackend::Hip,-1,hip_attribute,memory});
    failures+=check(caps.available==0,"invalid native identity cannot produce hardware facts");
    if(!failures) std::puts("Native capability semantic queries: ok");
    return failures ? 1 : 0;
}
#else
int main() { std::fputs("FAIL: native semantic capability adapter is missing\n",stderr); return 1; }
#endif
