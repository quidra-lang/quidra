// Included after CudaApi/HipApi and their resource helpers, inside the
// backend's anonymous namespace. Only node operations prepare a marker.
void arm_deferred_validation_exit_guard();
detail::OwnedNativeFences& owned_native_fences() {
    static detail::OwnedNativeFences state;
    return state;
}
struct NativeEventHandle {
    Backend backend{};
    int backend_index{};
    int global_index{-1};
    void* event{};
    bool recorded{};
    bool package_boundary{};
    int record_status{};
    std::string select() {
        std::string error;
        if(backend==Backend::Cuda) {
            CudaApi::CUcontext context=nullptr;
            if(!cuda().current(backend_index,context,error)) return error;
        } else if(!hip().set_device||hip().set_device(backend_index)!=0) {
            return "failed to select AMD GPU for an owned completion fence";
        }
        return {};
    }
    std::string release() {
        if(!event) return {};
        if(auto error=select();!error.empty()) return error;
        const auto handle=std::exchange(event,nullptr);
        const auto status=backend==Backend::Cuda
            ?cuda().event_destroy(handle):hip().event_destroy(handle);
        if(status!=0) return "failed to destroy an owned native GPU completion fence";
        return {};
    }
    ~NativeEventHandle() {if(event) {try {(void)release();}catch(...) {}}}
    detail::NativeFenceResult wait() {
        using Status=detail::NativeFenceStatus;
        if(auto error=select();!error.empty()) return {Status::unattributed_failure,std::move(error)};
        if(!recorded) {
            // Work has already been launched. A failed record cannot become
            // an immediate-success event: drain the backend before cleanup.
            const auto status=observation::wait(global_index,counters::WaitReason::Status,"native failed fence drain",[&]{
                return backend==Backend::Cuda ?cuda().ctx_synchronize():hip().device_synchronize();});
            return {Status::unattributed_failure,
                "native GPU completion fence recording failed (record status "+
                std::to_string(record_status)+", drain status "+std::to_string(status)+")"};
        }
        // Package code can enqueue on a custom nonblocking stream which a
        // default-stream event does not cover. Its native-handle borrow uses
        // the existing context/device synchronization API as the completion
        // proof; any shared fault follows the unknown-owner fatal policy.
        if(package_boundary) {
            const auto context_status=observation::wait(global_index,counters::WaitReason::Status,"native package fence",[&]{
                return backend==Backend::Cuda ?cuda().ctx_synchronize():hip().device_synchronize();});
            if(context_status!=0) return {Status::unattributed_failure,
                "package GPU context completion returned backend status "+std::to_string(context_status)};
        }
        const auto status=observation::wait(global_index,counters::WaitReason::Status,"native completion fence",[&]{
            return backend==Backend::Cuda ?cuda().event_synchronize(event):hip().event_synchronize(event);});
        if(status==0) return {};
        // Driver/runtime event waits can surface a shared-context fault. No
        // proof here identifies the operation that originally caused it.
        return {Status::unattributed_failure,
            "native GPU completion fence returned an unattributed backend status "+std::to_string(status)};
    }
    void record() noexcept {
        try {
            // Foreign package code may have selected another context/device.
            if(!select().empty()) {record_status=-1;recorded=false;return;}
            record_status=backend==Backend::Cuda
                ?cuda().event_record(event,nullptr):hip().event_record(event,nullptr);
            recorded=record_status==0;
        }catch(...) {record_status=-1;recorded=false;}
    }
};
struct PreparedNativeFence {
    std::shared_ptr<runtime_parallel::detail::DeferredNode> owner;
    std::shared_ptr<NativeEventHandle> event;
    std::optional<detail::OwnedNativeFences::Submission> submission;
    detail::OwnedNativeFences::Reservation reservation;
    std::optional<runtime::ReportSite> source;
    std::shared_ptr<runtime::OwnedNodeReport> trace;
    int global_index{};
    explicit operator bool() const noexcept {return owner!=nullptr;}
    bool published{};
    bool armed{};
    PreparedNativeFence()=default;
    PreparedNativeFence(PreparedNativeFence&&)=default;
    PreparedNativeFence(const PreparedNativeFence&)=delete;
    PreparedNativeFence& operator=(PreparedNativeFence&& other) {
        if(this!=&other) {
            if(owner&&armed&&!published) publish();
            owner=std::move(other.owner);event=std::move(other.event);
            submission.reset();if(other.submission) {submission.emplace(std::move(*other.submission));other.submission.reset();}
            reservation=std::move(other.reservation);
            source=std::move(other.source);trace=std::move(other.trace);
            global_index=other.global_index;published=other.published;armed=other.armed;
        }
        return *this;
    }
    ~PreparedNativeFence() {if(owner&&armed&&!published) publish();}
    void publish() noexcept {
        if(!owner||!armed||published) return;
        // This path is also used during failure unwinding. All queue/ticket
        // and callback allocations were reserved before the first GPU work.
        if(!submission) submission.emplace(owned_native_fences().submission(reservation));
        event->record();
        owned_native_fences().publish(*submission,reservation);
        published=true;
        submission.reset();
    }

};
std::optional<PreparedNativeFence> prepare_native_fence(
        Backend backend,int global_index,int backend_index,std::string& error) {
    PreparedNativeFence prepared;
    prepared.owner=runtime_parallel::detail::deferred_node();
    if(!prepared.owner) return std::optional<PreparedNativeFence>(std::move(prepared));
    prepared.global_index=global_index;
    prepared.source=runtime::report_site(0,0,runtime::current_user_statement(),true);
    // Snapshot the worker's user-call path while its dispatch frames exist.
    const auto reason=abi::FailureReason::deferred_check_failed;
    runtime::ReportData data;data.reason=&reason;
    data.provenance=prepared.source->statement;
    const runtime::ReportHead head{runtime::ReportKind::coded,
        abi::spelling(abi::failure_reason_info(reason).code)};
    prepared.trace=std::make_shared<runtime::OwnedNodeReport>(head,
        runtime::locate_report_site(*prepared.source),std::string_view{},data,prepared.source);
    prepared.submission.emplace(owned_native_fences().submission(global_index));
    prepared.event=std::make_shared<NativeEventHandle>();
    auto& event=*prepared.event;event.backend=backend;event.backend_index=backend_index;event.global_index=global_index;
    if(auto selection=event.select();!selection.empty()) {error=std::move(selection);return std::nullopt;}
    int status=-1;
    if(backend==Backend::Cuda) {
        auto& api=cuda();
        if(api.event_create&&api.event_record&&api.event_synchronize&&api.event_destroy&&api.ctx_synchronize)
            status=api.event_create(&event.event,2u);
    } else {
        auto& api=hip();
        if(api.event_create&&api.event_record&&api.event_synchronize&&api.event_destroy&&api.device_synchronize)
            status=api.event_create(&event.event);
    }
    if(status!=0||!event.event) {
        error="native GPU completion fence API is unavailable for a parallel node";
        return std::nullopt;
    }
    detail::NativeFence fence{
        [event=prepared.event] {return event->wait();},
        [event=prepared.event] {return event->release();},
        [trace=prepared.trace,source=*prepared.source](const std::string& message) {
            const auto reason=abi::FailureReason::deferred_check_failed;
            runtime::ReportData data;
            data.reason=&reason;data.args.message=message;
            data.path=&trace->path();data.provenance=source.statement;
            const auto text=abi::format(reason,data.args);
            return std::make_shared<runtime::OwnedNodeReport>(trace->head(),
                runtime::locate_report_site(source),text,data,source);
        }};
    prepared.reservation=owned_native_fences().reserve(*prepared.submission,
        prepared.owner,std::move(fence),prepared.source);
    arm_deferred_validation_exit_guard();
    prepared.armed=true;
    return std::optional<PreparedNativeFence>(std::move(prepared));
}

