#pragma once

#include "device/capabilities.hpp"
#include "platform/clock.hpp"
#include "runtime/profile.hpp"
#include "runtime/timeline.hpp"
#include <string>

namespace quidra::device {
// Metadata only: no queue, pipeline, context, GPU allocation or work.
DeviceCapabilities capabilities(int index);
std::string device_key(int index);
void request_capability_metadata() noexcept;
// Called before the logical step advances. Never creates a stream or waits.
// Capture alone may commit safe Core buffers at its diagnostic step edges;
// timeline/split never commits, and no mode touches lent package encoders.
void observation_step_boundary() noexcept;
// Root calls normal after existing drains, or immediate before failure flush.
// Never initializes a stream/controller, commits, or waits.
void finish_metal_capture(bool immediate_failure) noexcept;
// Finalize only the optional capture manager; no additional stream work/wait.
void finish_metal_capture(bool immediate_failure) noexcept;
}

namespace quidra::observation {
inline bool enabled() noexcept { return timeline::enabled() || profile::enabled(); }

class Source {
public:
    bool capture() noexcept {
        try {
            counters::SourceSite site{};
            (void)counters::current_source_site(site);
            line_=site.line; column_=site.column;
            file_=site.file ? site.file : "";
            node_=site.node_id ? site.node_id : "";
            return true;
        } catch (...) { file_.clear(); node_.clear(); line_=column_=0; return false; }
    }
    counters::SourceSite site() const noexcept {
        return {file_.c_str(),node_.c_str(),line_,column_};
    }
private:
    std::string file_,node_;
    unsigned long long line_{},column_{};
};

enum class Kind { Wait, Warmup, Compile };
// Synchronous scopes own provenance at entry; their record step is unaffected
// by a concurrent logical boundary. Disabled scopes query nothing.
class Scope {
public:
    Scope(Kind kind,std::int32_t device,const char* name,
          counters::WaitReason reason=counters::WaitReason::Sync,
          std::uint32_t identity=0) noexcept
        : kind_(kind),device_(device),name_(name),reason_(reason),identity_(identity) {
        active_=enabled();
        if(!active_) return;
        begin_=platform::monotonic_ns(); step_=timeline::current_step();
        source_.capture(); thread_=profile::current_thread();
    }
    ~Scope() { finish(); }
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    void finish() noexcept {
        if(!active_) return;
        active_=false;
        const auto end=platform::monotonic_ns();
        const auto site=source_.site();
        switch(kind_) {
        case Kind::Wait:
            timeline::note_wait({step_,reason_,begin_,end,site}); break;
        case Kind::Warmup:
            timeline::note_warmup({device_,identity_,begin_,end,step_,site}); break;
        case Kind::Compile:
            timeline::note_compile({device_,identity_,site.file[0]||site.node_id[0]||site.line,
                                    begin_,end,step_,site}); break;
        }
        profile::ProfileEvent event;
        event.kind=kind_==Kind::Wait ? profile::ProfileEvent::Kind::Sync
                                   : profile::ProfileEvent::Kind::HostOp;
        event.site=site; event.device=device_; event.host_begin_ns=begin_; event.host_end_ns=end;
        event.host_thread=thread_; event.owner="core"; event.name=name_; event.kernel=name_;
        profile::record(event);
    }
private:
    Kind kind_;
    std::int32_t device_;
    const char* name_;
    counters::WaitReason reason_;
    std::uint32_t identity_{},step_{};
    bool active_{};
    std::uint64_t begin_{},thread_{};
    Source source_;
};
template<class Function>
auto wait(std::int32_t device,counters::WaitReason reason,const char* name,Function&& call) {
    Scope observed(Kind::Wait,device,name,reason);
    return call();
}
} // namespace quidra::observation
