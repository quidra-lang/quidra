#include "llvm_backend/tail_record_emitter.hpp"

#include "llvm_backend/runtime_abi.hpp"

namespace quidra::llvm_backend {
using namespace llvm_text;
using namespace llvm_text::types;

namespace {
constexpr std::string_view range_end = "__quidra_tail_range_end__";
}

std::string TailRecordEmitter::cfa() {
    const auto frame = names_.value("tail.cfa");
#if defined(__aarch64__) || defined(_M_ARM64)
    builder_.call(frame, ptr, LlvmOperand::global("llvm.sponentry"), {});
#else
    const auto address = names_.value("tail.return.address");
    builder_.call(address, ptr, LlvmOperand::global("llvm.addressofreturnaddress"), {});
    builder_.getelementptr(frame, Inbounds::no, i8, address, {{i64, 8}});
#endif
    return frame;
}

void TailRecordEmitter::record(std::uint32_t site, bool self) {
    const auto frame = cfa();
    std::string depth = "0";
    if (self_depth_) {
        depth = names_.value("tail.depth");
        builder_.cast(depth, CastOp::trunc, {i64, "%quidra.depth"}, i32);
    }
    builder_.call(runtime_abi::tail::record,
                  {{i32, "__quidra_tail_site_"+std::to_string(site)+"__"}, {i32, depth}, {ptr, frame}, {i32, self ? 1 : 0}});
}

void TailRecordEmitter::normal_return() {
    // R3 has no TLS read on an activation that made no self call.
    const auto active = names_.value("tail.self.active");
    const auto cleanup = names_.label("tail.self.cleanup");
    const auto resumed = names_.label("tail.self.return");
    builder_.icmp(active, IntPredicate::ugt, i64, "%quidra.depth", 1);
    builder_.br(active, cleanup, resumed);
    builder_.block(cleanup);
    const auto frame = cfa();
    builder_.call(runtime_abi::tail::return_cleanup,
                  {{i32, "__quidra_tail_begin_"+std::to_string(owner_)+"__"},
                   {i32, "__quidra_tail_end_"+std::to_string(owner_)+"__"}, {ptr, frame}});
    builder_.br(resumed);
    builder_.block(resumed);
}

void TailRecordEmitter::resolve_range(std::string& text, std::uint32_t end) {
    const auto value = std::to_string(end);
    for (auto at = text.find(range_end); at != std::string::npos; at = text.find(range_end, at))
        text.replace(at, range_end.size(), value);
}

} // namespace quidra::llvm_backend
