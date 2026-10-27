// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// UI-HIRES (ryGT): GT Sport's UI drawn at the output size. After a flip, the first draw into a
// display buffer is the scene composite. Before the next draw into it, DLSS (or a linear blit
// when DLSS cannot scale) scales that picture into a proxy image at the output size, and the
// remaining draws into the display buffer (the UI) render into the proxy with their viewport and
// scissor scaled. The presenter then shows the proxy instead of scaling the display buffer.
// A draw the proxy cannot take (more targets, a depth buffer) resolves the proxy back into the
// display buffer and the frame continues the normal way. GPU.ui_hires in
// user\custom_configs\CUSA03220.json (the launcher's option) or RYGT_UI_HIRES=1 turns it on.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "ryty_gpu/ryty_dlss.h"
#include "ryty_gpu/ryty_pro_mode.h"
#include "ryty_gpu/ryty_hdr_ui.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan::UiHires {

enum class Phase {
    Idle,  ///< nothing drawn into the display buffer since its last flip
    Scene, ///< the scene composite was drawn into the display buffer
    Ui,    ///< the proxy holds the picture; display buffer draws go to the proxy
    Off,   ///< the proxy was given up for this frame; the display buffer holds the picture
};

struct Proxy {
    vk::Extent2D native{}; ///< the display buffer's size as the presenter shows it
    vk::Extent2D size{};   ///< the output (window or screen) size; empty when not used
    vk::Format format{};
    VideoCore::UniqueImage image;
    VideoCore::UniqueImage scaled; ///< DLSS output (R16G16B16A16), copied into image
    vk::UniqueImageView scaled_view;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniqueImageView>> views;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    // HDR-UI: the UI layer (RGBA16F, SDR colours and coverage), composed by the presenter.
    VideoCore::UniqueImage ui;
    vk::UniqueImageView ui_view;
    vk::ImageLayout ui_layout = vk::ImageLayout::eUndefined;
    bool ui_used = false;
    Phase phase = Phase::Idle;
    bool dlss_failed = false;
    bool dlss_used = false;
    bool native_stale = false; ///< the proxy got draws the display buffer has not
};

struct Retired {
    VideoCore::UniqueImage image;
    VideoCore::UniqueImage scaled;
    vk::UniqueImageView scaled_view;
    VideoCore::UniqueImage ui;
    vk::UniqueImageView ui_view;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniqueImageView>> views;
    u64 frame = 0;
};

struct State {
    std::mutex mutex;
    std::unordered_map<VAddr, Proxy> proxies;
    std::deque<Retired> retired; ///< freed some frames later: the GPU may still read them
    u64 frames = 0;
    /// Nonzero while a draw into the proxy is recorded: its viewport and scissor scale.
    float scale = 0.0f;
    /// Lock-free checks for every draw: the display buffer addresses with a proxy, and how
    /// many proxies hold draws their display buffer has not.
    std::array<std::atomic<VAddr>, 8> addresses{};
    std::atomic<u32> stale{};
};

inline State& Get() {
    static State state;
    return state;
}

