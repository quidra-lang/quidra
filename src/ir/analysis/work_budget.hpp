#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>

namespace quidra::ir::analysis {
struct WorkBudgetLimits {
    std::uint64_t maximum_operations{2'000'000};
    std::size_t maximum_encoded_bytes{64 * 1024 * 1024};
    std::size_t maximum_depth{256};
};
enum class WorkBudgetReason : std::uint8_t { Operations, Bytes, Depth };
class WorkBudgetExhausted final : public std::exception {
  public:
    explicit WorkBudgetExhausted(WorkBudgetReason value) noexcept : reason(value) {}
    const char* what() const noexcept override {
        return "IR analysis work budget exhausted";
    }
    WorkBudgetReason reason;
};
// A compilation request owns this counter; nested producers borrow it. Charge
// before work or allocation. Zero-cost cache hits remain available at a zero
// ceiling. This counter never certifies that a producer's accounting is complete.
class WorkBudget {
  public:
    explicit WorkBudget(WorkBudgetLimits limits = {}) noexcept : limits_(limits) {}
    WorkBudget(const WorkBudget&) = delete;
    WorkBudget& operator=(const WorkBudget&) = delete;
    WorkBudget(WorkBudget&&) = delete;
    WorkBudget& operator=(WorkBudget&&) = delete;
    void take(std::uint64_t count = 1) {
        if (count && reason_)
            throw WorkBudgetExhausted(*reason_);
        if (count > remaining_operations())
            fail(WorkBudgetReason::Operations);
        operations_ += count;
    }
    void bytes(std::size_t count) {
        if (count && reason_)
            throw WorkBudgetExhausted(*reason_);
        if (count > remaining_bytes())
            fail(WorkBudgetReason::Bytes);
        bytes_ += count;
    }
    void enter_depth() {
        if (reason_)
            throw WorkBudgetExhausted(*reason_);
        if (depth_ == limits_.maximum_depth)
            fail(WorkBudgetReason::Depth);
        ++depth_;
    }
    void leave_depth() noexcept {
        if (depth_)
            --depth_;
    }
    class DepthGuard {
      public:
        explicit DepthGuard(WorkBudget& budget) : budget_(budget) {
            budget_.enter_depth();
        }
        ~DepthGuard() {
            budget_.leave_depth();
        }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;

      private:
        WorkBudget& budget_;
    };
    std::uint64_t used_operations() const noexcept {
        return operations_;
    }
    std::uint64_t remaining_operations() const noexcept {
        return limits_.maximum_operations - operations_;
    }
    std::size_t used_bytes() const noexcept {
        return bytes_;
    }
    std::size_t remaining_bytes() const noexcept {
        return limits_.maximum_encoded_bytes - bytes_;
    }
    std::size_t current_depth() const noexcept {
        return depth_;
    }
    bool exhausted() const noexcept {
        return reason_.has_value();
    }
    std::optional<WorkBudgetReason> reason() const noexcept {
        return reason_;
    }

  private:
    [[noreturn]] void fail(WorkBudgetReason value) {
        if (!reason_)
            reason_ = value;
        throw WorkBudgetExhausted(*reason_);
    }
    WorkBudgetLimits limits_;
    std::uint64_t operations_{};
    std::size_t bytes_{}, depth_{};
    std::optional<WorkBudgetReason> reason_;
};
} // namespace quidra::ir::analysis
