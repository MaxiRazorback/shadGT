// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>

#include "common/types.h"
#include "video_core/renderer_vulkan/compiler_worker_budget.h"

namespace Vulkan {

/// FIX-GT7-100: pipeline builds queued or running, for the compile notice.
inline std::atomic<u32> g_build_jobs{0};

/// FIX-GT7-109: the game is in play (a race: slow frames for a second; FIX-GT7-120: kept by a
/// heavy scene), as PipelineCache::UpdateCatchUp measures it. Compile work then stays on its
/// cores at its usual width, and background compiles that read it pause.
inline std::atomic<bool> g_game_in_play{false};

/// The pipeline build workers' jobs (PERF-019). Urgent jobs (a draw waits) run last-first,
/// then FIX-GT7-009's Screen jobs (draws skipped again) in order, then Soon jobs (a draw went
/// on, PERF-GT7-025) in draw order, then PERF-171's tessellated read-ahead jobs (the cars), then
/// normal read-ahead in order. FIX-GT7-022: read-ahead jobs start only while fewer than
/// `background_limit` jobs run.
/// PERF-038: background jobs (stored pipelines built while
/// the game runs) run only when neither waits, on at most `max_background` workers at once,
/// and not while paused: each pipeline a draw needs pauses them for a while, so they never
/// compete with the builds the game is waiting for.
class BuildQueue {
public:
    using Job = std::function<void()>;
    using Clock = std::chrono::steady_clock;
    enum class Kind { Urgent, Screen, Soon, Tessellated, Normal, Background };

    explicit BuildQueue(u32 max_foreground_, u32 max_background_, u32 max_boost_ = 0)
        : max_foreground{max_foreground_}, max_background{max_background_},
          max_boost{std::max(max_background_, max_boost_)} {}

    /// PERF-098: while the splash screen shows, background jobs run on every worker and are
    /// never paused (no game draw waits for a pipeline yet).
    bool Boosted() const {
        return boost.load(std::memory_order_relaxed);
    }

    void SetBoost(bool on) {
        if (boost.exchange(on) != on) {
            cv.notify_all();
        }
    }

    void Push(Job&& job, Kind kind, bool heavy = false) {
        {
            std::scoped_lock lk{mutex};
            switch (kind) {
            case Kind::Urgent:
                urgent.push_front({std::move(job), heavy});
                break;
            case Kind::Screen:
                screen.push_back({std::move(job), heavy});
                break;
            case Kind::Soon:
                soon.push_back({std::move(job), heavy});
                break;
            case Kind::Tessellated:
                tessellated.push_back({std::move(job), heavy});
                break;
            case Kind::Normal:
                normal.push_back({std::move(job), heavy});
                break;
            case Kind::Background:
                background.push_back({std::move(job), heavy});
                break;
            }
        }
        g_build_jobs.fetch_add(1, std::memory_order_relaxed); // FIX-GT7-100
        cv.notify_all();
    }

    /// Waits for the next job. False once stopped. `is_background` tells which queue it came
    /// from; a background job must be followed by FinishedBackground, any other by
    /// FinishedForeground.
    bool Pop(Job& job, bool& is_background, std::stop_token stop, bool* is_heavy = nullptr, bool* is_read_ahead = nullptr) {
        std::unique_lock lk{mutex};
        while (true) {
            if (stop.stop_requested()) {
                return false;
            }
            if (auto* entries = RunnableForeground(); entries && HasCapacity(running_foreground, max_foreground + extra_running)) {
                auto next = Runnable(*entries);
                const bool heavy = next->heavy;
                job = std::move(next->job);
                entries->erase(next);
                running_heavy += heavy;
                if (is_heavy) *is_heavy = heavy;
                is_background = false;
                const bool read_ahead = entries == &normal || entries == &tessellated;
                if (is_read_ahead) *is_read_ahead = read_ahead;
                running_read_ahead += read_ahead;
                ++running_foreground;
                return true;
            }
            const auto now = Clock::now();
            const bool boosted = boost.load(std::memory_order_relaxed);
            const bool paused = !boosted && now < pause_until;
            const u32 limit = boosted ? max_boost : max_background;
            if (auto next = Runnable(background); next != background.end() && !paused && ReadAheadAvailable() && HasCapacity(running_background, limit)) {
                const bool heavy = next->heavy;
                job = std::move(next->job);
                background.erase(next);
                running_heavy += heavy;
                if (is_heavy) *is_heavy = heavy;
                ++running_background;
                ++running_read_ahead;
                if (is_read_ahead) *is_read_ahead = true;
                is_background = true;
                return true;
            }
            if (!background.empty() && paused) {
                cv.wait_until(lk, stop, pause_until, [&] {
                    return (RunnableForeground() && HasCapacity(running_foreground, max_foreground + extra_running)) ||
                           boost.load(std::memory_order_relaxed) || Clock::now() >= pause_until;
                });
            } else {
                cv.wait(lk, stop, [&] {
                    const u32 limit = boost.load(std::memory_order_relaxed) ? max_boost
                                                                            : max_background;
                    return (RunnableForeground() && HasCapacity(running_foreground, max_foreground + extra_running)) ||
                           (Runnable(background) != background.end() && ReadAheadAvailable() && HasCapacity(running_background, limit));
                });
            }
        }
    }