inline void MarkStale(Proxy& proxy, bool stale) {
    if (proxy.native_stale != stale) {
        proxy.native_stale = stale;
        if (stale) {
            Get().stale.fetch_add(1, std::memory_order_relaxed);
        } else {
            Get().stale.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

inline bool KnownAddress(VAddr address) {
    for (const auto& known : Get().addresses) {
        if (known.load(std::memory_order_relaxed) == address) {
            return true;
        }
    }
    return false;
}

inline bool Enabled() {
    static const bool enabled = [] {
        if (const char* env = std::getenv("RYGT_UI_HIRES")) {
            return env[0] == '1';
        }
        return Ryty::ConfigTrue("ui_hires");
    }();
    return enabled;
}

inline void Retire(State& state, Proxy& proxy) {
    if (!proxy.image && !proxy.scaled) {
        return;
    }
    state.retired.push_back(Retired{std::move(proxy.image), std::move(proxy.scaled),
                                    std::move(proxy.scaled_view), std::move(proxy.ui),
                                    std::move(proxy.ui_view), std::move(proxy.views),
                                    state.frames});
    proxy.views.clear();
    proxy.layout = vk::ImageLayout::eUndefined;
    proxy.ui_layout = vk::ImageLayout::eUndefined;
}

inline vk::ImageView View(const Instance& instance, Proxy& proxy, vk::Format format,
                          bool alpha_one) {
    const auto key = std::make_pair(format, alpha_one);
    for (const auto& [k, view] : proxy.views) {
        if (k == key) {
            return *view;
        }
    }
    auto view = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = proxy.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components{.a = alpha_one ? vk::ComponentSwizzle::eOne : vk::ComponentSwizzle::eIdentity},
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (view.result != vk::Result::eSuccess) {
        return {};
    }
    const vk::ImageView handle = *view.value;
    proxy.views.emplace_back(key, std::move(view.value));
    return handle;
}

inline bool CreateImages(const Instance& instance, Proxy& proxy, vk::Format format) {
    if (proxy.image && proxy.format == format) {
        return true;
    }
    Retire(Get(), proxy);
    proxy.format = format;
    proxy.image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    proxy.image.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent{.width = proxy.size.width, .height = proxy.size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    proxy.scaled = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    proxy.scaled.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{.width = proxy.size.width, .height = proxy.size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto view = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = proxy.scaled,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (view.result != vk::Result::eSuccess) {
        return false;
    }
    proxy.scaled_view = std::move(view.value);
    proxy.layout = vk::ImageLayout::eUndefined;
    proxy.ui = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    proxy.ui.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent{.width = proxy.size.width, .height = proxy.size.height, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    auto ui_view = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
        .image = proxy.ui,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        },
    });
    if (ui_view.result != vk::Result::eSuccess) {
        return false;
    }
    proxy.ui_view = std::move(ui_view.value);
    proxy.ui_layout = vk::ImageLayout::eUndefined;
    LOG_INFO(Render_Vulkan, "UI-HIRES: proxy {}x{} for a {}x{} display buffer, {}",
             proxy.size.width, proxy.size.height, proxy.native.width, proxy.native.height,
             vk::to_string(format));
    return true;
}

constexpr vk::ImageSubresourceRange ColorRange{
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

inline void ProxyBarrier(vk::CommandBuffer cmdbuf, Proxy& proxy, vk::ImageLayout layout,
                         vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = proxy.layout,
        .newLayout = layout,
        .image = proxy.image,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
    proxy.layout = layout;
}

constexpr vk::PipelineStageFlags2 DrawStages =
    vk::PipelineStageFlagBits2::eColorAttachmentOutput |
    vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eVertexShader;
constexpr vk::AccessFlags2 DrawAccess = vk::AccessFlagBits2::eColorAttachmentRead |
                                        vk::AccessFlagBits2::eColorAttachmentWrite |
                                        vk::AccessFlagBits2::eShaderSampledRead;

/// HDR-UI: the UI layer starts each frame transparent.
inline void ClearUiLayer(vk::CommandBuffer cmdbuf, Proxy& proxy) {
    const vk::ImageMemoryBarrier2 to_clear{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = proxy.ui,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_clear,
    });
    cmdbuf.clearColorImage(proxy.ui, vk::ImageLayout::eGeneral,
                           vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}}, ColorRange);
    const vk::ImageMemoryBarrier2 to_draw{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = DrawStages,
        .dstAccessMask = DrawAccess,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = proxy.ui,
        .subresourceRange = ColorRange,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &to_draw,
    });
    proxy.ui_layout = vk::ImageLayout::eGeneral;
    proxy.ui_used = false;
}

/// HDR-UI: a frame whose first draw is UI: the scene is black.
inline void StartWithoutScene(Scheduler& scheduler, Proxy& proxy) {
    proxy.dlss_used = false;
    scheduler.EndRendering();
    const vk::CommandBuffer cmdbuf = scheduler.DirectCommandBuffer();
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
    cmdbuf.clearColorImage(proxy.image, vk::ImageLayout::eGeneral,
                           vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}}, ColorRange);
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    ClearUiLayer(cmdbuf, proxy);
    proxy.phase = Phase::Ui;
}

