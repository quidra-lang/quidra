// Observation and metadata adapters; included after device_compute.inc.
namespace {
DeviceCapabilities capabilities_for(const Info& info,const std::vector<Info>& infos) {
    const auto count=infos.size();
    (void)count;
    DeviceCapabilities caps;
    const auto known=[&](CapabilityField field) { caps.available|=static_cast<std::uint32_t>(field); };
    caps.os_build=platform::os_version();
    if(!caps.os_build.empty()) known(CapabilityField::OsBuild);
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    if(info.backend==Backend::Test) {
        caps=test_capabilities(count,platform::environment_value_is("QUIDRA_TEST_FAKE_GPU_UNIFIED","1"));
        return caps;
    }
#endif
#ifdef __APPLE__
    if(info.backend==Backend::Metal && info.observation_identity) {
        @autoreleasepool {
            NSArray<id<MTLDevice>>* all=MTLCopyAllDevices();
            id<MTLDevice> device=nil;
            for(id<MTLDevice> candidate in all)
                if([candidate registryID]==info.observation_identity) {
                    if(device) { device=nil; break; }
                    device=candidate;
                }
            if(device) {
                if(@available(macOS 14.0,*)) {
                    const char* name=[[[device architecture] name] UTF8String];
                    if(name) caps.architecture=name;
                    if(!caps.architecture.empty()) known(CapabilityField::Architecture);
                }
                caps.shared_bytes_per_group=static_cast<std::uint32_t>([device maxThreadgroupMemoryLength]);
                if(caps.shared_bytes_per_group) known(CapabilityField::SharedBytesPerGroup);
                caps.memory_bytes=[device recommendedMaxWorkingSetSize];
                if(caps.memory_bytes) known(CapabilityField::MemoryBytes);
                if(@available(macOS 10.15,*)) {
                    caps.unified_memory=[device hasUnifiedMemory]; known(CapabilityField::UnifiedMemory);
                }
                // Device dimensions are not a total threadgroup bound, and
                // SIMD width is a pipeline property. Both remain unknown.
                caps.driver_version=info.driver; caps.runtime_version=info.runtime;
                known(CapabilityField::DriverVersion); known(CapabilityField::RuntimeVersion);
            }
            [all release];
        }
        return caps;
    }
#endif
    if(info.backend==Backend::Cuda || info.backend==Backend::Hip) {
        const bool nvidia=info.backend==Backend::Cuda;
        auto& library=nvidia ? cuda().library : hip().library;
        using Attribute=int(*)(int*,int,int);
        using Memory=int(*)(std::size_t*,int);
        const auto attribute=load_symbol<Attribute>(library,nvidia ? "cuDeviceGetAttribute" : "hipDeviceGetAttribute");
        const auto memory=load_symbol<Memory>(library,nvidia ? "cuDeviceTotalMem_v2" : "hipDeviceTotalMem");
        int native_device=info.backend_index;
        if(nvidia && (!cuda().device_get || cuda().device_get(&native_device,info.backend_index)!=0)) return caps;
        const auto os=std::move(caps.os_build);
        caps=collect_native_capabilities({nvidia ? NativeCapabilityBackend::Cuda : NativeCapabilityBackend::Hip,
                                          native_device,attribute,memory});
        caps.os_build=os; if(!os.empty()) known(CapabilityField::OsBuild);
        if(nvidia) { caps.driver_version=info.driver; known(CapabilityField::DriverVersion); }
        caps.runtime_version=info.runtime; known(CapabilityField::RuntimeVersion);
        using Peer=int(*)(int*,int,int);
        const auto peer=load_symbol<Peer>(library,nvidia ? "cuDeviceCanAccessPeer" : "hipDeviceCanAccessPeer");
        if(peer) {
            std::vector<bool> access;
            bool complete=true;
            for(const auto& other:infos) {
                if(other.backend!=info.backend) { complete=false; break; }
                int native_peer=other.backend_index;
                if(nvidia && (!cuda().device_get || cuda().device_get(&native_peer,other.backend_index)!=0)) {complete=false; break;}
                int supported{};
                if(peer(&supported,native_device,native_peer)!=0 || (supported!=0 && supported!=1)) {complete=false; break;}
                access.push_back(supported!=0);
            }
            if(complete) { caps.peer_access=std::move(access); known(CapabilityField::PeerAccess); }
        }
        // CUDA/HIP UVA is not a physical unified-memory fact.
        return caps;
    }
    return caps;
}
std::string key_for(const Info& info,const DeviceCapabilities& caps) {
    DeviceKeyFacts facts;
    switch(info.backend) {
    case Backend::Metal: facts.backend="metal"; break;
    case Backend::Cuda: facts.backend="cuda"; break;
    case Backend::Hip: facts.backend="hip"; break;
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
    case Backend::Test: facts.backend="test"; break;
#endif
    }
    facts.name=info.name; facts.os_build=caps.os_build;
    if(caps.has(CapabilityField::Architecture)) facts.architecture=caps.architecture;
    if(caps.has(CapabilityField::ComputeUnits)) facts.compute_units=caps.compute_units;
    if(caps.has(CapabilityField::DriverVersion)) facts.driver_version=caps.driver_version;
    if(caps.has(CapabilityField::RuntimeVersion)) facts.runtime_version=caps.runtime_version;
    facts.math_mode=execution_mode()==ExecutionMode::Deterministic ? "deterministic" : "fast";
#ifdef __APPLE__
    if(info.backend==Backend::Metal) facts.math_mode+=metal_fast_math() ? ":metal-fast" : ":metal-safe";
#endif
    return device_key(facts);
}
void note_observed_devices(const std::vector<Info>& infos) noexcept {
    if(!observation::enabled()) return;
    try {
        for(const auto& info:infos) {
            const auto caps=capabilities_for(info,infos);
            const auto key=key_for(info,caps);
            const char* backend=info.backend==Backend::Metal ? "metal" : info.backend==Backend::Cuda ? "cuda" : "hip";
#ifdef QUIDRA_ENABLE_TEST_GPU_BACKEND
            if(info.backend==Backend::Test) backend="test";
#endif
            timeline::DeviceCapabilities value{info.index,backend,caps.architecture.c_str(),key.c_str(),
                caps.compute_units,caps.max_threads_per_group,caps.shared_bytes_per_group,caps.simd_width,
                caps.memory_bytes,caps.unified_memory,caps.driver_version.c_str(),caps.os_build.c_str(),
                caps.has(CapabilityField::UnifiedMemory)};
            timeline::note_device(value); profile::note_device(value);
            if(info.backend==Backend::Cuda || info.backend==Backend::Hip)
                timeline::note_gpu_sampling_gap({info.index,0,timeline::current_step(),"gpu_clock_alignment_unavailable",true,false});
        }
    } catch (...) {}
}
}
void request_capability_metadata() noexcept {
    capability_metadata_requested.store(true,std::memory_order_relaxed);
}
DeviceCapabilities capabilities(int index) {
    const auto* info=find(index);
    return info ? capabilities_for(*info,devices()) : DeviceCapabilities{};
}
std::string device_key(int index) {
    const auto* info=find(index);
    if(!info) return {};
    return key_for(*info,capabilities_for(*info,devices()));
}
void observation_step_boundary() noexcept {
    const bool capturing=profile::capture_requested() && !metal_capture_closed.load(std::memory_order_acquire);
    if(!timeline::enabled() && !profile::split_enabled() && !capturing) return;
#ifdef __APPLE__
    auto* capture=capturing ? metal_capture_created.load(std::memory_order_acquire) : nullptr;
    const auto* configuration=capturing ? profile::capture_configuration() : nullptr;
    const auto ending_step=capturing ? profile::capture_current_step() : 0;
    const bool capture_edge=capture && configuration &&
        (ending_step==configuration->requested_step ||
         (ending_step!=std::numeric_limits<std::uint64_t>::max() && ending_step+1==configuration->requested_step));
    profile::metal::Edge edge;
    auto* streams=metal_streams_created.load(std::memory_order_acquire);
    if(streams) for(auto* stream:*streams) {
        std::unique_lock lock(stream->mutex,std::try_to_lock);
        const auto serial=stream->next_serial.load(std::memory_order_relaxed);
        const auto step=timeline::current_step();
        if(!lock.owns_lock()) {
            timeline::note_gpu_sampling_gap({stream->global_index,serial,step,"stream_busy_at_boundary",true,false});
            if(capture_edge) { edge.busy=true; edge.prior_work_closed=false; }
            continue;
        }
        if(capture_edge && stream->package_lent) edge.opaque_work=true;
        if(stream->hold!=MetalHold::None) {
            if(stream->open) timeline::note_gpu_sampling_gap({stream->global_index,serial,step,"package_hold_at_boundary",true,false});
            if(capture_edge) { edge.held=true; if(stream->open) edge.prior_work_closed=false; }
            continue;
        }
        if(!stream->open) continue;
        if(capture_edge) {
            // Synchronize diagnostic commits with capture closure, too. An
            // edge computed before finish must not add work after it closes.
            auto* gate=metal_capture_gate_created.load(std::memory_order_acquire);
            try {
                if(gate && gate->command([&] {
                    stream->capture_extra_commit=true;
                    metal_commit_locked(*stream); // Existing safe commit, no marker/wait.
                    return true;
                })) continue;
            } catch (...) { edge.busy=true; edge.prior_work_closed=false; }
        }
        if(stream->encoder && stream->observation && stream->observation->batch &&
           stream->observation->batch->should_split_compute()) {
            metal_end_encoder_locked(*stream); stream->observation_reopen=true;
        }
    }
    if(capture_edge) {
        if(ending_step==configuration->requested_step) capture->boundary(ending_step,edge);
        else capture->note_edge(edge);
        note_metal_capture(capture);
    }
#endif
}
void finish_metal_capture(bool immediate_failure) noexcept {
    if(!profile::capture_requested()) return;
    if(immediate_failure) metal_capture_immediate_close.store(true,std::memory_order_relaxed);
    metal_capture_closed.store(true,std::memory_order_release);
    std::unique_lock<std::mutex> closure;
    if(auto* gate=metal_capture_gate_created.load(std::memory_order_acquire)) {
        try { closure=gate->close(immediate_failure); } catch (...) {}
    }
    if(auto* capture=metal_capture_created.load(std::memory_order_acquire)) {
        capture->stop(immediate_failure ? profile::metal::Reason::ImmediateFailure : profile::metal::Reason::NormalExit,
                      {},immediate_failure);
        note_metal_capture(capture,immediate_failure);
        return;
    }
    const auto* configuration=profile::capture_configuration();
    if(!configuration) return; // Preserve root's preparation failure metadata.
    profile::metal::Status status; status.requested_step=configuration->requested_step;
    status.completion_reason=immediate_failure ? profile::metal::Reason::ImmediateFailure : profile::metal::Reason::NormalExit;
    if(metal_capture_setup_attempted.load(std::memory_order_acquire)) {
        if(metal_capture_setup_finished.load(std::memory_order_acquire)) return; // Preserve rejected setup status.
        status.state=profile::metal::State::Unavailable; status.snapshot_complete=false;
        status.gaps[static_cast<std::size_t>(profile::metal::Gap::StopBusy)]=1;
        profile::note_capture(status); return; // Never wait for concurrent setup.
    }
    status.state=profile::metal::State::NoMetalWork;
    try { status.output_path=configuration->output_path; }
    catch (...) { status.state=profile::metal::State::Unavailable; status.snapshot_complete=false;
        status.metadata_drops=1; status.gaps[static_cast<std::size_t>(profile::metal::Gap::StorageFailed)]=1; }
    profile::note_capture(status);
}
