// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT FSR-204: the upscaler in use, shown in the game window's title ("ryGT - Gran Turismo Sport |
// FSR 3.1.4 temporal 1920x1080 -> 2560x1440"). The presenter reports it every frame; the window
// thread, woken by an SDL event, sets the title only when it changed.

#pragma once

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

namespace Vulkan::RytyUpscalerStatus {

inline std::mutex g_mutex;
inline std::string g_status;
inline bool g_changed{};
// Observed rendered scene extent, not the full-resolution composition attachment.
inline std::atomic<std::uint64_t> g_scene_extent{};
inline void ObserveScene(unsigned width, unsigned height) {
    g_scene_extent.store((std::uint64_t(width) << 32) | height, std::memory_order_relaxed);
}


/// Presenter (any thread): the upscaler used for this frame.
inline void Report(std::string status) {
    {
        std::scoped_lock lock{g_mutex};
        if (status == g_status) {
            return;
        }
        g_status = std::move(status);
        g_changed = true;
    }
    SDL_Event wake{};
    wake.type = SDL_EVENT_USER;
    SDL_PushEvent(&wake);
}

inline void ReportResolution(unsigned output_width, unsigned output_height,
                             unsigned fallback_width, unsigned fallback_height,
                             std::string upscaler, bool generating_frames = false) {
    const auto scene = g_scene_extent.load(std::memory_order_relaxed);
    const unsigned width = scene ? unsigned(scene >> 32) : fallback_width;
    const unsigned height = scene ? unsigned(scene) : fallback_height;
    std::string status = std::to_string(output_width) + "x" + std::to_string(output_height) +
                         " -> " + std::to_string(width) + "x" + std::to_string(height);
    // The presenter supplies the backend that actually produced this frame.
    if (!upscaler.empty() && upscaler != "no upscaling" && upscaler != "high-resolution UI") {
        if (const auto suffix = upscaler.find(" temporal"); suffix != std::string::npos)
            upscaler.erase(suffix);
        if (upscaler.starts_with("FSR 3.1")) upscaler = "FSR 3.1";
        const double ratio = output_width && output_height
            ? std::max(double(width) / output_width, double(height) / output_height) : 1.0;
        const char* scale = "Custom";
        const struct { double ratio; const char* name; } presets[] = {
            {1.0, "Native"}, {2.0 / 3.0, "Quality"}, {1.0 / 1.7, "Balanced"},
            {0.5, "Performance"}, {1.0 / 3.0, "Ultra Performance"}};
        double best = 0.035;
        for (const auto& preset : presets) {
            const double difference = std::abs(ratio - preset.ratio);
            if (difference < best) { best = difference; scale = preset.name; }
        }
        if (upscaler == "DLSS" && std::string_view(scale) == "Native") upscaler = "DLAA";
        if (generating_frames) upscaler += " FG";
        status = upscaler + " | " + scale;
    }
    Report(std::move(status));
}

/// Window thread: the new status once, after it changed.
inline std::optional<std::string> Take() {
    std::scoped_lock lock{g_mutex};
    if (!g_changed) {
        return std::nullopt;
    }
    g_changed = false;
    return g_status;
}

} // namespace Vulkan::RytyUpscalerStatus
