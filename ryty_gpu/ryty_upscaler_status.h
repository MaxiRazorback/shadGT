// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// ryGT FSR-204: the upscaler in use, shown in the game window's title ("ryGT - Gran Turismo Sport |
// FSR 3.1.4 temporal 1920x1080 -> 2560x1440"). The presenter reports it every frame; the window
// thread, woken by an SDL event, sets the title only when it changed.

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

namespace Vulkan::RytyUpscalerStatus {

inline std::mutex g_mutex;
inline std::string g_status;
inline bool g_changed{};

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
