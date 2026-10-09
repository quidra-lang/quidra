#pragma once

// LoopTargets: where break and continue jump in the innermost loop being
// lowered, and what they emit before they jump. A loop enters its targets
// before lowering its body and leaves them after.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace quidra::lowering {

class LoopTargets {
public:
    void enter(const std::string& continue_label, const std::string& break_label) {
        targets_.push_back({continue_label, break_label, {}});
    }
    void leave() { targets_.pop_back(); }
    bool empty() const { return targets_.empty(); }
    const std::string& continue_target() const { return targets_.back().continue_label; }
    const std::string& break_target() const { return targets_.back().break_label; }

    // Code that a break or continue of the innermost loop emits before it
    // jumps (a reference loop's check of its array, control_flow_lowering.cpp);
    // an empty function emits nothing.
    void set_before_jump(std::function<void()> code) { targets_.back().before_jump = std::move(code); }
    void emit_before_jump() const {
        if (targets_.back().before_jump) targets_.back().before_jump();
    }

private:
    struct Targets {
        std::string continue_label;
        std::string break_label;
        std::function<void()> before_jump;
    };
    std::vector<Targets> targets_;
};

} // namespace quidra::lowering
