#pragma once

#include <atomic>
#include <mutex>
#include <utility>

namespace quidra::device::detail {
// Shared synchronization for lazy preparation and native command admission.
// The closure flag remains valid for the process. No closed entry runs its
// factory/operation; preparation checks again before publishing a candidate.
class CaptureGate {
public:
    explicit CaptureGate(std::atomic<bool>& closed) : closed_(closed) {}
    template<class Factory,class Publish,class Cancel>
    bool prepare(Factory&& factory,Publish&& publish,Cancel&& cancel) {
        std::lock_guard lock(mutex_);
        if(closed_.load(std::memory_order_acquire)) return false;
        auto candidate=factory();
        if(closed_.load(std::memory_order_acquire)) {
            cancel(std::move(candidate)); return false;
        }
        publish(std::move(candidate)); return true;
    }
    template<class Operation>
    auto command(Operation&& operation) -> decltype(operation()) {
        std::lock_guard lock(mutex_);
        if(closed_.load(std::memory_order_acquire)) return {};
        return operation();
    }
    // Closure is sticky even when a failure cannot acquire synchronization.
    // Already admitted native work is then explicitly unavailable/busy; the
    // caller retains its separate nonblocking owned-stop/snapshot protocol.
    std::unique_lock<std::mutex> close(bool nonblocking) {
        closed_.store(true,std::memory_order_release);
        std::unique_lock lock(mutex_,std::defer_lock);
        if(nonblocking) (void)lock.try_lock(); else lock.lock();
        return lock;
    }
private:
    std::atomic<bool>& closed_;
    std::mutex mutex_;
};
} // namespace quidra::device::detail