    void FinishedForeground(bool heavy = false, bool read_ahead = false) {
        {
            std::scoped_lock lk{mutex};
            --running_foreground;
            running_read_ahead -= read_ahead;
            running_heavy -= heavy;
        }
        g_build_jobs.fetch_sub(1, std::memory_order_relaxed); // FIX-GT7-100
        cv.notify_all();
    }

    /// PERF-101: no draw or read-ahead build is queued or running.
    bool ForegroundIdle() {
        std::scoped_lock lk{mutex};
        return urgent.empty() && screen.empty() && soon.empty() && tessellated.empty() &&
               normal.empty() && running_foreground == 0;
    }

    void FinishedBackground(bool heavy = false) {
        {
            std::scoped_lock lk{mutex};
            --running_background;
            --running_read_ahead;
            running_heavy -= heavy;
        }
        g_build_jobs.fetch_sub(1, std::memory_order_relaxed); // FIX-GT7-100
        cv.notify_all();
    }

    /// No background job starts before `until` (jobs already running finish).
    void PauseBackground(Clock::time_point until) {
        std::scoped_lock lk{mutex};
        pause_until = std::max(pause_until, until);
    }

    size_t BackgroundQueued() {
        std::scoped_lock lk{mutex};
        return background.size();
    }

    /// PERF-160: jobs waiting for a worker, foreground and background.
    size_t Pending() {
        std::scoped_lock lk{mutex};
        return urgent.size() + screen.size() + soon.size() + tessellated.size() + normal.size() +
               background.size();
    }

    bool Busy() {
        std::scoped_lock lk{mutex};
        return running_foreground || running_background || !urgent.empty() || !screen.empty() ||
               !soon.empty() || !tessellated.empty() || !normal.empty() || !background.empty();
    }

    void SetForegroundLimit(u32 limit) {
        { std::scoped_lock lk{mutex}; max_foreground = std::max(1u, limit); }
        cv.notify_all();
    }

    /// PERF-GT7-034: the limit itself (fewer while the game runs late).
    void SetMaxRunning(u32 limit) {
        SetForegroundLimit(limit);
    }

    /// PERF-GT7-021: this many more jobs may run at once (0: back to the limit).
    void SetExtraRunning(u32 extra) {
        { std::scoped_lock lk{mutex}; extra_running = extra; }
        cv.notify_all();
    }

    /// FIX-GT7-022: a read-ahead job starts only while fewer than this many jobs run.
    void SetBackgroundLimit(u32 limit) {
        { std::scoped_lock lk{mutex}; background_limit = limit; }
        cv.notify_all();
    }

    void SetReadAheadLimit(u32 limit) {
        { std::scoped_lock lk{mutex}; max_read_ahead = std::max(1u, limit); }
        cv.notify_all();
    }

    /// Wakes the workers waiting for jobs or a pause, so they can see a stop request.
    void NotifyAll() {
        cv.notify_all();
    }

private:
    u32 max_foreground;
    const u32 max_background;
    const u32 max_boost;
    std::atomic<bool> boost{false};
    std::mutex mutex;
    std::condition_variable_any cv;
    bool HasCapacity(u32 running_kind, u32 kind_limit) const {
        const u32 total_limit = (boost.load(std::memory_order_relaxed)
                                     ? std::max(max_foreground, max_boost)
                                     : max_foreground) +
                                extra_running;
        return running_kind < kind_limit &&
               running_foreground + running_background < total_limit;
    }
    bool ReadAheadAvailable() const {
        return boost.load(std::memory_order_relaxed) ||
               (running_read_ahead < max_read_ahead &&
                running_foreground + running_background < background_limit);
    }
    struct Entry { Job job; bool heavy; };
    std::deque<Entry> urgent;
    std::deque<Entry> screen;      ///< FIX-GT7-009
    std::deque<Entry> soon;        ///< PERF-GT7-025
    std::deque<Entry> tessellated; ///< PERF-171
    std::deque<Entry> normal;
    std::deque<Entry> background;
    // Keep queued heavy jobs out of worker threads until memory capacity is available.
    std::deque<Entry>::iterator Runnable(std::deque<Entry>& entries) {
        return std::find_if(entries.begin(), entries.end(), [this](const Entry& entry) {
            return !entry.heavy || running_heavy < CompilerWorkerBudget::HeavyPipelineLimit;
        });
    }
    // PERF-109 (WillScarlettOhara, PERF-GT7-025): preserve priority while allowing
    // runnable lighter jobs to bypass heavy jobs blocked by the shared budget.
    std::deque<Entry>* RunnableForeground() {
        for (auto* entries : {&urgent, &screen, &soon}) {
            if (Runnable(*entries) != entries->end()) return entries;
        }
        if (!ReadAheadAvailable()) return nullptr;
        for (auto* entries : {&tessellated, &normal}) {
            if (Runnable(*entries) != entries->end()) return entries;
        }
        return nullptr;
    }
    u32 max_read_ahead{UINT32_MAX};
    u32 background_limit{~0u}; ///< FIX-GT7-022
    u32 extra_running{};       ///< PERF-GT7-021
    u32 running_read_ahead{};
    u32 running_heavy{};
    u32 running_background{};
    u32 running_foreground{};
    Clock::time_point pause_until{};
};

} // namespace Vulkan
