#pragma once

#include "quidra/checker.hpp"
#include "quidra/diagnostic.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace quidra {

class PatchError final : public std::runtime_error {
public:
    PatchError(std::string code, std::string message, SourceSpan span = {}, std::string node_id = {})
        : std::runtime_error(message), code_(std::move(code)), span_(span), node_id_(std::move(node_id)) {}

    const std::string& code() const noexcept { return code_; }
    const SourceSpan& span() const noexcept { return span_; }
    const std::string& node_id() const noexcept { return node_id_; }

private:
    std::string code_;
    SourceSpan span_{};
    std::string node_id_;
};

struct PatchResult {
    std::string base_revision;
    std::string revision;
    std::string source;
};

PatchResult apply_source_patch(
    std::string_view source,
    const CheckedProgram& checked,
    std::string_view patch_json,
    std::function<void(std::string_view)> validator = {});

} // namespace quidra
