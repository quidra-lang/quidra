#pragma once
#include <cstdint>
#include <optional>

namespace quidra::device::detail {
// A timestamp covers an encoder, not necessarily one dispatch or caller.
// Counts saturate at two: only zero/one/multiple affect exact attribution.
class EncoderAttribution {
public:
    enum class Owner { Core,Package,Mixed };
    void core_dispatch(std::optional<std::uint32_t> pipeline) noexcept {
        if(core_<2) ++core_;
        pipeline_=core_==1 && package_==0 ? pipeline : std::nullopt;
    }
    void package_open() noexcept {
        if(package_<2) ++package_;
        pipeline_.reset();
    }
    Owner owner() const noexcept {
        if(core_ && package_) return Owner::Mixed;
        return package_ ? Owner::Package : Owner::Core;
    }
    bool source_available() const noexcept {
        return (core_==1 && package_==0) || (package_==1 && core_==0);
    }
    std::optional<std::uint32_t> kernel() const noexcept { return pipeline_; }
    const char* gap_reason() const noexcept {
        if(owner()==Owner::Mixed) return "mixed_encoder_attribution";
        if(core_>1) return "coarse_core_encoder_attribution";
        if(package_>1) return "coarse_package_encoder_attribution";
        if(core_==1 && !pipeline_) return "kernel_identity_unavailable";
        return nullptr;
    }
private:
    std::uint8_t core_{},package_{};
    std::optional<std::uint32_t> pipeline_;
};
} // namespace quidra::device::detail
