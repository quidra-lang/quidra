#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
std::uint64_t random_next(void* generator) {
    auto* state=static_cast<std::uint64_t*>(generator);
    *state+=0x9e3779b97f4a7c15ULL;
    std::uint64_t value=*state;
    value=(value^(value>>30U))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27U))*0x94d049bb133111ebULL;
    return value^(value>>31U);
}
}

extern "C" void quidra_test_assert(bool condition) {
    if(condition) return;
    std::fprintf(stderr,"Quidra test assertion failed\n");
    std::exit(1);
}
extern "C" double quidra_time_now() {
    using clock=std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
extern "C" bool quidra_time_sleep(double seconds) {
    if(!std::isfinite(seconds)||seconds<0.0) return false;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return true;
}
extern "C" long long quidra_random_int(void* generator,long long start,long long end) {
    const auto span=static_cast<std::uint64_t>(end)-static_cast<std::uint64_t>(start);
    const auto threshold=(std::uint64_t{0}-span)%span;
    std::uint64_t sample=0;
    do{sample=random_next(generator);}while(sample<threshold);
    const auto result_bits=static_cast<std::uint64_t>(start)+(sample%span);
    return std::bit_cast<long long>(result_bits);
}
extern "C" double quidra_random_float(void* generator) {
    const auto sample=random_next(generator)>>11U;
    return static_cast<double>(sample)*(1.0/9007199254740992.0);
}
extern "C" bool quidra_random_bool(void* generator) {
    return (random_next(generator)&1ULL)!=0;
}
