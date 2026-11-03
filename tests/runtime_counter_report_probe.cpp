#include "runtime_counters.hpp"
#include "quidra/native_extension.h"

#include <cstdio>
#include <string_view>

extern "C" void quidra_parallel_call_require_accept(bool accepted);

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    quidra::counters::mark_program_started();
    quidra::counters::add(quidra::counters::Id::CoreDispatches, 3);
    quidra::counters::step_boundary(quidra::counters::StepEvent::Mark, "first");
    if (std::string_view(argv[1]) == "--failure") {
        quidra::counters::add(quidra::counters::Id::CoreDispatches, 2);
        quidra::counters::report_unlocated_failure(
            quidra::abi::FailureReason::tensor_failure, "forced counter failure", true);
    }
    if (std::string_view(argv[1]) == "--handoff-failure") {
        quidra::counters::add(quidra::counters::Id::CoreDispatches, 2);
        quidra_parallel_call_require_accept(false);
    }
    if (std::string_view(argv[1]) == "--native-failure") {
        quidra::counters::add(quidra::counters::Id::CoreDispatches, 2);
        std::fputs("forced package failure\n", stderr);
        qcore_runtime_immediate_failure_exit();
    }
    if (std::string_view(argv[1]) != "--wait") return 2;
    std::puts("ready");
    std::fflush(stdout);
    return std::getchar() == '\n' ? 0 : 2;
}
