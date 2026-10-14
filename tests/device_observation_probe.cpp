#include "device_backend.hpp"
#include "runtime/device_observation.hpp"
#include "quidra/abi/dtype.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
int main(int argc,char** argv) {
    using namespace quidra;
    bool hold=false,mixed_core=false,two_capture_boundaries=false,close_capture_before_work=false;
    for(int i=1;i<argc;++i) {
        hold=hold || std::strcmp(argv[i],"--hold")==0;
        mixed_core=mixed_core || std::strcmp(argv[i],"--mixed-core")==0;
        two_capture_boundaries=two_capture_boundaries || std::strcmp(argv[i],"--capture-two-boundaries")==0;
        close_capture_before_work=close_capture_before_work || std::strcmp(argv[i],"--capture-close-before-work")==0;
    }
    counters::mark_program_started();
    timeline::mark_program_started(); profile::mark_program_started();
    if(close_capture_before_work) {
        device::finish_metal_capture(false);
        if(!profile::flush_report()) return 1;
    }
    const auto& all=device::devices();
    if(all.empty()) { std::fputs("SKIP: GPU backend unavailable\n",stderr); return 77; }
    const int gpu=all.front().index;
    std::string error;
    const auto cleanup=[](device::Buffer* x){device::release(x);};
    std::unique_ptr<device::Buffer,decltype(cleanup)> out(device::allocate(gpu,8,error),cleanup);
    const auto check=[&](bool ok) { if(!ok) std::fprintf(stderr,"FAIL: %s\n",error.c_str()); return ok; };
    if(!check(bool(out))) return 1;
    if(!check(device::compute_fill_ones(out.get(),QCORE_DTYPE_FLOAT32,2,error))) return 1;
    bool held=false;
    if(hold) {
        held=device::hold_package_stream(gpu);
        if(all.front().backend==device::Backend::Metal && !held) return 1;
        if(held && !check(device::begin_package_encode(gpu,false,error)!=0)) return 1;
    }
    device::observation_step_boundary();
    timeline::note_point(timeline::Point::StepBoundary);
    profile::advance_capture_step();
    if(held) {
        bool violated=false;
        if(!check(device::end_package_encode(gpu,0,0,violated,error)) || violated) return 1;
    }
    if(!check(device::compute_fill_ones(out.get(),QCORE_DTYPE_FLOAT32,2,error))) return 1;
    if(mixed_core) {
        const float zero=0.0f;
        if(!check(device::compute_binary(out.get(),out.get(),0,nullptr,0,&zero,
             abi::scalar_side::right,QCORE_DTYPE_FLOAT32,abi::tensor_binary_opcode::add,2,error))) return 1;
    }
    float values[2]{};
    if(!check(device::copy_to_host(out.get(),0,values,sizeof(values),error))) return 1;
    if(values[0]!=1.0f || values[1]!=1.0f) {
        std::fputs("FAIL: observation changed numeric values\n",stderr); return 1;
    }
    if(two_capture_boundaries) {
        device::observation_step_boundary(); timeline::note_point(timeline::Point::StepBoundary);
        profile::advance_capture_step();
    }
    out.reset();
    timeline::note_point(timeline::Point::ExitBegin);
    if(!check(device::synchronize(gpu,error))) return 1;
    timeline::note_point(timeline::Point::ExitEnd);
    device::finish_metal_capture(false);
    counters::flush_report();
    if(timeline::enabled() && !timeline::flush_report()) return 1;
    if(profile::enabled() && !profile::flush_report()) return 1;
    std::puts("Device observation backend values: ok");
}
