#pragma once

#include <filesystem>
#include <memory>

namespace quidra::run_artifact {

void cleanup_stale() noexcept;

class TemporaryArtifact {
public:
    explicit TemporaryArtifact(const std::filesystem::path& source);
    ~TemporaryArtifact();

    TemporaryArtifact(const TemporaryArtifact&) = delete;
    TemporaryArtifact& operator=(const TemporaryArtifact&) = delete;
    TemporaryArtifact(TemporaryArtifact&&) = delete;
    TemporaryArtifact& operator=(TemporaryArtifact&&) = delete;

    const std::filesystem::path& executable() const noexcept { return executable_; }

private:
    struct LockState;

    std::filesystem::path executable_;
    std::filesystem::path llvm_;
    std::filesystem::path lease_path_;
    std::unique_ptr<LockState> lease_;
};

} // namespace quidra::run_artifact