// Called only after native handles and registry/device locks were released.
// A context-wide failure is not an attributed operation failure and must not
// be caught by a NodeFailureScope or retried with a live pending ticket.
[[noreturn]] void fail_unattributed_native(const std::string& detail) {
    std::fflush(stdout);
    const auto code=abi::spelling(abi::failure_reason_info(abi::FailureReason::deferred_check_failed).code);
    // Formatting must not allocate after an unproved device-context failure.
    std::fprintf(stderr,"Quidra runtime error[%.*s]: operation owner unknown: %s\n",
        static_cast<int>(code.size()),code.data(),detail.c_str());
    std::fflush(stderr);
    runtime::immediate_failure_exit();
}
void drain_native_fences(int index,std::uint64_t covered) {
    owned_native_fences().drain_or_fail(index,covered,fail_unattributed_native);
}
// A borrow saves the owner/worker trace before package code runs. Holding a
// device lock across arbitrary foreign code could deadlock a nested Node join;
// only the final marker record/enqueue holds the per-device submission lock.
thread_local std::vector<PreparedNativeFence> native_package_epochs;
void lend_native_package_work(Backend backend,int global_index,int backend_index) {
    auto owner=runtime_parallel::detail::deferred_node();
    if(!owner) return;
    for(const auto& epoch:native_package_epochs)
        if(epoch.global_index==global_index&&epoch.owner==owner) return;
    std::string error;
    auto prepared=prepare_native_fence(backend,global_index,backend_index,error);
    if(!prepared) fail_unattributed_native(error);
    prepared->event->package_boundary=true;
    prepared->submission.reset();
    arm_deferred_validation_exit_guard();
    native_package_epochs.push_back(std::move(*prepared));
}
void seal_native_package_epochs() {
    const auto owner=runtime_parallel::detail::current_deferred_node;
    for(auto it=native_package_epochs.begin();it!=native_package_epochs.end();) {
        if(it->owner==owner) {
            auto epoch=std::move(*it);
            it=native_package_epochs.erase(it);
            epoch.publish();
        }else ++it;
    }
}
