#include "parallel_call_handoff.hpp"
#include <cstdio>
#include <cstdlib>
#include "quidra/abi/parallel_call_ownership.h"
#include "quidra/abi/runtime_failure.hpp"
extern "C" void* quidra_parallel_call_begin(const void* target,const void* const* owners,std::uint32_t count) {
    using namespace quidra::runtime;
    if(!current_node_failure || !count)return nullptr;
    if(!target || !owners || static_cast<std::size_t>(count)>(std::numeric_limits<std::size_t>::max()-sizeof(PendingCallAcceptance))/sizeof(PendingArgumentOwner))
        report_uncoded("invalid parallel argument owner table");
    for(std::size_t i=0;i<count;++i)if(!owners[i*2] || !owners[i*2+1])
        report_uncoded("invalid parallel argument owner mirror");
    const auto bytes=sizeof(PendingCallAcceptance)+sizeof(PendingArgumentOwner)*count;
    auto* call=static_cast<PendingCallAcceptance*>(create_node_frame(bytes,alignof(PendingCallAcceptance),abandon_pending_call));
    call->previous=pending_call_acceptance;call->owner=current_node_failure;call->target=target;call->count=count;
    for(std::size_t i=0;i<count;++i)
        pending_arguments(call)[i]={const_cast<void*>(owners[i*2]),reinterpret_cast<void(*)(void*) noexcept>(const_cast<void*>(owners[i*2+1]))};
    pending_call_acceptance=call;
    return call;
}
extern "C" bool quidra_parallel_call_accept(const void* target) {
    return quidra::runtime::accept_pending_call(target);
}
extern "C" bool quidra_parallel_call_end(void* ticket) {
    return quidra::runtime::end_pending_call(ticket);
}

extern "C" void quidra_parallel_call_require_accept(bool accepted) {
    if(accepted)return;
    // Once a consuming callee has run, a failed acceptance proof cannot say
    // whether caller/callee cleanup still owns the same allocation. Capturing
    // this invariant failure would let ordinary cleanup double-drop it.
    const auto& reason=quidra::abi::failure_reason_info(quidra::abi::FailureReason::parallel_call_unaccepted);
    std::fputs("Quidra runtime error[",stderr);
    const auto code=quidra::abi::spelling(reason.code);
    std::fwrite(code.data(),1,code.size(),stderr);
    std::fputs("]: ",stderr);
    std::fwrite(reason.message.data(),1,reason.message.size(),stderr);
    std::fputc('\n',stderr);
    std::fflush(stderr);
    quidra::runtime::immediate_failure_exit();
}
