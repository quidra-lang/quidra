#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if __has_include("runtime/device_observation.hpp")
#include "runtime/device_observation.hpp"
namespace {
std::uint64_t stamp = 100;
unsigned clocks{}, sources{};
char file[] = "original.qui", node[] = "first";
std::vector<quidra::timeline::WaitInterval> waits;
std::vector<quidra::timeline::WarmupInterval> warmups;
std::vector<quidra::timeline::CompileInterval> compiles;
std::vector<quidra::profile::ProfileEvent> events;
std::string seen_file, seen_node;
}
namespace quidra::platform {
std::uint64_t monotonic_ns() { ++clocks; return stamp += 10; }
}
namespace quidra::counters {
bool current_source_site(SourceSite& site) { ++sources; site = {file,node,7,3}; return true; }
}
namespace quidra::timeline::detail {
std::atomic<bool> enabled{false};
std::atomic<std::uint32_t> step{4};
void note_wait_slow(const WaitInterval& x) noexcept { waits.push_back(x); seen_file=x.site.file; seen_node=x.site.node_id; }
void note_warmup_slow(const WarmupInterval& x) noexcept { warmups.push_back(x); }
void note_compile_slow(const CompileInterval& x) noexcept { compiles.push_back(x); }
}
namespace quidra::profile::detail {
std::atomic<bool> enabled{false};
std::uint64_t current_thread_slow() noexcept { return 11; }
void record_slow(const ProfileEvent& x) noexcept { events.push_back(x); seen_file=x.site.file; seen_node=x.site.node_id; }
}
static int check(bool ok,const char* name) {
    if (!ok) std::fprintf(stderr,"FAIL: %s\n",name);
    return ok ? 0 : 1;
}
int main() {
    using namespace quidra;
    using observation::Scope;
    using observation::Kind;
    int failures{};
    { Scope x(Kind::Wait,2,"wait",counters::WaitReason::Read); }
    failures += check(clocks==0 && sources==0 && waits.empty() && events.empty(),"disabled scope never queries clocks/source");
    timeline::detail::enabled=true;
    {
        Scope x(Kind::Wait,2,"wait",counters::WaitReason::Read);
        timeline::detail::step=5;
        std::strcpy(file,"modified.qui"); std::strcpy(node,"later");
        x.finish(); x.finish();
    }
    failures += check(waits.size()==1 && waits[0].step==4 && waits[0].begin_ns==110 && waits[0].end_ns==120 && waits[0].reason==counters::WaitReason::Read,"wait keeps begin step and closes once");
    failures += check(seen_file=="original.qui" && seen_node=="first","source is owned until delayed record");
    { Scope x(Kind::Warmup,3,"warmup",counters::WaitReason::Sync,9); }
    { Scope x(Kind::Compile,3,"pipeline",counters::WaitReason::Sync,12); }
    failures += check(warmups.size()==1 && warmups[0].function==9 && compiles.size()==1 && compiles[0].pipeline==12 && compiles[0].timed_path,"typed warmup and compile IDs recorded");
    timeline::detail::enabled=false; profile::detail::enabled=true;
    { Scope x(Kind::Wait,7,"native wait",counters::WaitReason::Status); }
    failures += check(waits.size()==1 && events.size()==1 && events[0].device==7 && events[0].kind==profile::ProfileEvent::Kind::Sync && events[0].host_thread==11 && events[0].gpu_begin_ns==0 && events[0].host_end_ns>=events[0].host_begin_ns,"profile-only records host wait without fabricated GPU time");
    if(!failures) std::puts("Device observation scopes: ok");
    return failures ? 1 : 0;
}
#else
int main() { std::fputs("FAIL: backend observation scopes are missing\n",stderr); return 1; }
#endif