/// The scene composite in the display buffer `image` becomes the proxy's picture.
inline void Upscale(const Instance& instance, Scheduler& scheduler, Runtime& runtime,
                    VideoCore::Image& image, const VideoCore::ImageViewInfo& target_view,
                    Proxy& proxy) {
    scheduler.EndRendering();
    const bool dlss =
        !proxy.dlss_failed && RytyDlss::CanScale(proxy.native, proxy.size);
    if (dlss) {
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead);
    } else {
        runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer,
                        vk::AccessFlagBits2::eTransferRead);
    }
    runtime.FlushBarriers();
    const vk::CommandBuffer cmdbuf = scheduler.DirectCommandBuffer();
    bool scaled = false;
    if (dlss) {
        VideoCore::ImageViewInfo info{};
        info.format = target_view.format;
        const vk::ImageView input = *image.FindView(info).image_view;
        scaled = RytyDlss::ScaleInto(cmdbuf, image.GetImage(), input, info.format, proxy.native,
                                     proxy.scaled, *proxy.scaled_view, proxy.size);
        if (!scaled) {
            proxy.dlss_failed = true;
        }
    }
    proxy.dlss_used = scaled;
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferDstOptimal,
                 vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageBlit2 region{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 scaled ? vk::Offset3D{s32(proxy.size.width),
                                                       s32(proxy.size.height), 1}
                                        : vk::Offset3D{s32(proxy.native.width),
                                                       s32(proxy.native.height), 1}},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{s32(proxy.size.width), s32(proxy.size.height), 1}},
    };
    if (scaled) {
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .image = proxy.scaled,
            .subresourceRange = ColorRange,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_copy,
        });
        cmdbuf.blitImage2(vk::BlitImageInfo2{
            .srcImage = proxy.scaled,
            .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
            .dstImage = proxy.image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = 1,
            .pRegions = &region,
            .filter = vk::Filter::eNearest,
        });
    } else {
        if (dlss) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer,
                            vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
        }
        cmdbuf.blitImage2(vk::BlitImageInfo2{
            .srcImage = image.GetImage(),
            .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
            .dstImage = proxy.image,
            .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
            .regionCount = 1,
            .pRegions = &region,
            .filter = vk::Filter::eLinear,
        });
    }
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    ClearUiLayer(cmdbuf, proxy);
    proxy.phase = Phase::Ui;
}

/// The proxy's picture goes back into the display buffer; the frame continues without it.
/// With `keep`, the proxy stays in use: the display buffer only gets a copy for draws that
/// read it (they sample in the game's coordinates).
inline void Resolve(Scheduler& scheduler, Runtime& runtime, VideoCore::Image& image,
                    Proxy& proxy, bool keep = false) {
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    const vk::CommandBuffer cmdbuf = scheduler.DirectCommandBuffer();
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eTransferSrcOptimal,
                 vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    const vk::ImageBlit2 region{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{s32(proxy.size.width), s32(proxy.size.height), 1}},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{s32(proxy.native.width), s32(proxy.native.height),
                                              1}},
    };
    cmdbuf.blitImage2(vk::BlitImageInfo2{
        .srcImage = proxy.image,
        .srcImageLayout = vk::ImageLayout::eTransferSrcOptimal,
        .dstImage = image.GetImage(),
        .dstImageLayout = vk::ImageLayout::eTransferDstOptimal,
        .regionCount = 1,
        .pRegions = &region,
        .filter = vk::Filter::eLinear,
    });
    ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eGeneral, DrawStages, DrawAccess);
    MarkStale(proxy, false);
    if (!keep) {
        proxy.phase = Phase::Off;
    }
}

struct Redirect {
    Proxy* proxy = nullptr;
    VideoCore::ImageId image_id{};
    u32 cb = 0;
    bool ui = false;   ///< HDR-UI: into the UI layer
    bool skip = false; ///< HDR-UI: a UI-layer draw with no layer to draw into
};

/// Draws that read a display buffer whose proxy holds newer draws read it in the game's
/// coordinates: the proxy is copied into the display buffer first, and stays in use.
template <typename Ctx>
bool RefreshSampled(Ctx& ctx, State& state, Scheduler& scheduler, Runtime& runtime,
                    VideoCore::TextureCache& texture_cache) {
    bool refreshed = false;
    for (const auto& bound : ctx.bound_textures) {
        auto& image = texture_cache.GetImage(bound.image_id);
        const auto found = state.proxies.find(image.info.guest_address);
        if (found != state.proxies.end() && found->second.phase == Phase::Ui &&
            found->second.native_stale) {
            Resolve(scheduler, runtime, image, found->second, true);
            refreshed = true;
        }
    }
    return refreshed;
}

