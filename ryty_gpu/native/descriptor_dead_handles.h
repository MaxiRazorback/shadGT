// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

// PERF-GTS-375: descriptor sets kept across command lists hold Vulkan handles. A set is dropped
// before any buffer, image view, sampler or set layout it was written or allocated with is
// destroyed: the driver may give a new object the same handle value, and a kept set found by
// that value would point at the destroyed object. Every such destroy notes the handle here
// first; the recorder takes the notes before it looks a kept set up.
namespace Native::DeadHandles {
inline std::atomic<bool> enabled{false};
inline std::atomic<bool> pending{false};
inline std::mutex mutex;
inline std::vector<std::uint64_t> handles;

/// Any thread, before the handle is destroyed.
template <class Handle>
void Note(Handle handle) {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    const auto value = reinterpret_cast<std::uint64_t>(handle);
    if (value == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    handles.push_back(value);
    pending.store(true, std::memory_order_release);
}

/// The recorder: the handles noted since the last call. False when there are none.
inline bool Take(std::vector<std::uint64_t>& out) {
    if (!pending.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    out.clear();
    std::scoped_lock lock{mutex};
    out.swap(handles);
    return true;
}
} // namespace Native::DeadHandles
