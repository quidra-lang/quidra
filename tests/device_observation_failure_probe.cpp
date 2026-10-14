#include "quidra/abi/layout.hpp"
#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/native_extension.h"
#include <chrono>
#include <thread>

namespace {
void failing_warmup(long long) {
    (void)qcore_profile_range_begin("test","warmup ordinary failure",-1);
    // This ordinary failure reaches the device callback's immediate route;
    // running process-exit drains on its own warmup thread would deadlock.
    quidra_runtime_bounds_fail(1,0,1,1);
}
}
int main(int argc,char** argv) {
    quidra_runtime_set_args(argc,argv);
    const quidra::abi::SourceProvenanceRecord source{"observation.qui","","first","statement",1,1};
    quidra_runtime_set_source_provenance(&source);
    qcore_register_warmup(failing_warmup);
    auto* buffer=qcore_device_buffer_allocate(0,4);
    if(!buffer) return 2;
    for(unsigned i=0;i<100;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    qcore_device_buffer_release(buffer);
    return 3;
}