/// Before a compute dispatch: the same for display buffers it reads.
template <typename Ctx>
void BeforeDispatch(Ctx& ctx, Scheduler& scheduler, Runtime& runtime,
                    VideoCore::TextureCache& texture_cache) {
    if (!Enabled() || Get().stale.load(std::memory_order_relaxed) == 0) {
        return;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    if (state.proxies.empty() || !RefreshSampled(ctx, state, scheduler, runtime, texture_cache)) {
        return;
    }
    // The dispatch's image layouts were set before the copy: set them again for what it binds.
    for (const auto& bound : ctx.bound_textures) {
        auto& image = texture_cache.GetImage(bound.image_id);
        if (!state.proxies.contains(image.info.guest_address)) {
            continue;
        }
        runtime.Transit(&image, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
        ctx.image_infos[bound.descriptor_index].imageLayout = vk::ImageLayout::eGeneral;
    }
    runtime.FlushBarriers();
}

/// Before a draw's targets are prepared: whether it renders into a proxy. `ctx` is the
/// rasterizer's draw context (cb_descs, db_desc).
template <typename Ctx>
Redirect Prepare(Ctx& ctx, u32 num_color, bool ui_layer, const Instance& instance,
                 Scheduler& scheduler, Runtime& runtime, VideoCore::TextureCache& texture_cache,
                 bool external_display = false) {
    auto& state = Get();
    state.scale = 0.0f;
    if (!Enabled()) {
        return Redirect{.skip = ui_layer};
    }
    if (external_display) {
        std::scoped_lock lock{state.mutex};
        const auto id = ctx.cb_descs[0].image_id;
        if (id) {
            const auto found = state.proxies.find(texture_cache.GetImage(id).info.guest_address);
            if (found != state.proxies.end()) {
                found->second.phase = Phase::Off;
                MarkStale(found->second, false);
            }
        }
        return Redirect{.skip = ui_layer};
    }
    if (state.stale.load(std::memory_order_relaxed) == 0) {
        // Most draws neither target nor read a display buffer: no lock for them.
        bool display = false;
        for (u32 cb = 0; cb < num_color && !display; ++cb) {
            const auto& [image_id, desc] = ctx.cb_descs[cb];
            display = image_id && KnownAddress(texture_cache.GetImage(image_id).info.guest_address);
        }
        if (!display) {
            return Redirect{.skip = ui_layer};
        }
    }
    std::scoped_lock lock{state.mutex};
    if (state.proxies.empty()) {
        return Redirect{.skip = ui_layer};
    }
    RefreshSampled(ctx, state, scheduler, runtime, texture_cache);
    for (u32 cb = 0; cb < num_color; ++cb) {
        const auto& [image_id, desc] = ctx.cb_descs[cb];
        if (!image_id) {
            continue;
        }
        auto& image = texture_cache.GetImage(image_id);
        const auto found = state.proxies.find(image.info.guest_address);
        if (found == state.proxies.end()) {
            continue;
        }
        auto& proxy = found->second;
        if (proxy.size.width == 0) {
            return Redirect{.skip = ui_layer};
        }
        if (ui_layer) {
            // HDR-UI: UI goes to the UI layer; its pipeline only fits that layer.
            if (num_color != 1 || ctx.db_desc.first || proxy.phase == Phase::Off ||
                !CreateImages(instance, proxy, image.info.pixel_format)) {
                return Redirect{.skip = true};
            }
            if (proxy.phase == Phase::Idle) {
                StartWithoutScene(scheduler, proxy);
            } else if (proxy.phase == Phase::Scene) {
                Upscale(instance, scheduler, runtime, image, desc.view_info, proxy);
            }
            proxy.ui_used = true;
            return Redirect{&proxy, image_id, cb, true};
        }
        switch (proxy.phase) {
        case Phase::Idle:
            proxy.phase = Phase::Scene;
            return {};
        case Phase::Off:
            return {};
        case Phase::Scene:
            if (num_color != 1 || ctx.db_desc.first) {
                proxy.phase = Phase::Off;
                return {};
            }
            if (!CreateImages(instance, proxy, image.info.pixel_format)) {
                proxy.size = {};
                proxy.phase = Phase::Off;
                return {};
            }
            Upscale(instance, scheduler, runtime, image, desc.view_info, proxy);
            break;
        case Phase::Ui:
            if (num_color != 1 || ctx.db_desc.first) {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    LOG_WARNING(Render_Vulkan,
                                "UI-HIRES: a draw with {} targets and depth {} into the display "
                                "buffer; the frame goes on without the proxy",
                                num_color, bool(ctx.db_desc.first));
                }
                Resolve(scheduler, runtime, image, proxy);
                return {};
            }
            break;
        }
        MarkStale(proxy, true);
        return Redirect{&proxy, image_id, cb};
    }
    return {};
}

/// After the draw's targets are prepared: it renders into the proxy. Reads of the display buffer
/// stay on the display buffer (Prepare copied the proxy into it).
template <typename Ctx>
void Apply(const Redirect& redirect, Ctx& ctx, RenderState& render_state,
           const Instance& instance) {
    if (!redirect.proxy) {
        return;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    auto& proxy = *redirect.proxy;
    const auto& desc = ctx.cb_descs[redirect.cb].desc;
    auto& attachment = render_state.color_attachments[redirect.cb];
    attachment.image_view = redirect.ui ? *proxy.ui_view
                                        : View(instance, proxy, desc.view_info.format, false);
    attachment.image_layout = vk::ImageLayout::eGeneral;
    render_state.width = u16(proxy.size.width);
    render_state.height = u16(proxy.size.height);
    state.scale = float(proxy.size.width) / float(proxy.native.width);
}

struct Shown {
    vk::ImageView view;
    vk::Extent2D size;
    vk::ImageView ui; ///< HDR-UI: the UI layer to compose over the scene, or null
    bool dlss = false;
};

/// At a flip of the display buffer at `address`: the proxy to show instead, if it holds the
/// picture. Also sizes the proxy for the next frame from the display buffer and output sizes.
inline std::optional<Shown> TakeFrame(const Instance& instance, vk::CommandBuffer cmdbuf,
                                      VAddr address, vk::Extent2D native, vk::Extent2D output,
                                      vk::Format view_format) {
    if (!Enabled()) {
        return std::nullopt;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.frames;
    while (!state.retired.empty() && state.frames - state.retired.front().frame > 8) {
        state.retired.pop_front();
    }
    auto& proxy = state.proxies[address];
    if (!KnownAddress(address)) {
        for (auto& known : state.addresses) {
            VAddr empty = 0;
            if (known.compare_exchange_strong(empty, address)) {
                break;
            }
        }
    }
    std::optional<Shown> shown;
    if (proxy.phase == Phase::Ui && proxy.image && proxy.size == output &&
        proxy.native == native) {
        ProxyBarrier(cmdbuf, proxy, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eFragmentShader |
                         vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderRead);
        shown = Shown{View(instance, proxy, view_format, true), proxy.size, {}, proxy.dlss_used};
        if (proxy.ui_used && proxy.ui) {
            const vk::ImageMemoryBarrier2 to_read{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = proxy.ui_layout,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = proxy.ui,
                .subresourceRange = ColorRange,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &to_read,
            });
            proxy.ui_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            shown->ui = *proxy.ui_view;
        }
    }
    proxy.phase = Phase::Idle;
    MarkStale(proxy, false);
    // Only an upscale of the same shape: a bigger window or screen of the game's aspect ratio.
    const bool wanted = output.width > native.width && output.height > native.height &&
                        std::abs(s64(output.width) * native.height -
                                 s64(output.height) * native.width) <= s64(native.height);
    const vk::Extent2D size = wanted ? output : vk::Extent2D{};
    Ryty::g_ui_layer_ready.store(wanted, std::memory_order_relaxed); // HDR-UI
    if (proxy.size != size || proxy.native != native) {
        Retire(state, proxy);
        proxy.size = size;
        proxy.native = native;
        proxy.dlss_failed = false;
    }
    return shown;
}

/// Viewport and scissor of a draw into the proxy.
inline void ScaleViewport(vk::Viewport& viewport, vk::Rect2D& scissor) {
    const float scale = Get().scale;
    if (scale == 0.0f) {
        return;
    }
    viewport.x *= scale;
    viewport.y *= scale;
    viewport.width *= scale;
    viewport.height *= scale;
    const s64 x0 = s64(std::floor(scissor.offset.x * scale));
    const s64 y0 = s64(std::floor(scissor.offset.y * scale));
    const s64 x1 = s64(std::ceil((s64(scissor.offset.x) + scissor.extent.width) * scale));
    const s64 y1 = s64(std::ceil((s64(scissor.offset.y) + scissor.extent.height) * scale));
    scissor.offset.x = s32(std::clamp<s64>(x0, 0, 32767));
    scissor.offset.y = s32(std::clamp<s64>(y0, 0, 32767));
    scissor.extent.width = u32(std::clamp<s64>(x1 - scissor.offset.x, 0, 32767));
    scissor.extent.height = u32(std::clamp<s64>(y1 - scissor.offset.y, 0, 32767));
}

} // namespace Vulkan::UiHires
