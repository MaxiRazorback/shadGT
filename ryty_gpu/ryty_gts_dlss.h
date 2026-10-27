// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include "ryty_gpu/ryty_dlss.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"

namespace Vulkan {
class Instance;
class Runtime;

class GtsDlssPass {
public:
    GtsDlssPass(const Instance& instance, Scheduler& scheduler, Runtime& runtime);
    void SetOutputSize(vk::Extent2D size, bool hdrEncoded) {
        requestedSize = size;
        supportedDisplay = !hdrEncoded;
    }
    bool Capture(u64 frame, VideoCore::Image& color, VideoCore::Image& depth,
                 VideoCore::Image& motion, VideoCore::Image& resolved,
                 const std::array<float, 28>& constants);
    bool HasScene(u64 frame, u64 resolvedUid) const;
    void PrepareScene(u64 frame, VideoCore::Image& resolved);
    void Resolve(u64 frame, VideoCore::Image& display);
    bool OverrideHud(u64 frame, u64 uid, RenderState& state);
    void FallbackHud(u64 frame, VideoCore::Image& display, vk::ImageLayout restoreLayout);
    void SyncGuestRead(u64 frame, VideoCore::Image& display);
    void ScaleHud(u64 frame, u64 uid, Viewports& viewports, Scissors& scissors) const;
    vk::ImageView PreparePresent(u64 frame, VideoCore::Image& display, vk::Format viewFormat);
    vk::Extent2D OutputSize() const { return outputSize; }
    bool HasDisplay(u64 frame, u64 uid) const { return isActive(frame, uid); }

private:
    struct Texture {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::UniqueImageView presentView;
        vk::Format format{};
        vk::Extent2D size{};
        vk::ImageAspectFlags aspects{vk::ImageAspectFlagBits::eColor};
        vk::ImageLayout layout{vk::ImageLayout::eUndefined};
        RygtDlssImage Descriptor() const;
    };
    void create(Texture& texture, vk::Format format, vk::Extent2D size,
                vk::ImageAspectFlags aspects = vk::ImageAspectFlagBits::eColor);
    void transit(Texture& texture, vk::ImageLayout layout);
    void copyInput(VideoCore::Image& source, Texture& dest);
    void copyBack(VideoCore::Image& display);
    bool isActive(u64 frame, u64 uid) const;

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    Texture color, depth, motion, reconstruction, native;
    std::unique_ptr<VideoCore::BlitHelper> colorCopy;
    vk::Extent2D requestedSize{}, renderSize{}, outputSize{};
    RygtDlssQuality quality{RYGT_DLSS_QUALITY_PERFORMANCE};
    u64 sceneFrame{~u64{}}, sceneUid{}, displayUid{}, outputFrame{~u64{}};
    float jitterX{}, jitterY{}, frameTimeMs{1000.f / 60};
    bool reset{true}, failed{}, supportedDisplay{}, nativeDirty{};
    u64 evaluatedFrames{}, nativePresentations{}, guestSyncs{}, nativeHudDraws{}, fallbacks{};
    std::chrono::steady_clock::time_point lastScene{};
};
} // namespace Vulkan
