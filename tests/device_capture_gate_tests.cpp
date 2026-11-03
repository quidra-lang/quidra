#include "device/capture_gate.hpp"
#include "runtime/metal_capture.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
namespace mc = quidra::profile::metal;
namespace {
int failures{};
void expect(bool value,const char* message) { if(!value) {++failures;std::fprintf(stderr,"FAIL: %s\n",message);} }
struct Native { unsigned acquires{},starts{},stops{},releases{}; bool active{}; };
bool available(void*) noexcept {return true;}
bool acquire(void* p) noexcept {++static_cast<Native*>(p)->acquires;return true;}
bool supports(void*) noexcept {return true;}
bool capturing(void* p) noexcept {return static_cast<Native*>(p)->active;}
bool start(void* p,void*,const char*,mc::NativeError&) noexcept {auto& n=*static_cast<Native*>(p);++n.starts;n.active=true;return true;}
bool stop(void* p) noexcept {auto& n=*static_cast<Native*>(p);++n.stops;n.active=false;return true;}
void release(void* p) noexcept {++static_cast<Native*>(p)->releases;}
std::uint64_t clock(void*) noexcept {return 1;}
mc::NativeApi api(Native& n) {return {&n,available,acquire,supports,capturing,start,stop,release,clock};}
struct Temporary {
    std::filesystem::path directory;
    Temporary() {
        const auto base=std::filesystem::temp_directory_path();
        for(unsigned i=0;i<1000;++i) {
            auto p=base/("quidra-capture-gate-"+std::to_string(reinterpret_cast<std::uintptr_t>(this))+"-"+std::to_string(i));
            if(std::filesystem::create_directory(p)) {directory=std::move(p);return;}
        }
        throw std::runtime_error("private temporary path unavailable");
    }
    ~Temporary() {std::error_code error;std::filesystem::remove_all(directory,error);}
    std::string path(const char* name) const {return (directory/name).string();}
};
void closed_before_work(Temporary& temporary) {
    std::atomic<bool> closed{false}; quidra::device::detail::CaptureGate gate(closed);
    Native n; const auto native=api(n); const auto path=temporary.path("closed.gputrace");
    unsigned setups{},publications{},commits{};
    {const auto closure=gate.close(false);expect(closure.owns_lock(),"normal close owns setup synchronization");}
    const bool prepared=gate.prepare([&]{++setups;return mc::make_capture({true,0,path,{}},&native);},
                                    [&](mc::Creation){++publications;},[](mc::Creation){});
    const auto token=gate.command([&]{++n.starts;return mc::CommandToken{1,0,1,0};});
    const bool committed=gate.command([&]{++commits;return true;});
    expect(!committed&&commits==0,"late boundary cannot add a diagnostic commit after closure");
    expect(!prepared&&!token&&setups==0&&publications==0,"finish before first work rejects setup and command admission");
    expect(n.acquires==0&&n.starts==0&&n.stops==0,"closed entry makes no manager acquire/start/stop calls");
}
void admitted_work(Temporary& temporary) {
    std::atomic<bool> closed{false}; quidra::device::detail::CaptureGate gate(closed);
    Native n; const auto native=api(n); const auto path=temporary.path("owned.gputrace");
    std::shared_ptr<mc::Capture> owner;
    expect(gate.prepare([&]{return mc::make_capture({true,0,path,{}},&native);},
                        [&](mc::Creation c){owner=std::move(c.capture);},[](mc::Creation){}),"open gate publishes controller");
    expect(bool(owner),"prepared controller exists");
    const auto token=gate.command([&]{return owner->before_command({0,1,0,&n,{}});});
    expect(bool(token)&&n.acquires==1&&n.starts==1,"admitted command uses actual owned helper native calls");
    {const auto closure=gate.close(false);expect(closure.owns_lock(),"finish synchronizes admitted native entry");owner->stop(mc::Reason::NormalExit);}
    expect(n.stops==1&&n.releases==1,"owned stop remains exactly once after closure");
    expect(!gate.command([&]{return owner->before_command({0,2,0,&n,{}});})&&n.starts==1,"later command cannot restart capture");
}
void concurrent_preparation(Temporary& temporary) {
    std::atomic<bool> closed{false}; quidra::device::detail::CaptureGate gate(closed);
    Native n; const auto native=api(n); const auto path=temporary.path("racing.gputrace");
    std::mutex mutex;std::condition_variable cv;bool entered{},resume{};
    unsigned setups{},publications{},cancellations{};bool prepared{};
    std::thread worker([&]{prepared=gate.prepare([&]{
        ++setups;{std::unique_lock lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return resume;});}
        return mc::make_capture({true,0,path,{}},&native);
    },[&](mc::Creation c){++publications;c.capture->before_command({0,1,0,&n,{}});},
      [&](mc::Creation c){++cancellations;c.capture->stop(mc::Reason::ImmediateFailure,{},true);});});
    {std::unique_lock lock(mutex);cv.wait(lock,[&]{return entered;});}
    {const auto closure=gate.close(true);expect(!closure.owns_lock(),"failure closure does not wait for preparation");}
    {std::lock_guard lock(mutex);resume=true;}cv.notify_all();worker.join();
    expect(!prepared&&setups==1&&publications==0&&cancellations==1,"finish racing preparation cancels publication after factory returns");
    expect(n.acquires==0&&n.starts==0&&n.stops==0,"cancelled preparation never starts native capture");
    expect(!gate.command([&]{++n.starts;return mc::CommandToken{1,0,1,0};})&&n.starts==0,"closure remains sticky after racing preparation ends");
}
}
int main() {
    try {Temporary temporary;closed_before_work(temporary);admitted_work(temporary);concurrent_preparation(temporary);}
    catch(const std::exception& e){std::fprintf(stderr,"setup failure: %s\n",e.what());return 2;}
    if(failures)return 1;std::puts("Device capture admission and late shutdown: ok");return 0;
}
