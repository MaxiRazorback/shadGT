// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "ryty_gpu/ryty_gts_dlss.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace Vulkan {
namespace {
vk::Extent2D Size(const VideoCore::Image& image) {
    return {image.info.size.width, image.info.size.height};
}
constexpr vk::ImageSubresourceLayers ColorLayer{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
vk::ImageBlit Blit(vk::Extent2D from, vk::Extent2D to) {
    return {.srcSubresource = ColorLayer,
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                    vk::Offset3D{s32(from.width), s32(from.height), 1}},
            .dstSubresource = ColorLayer,
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                    vk::Offset3D{s32(to.width), s32(to.height), 1}}};
}
} // namespace

GtsDlssPass::GtsDlssPass(const Instance& gpuInstance, Scheduler& gpuScheduler, Runtime& gpuRuntime)
    : instance{gpuInstance}, scheduler{gpuScheduler}, runtime{gpuRuntime} {
    quality = RytyDlss::Quality();
    // FSR-200: without temporal DLSS, FSR 3.1 (or 4) reconstructs the scene from the same inputs;
    // XESS-200: or Intel XeSS, when the launcher picked it and libxess.dll is there.
    useXess = !RytyDlss::TemporalEnabled() && RytyXess::Requested() && RytyXess::Load();
    useFsr = !RytyDlss::TemporalEnabled() && !useXess && RytyFsr3::TemporalRequested() &&
             fsr.Available();
    if (useXess) {
        LOG_INFO(Render_Vulkan, "XESS-200: temporal XeSS armed; waiting for a matching scene");
    } else if (RytyFsr3::TemporalRequested()) {
        LOG_INFO(Render_Vulkan, "FSR-200: temporal FSR {}", useFsr ? "armed; waiting for a matching "
                 "scene" : "unavailable (amd_fidelityfx_vk.dll); FSR stays spatial");
    }
}

std::string GtsDlssPass::BackendName() const {
    return useXess ? RytyXess::VersionName() : useFsr ? RytyFsr3::ActiveVersionName() : "DLSS";
}

RygtDlssImage GtsDlssPass::Texture::Descriptor() const {
    return {static_cast<VkImage>(static_cast<vk::Image>(image)),
            static_cast<VkImageView>(view.get()), static_cast<VkFormat>(format),
            size.width, size.height};
}

void GtsDlssPass::create(Texture& t, vk::Format format, vk::Extent2D size,
                          vk::ImageAspectFlags aspects) {
    t.view.reset();
    t.presentView.reset();
    t.image.Destroy();
    t.image = VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    t.format = format;
    t.size = size;
    t.aspects = aspects;
    t.layout = vk::ImageLayout::eUndefined;
    auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                 vk::ImageUsageFlagBits::eTransferDst;
    if (aspects == vk::ImageAspectFlagBits::eColor) {
        usage |= vk::ImageUsageFlagBits::eColorAttachment;
        if (format == vk::Format::eR16G16B16A16Sfloat) usage |= vk::ImageUsageFlagBits::eStorage;
    }
    t.image.Create({.flags = vk::ImageCreateFlagBits::eMutableFormat,
                    .imageType = vk::ImageType::e2D,
                    .format = format,
                    .extent = {size.width, size.height, 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = vk::SampleCountFlagBits::e1,
                    .usage = usage});
    t.view = Check<"create temporal scene view">(instance.GetDevice().createImageViewUnique({
        .image = t.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {.aspectMask = aspects & ~vk::ImageAspectFlagBits::eStencil,
                             .levelCount = 1, .layerCount = 1}}));
}

void GtsDlssPass::transit(Texture& t, vk::ImageLayout layout) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .oldLayout = t.layout, .newLayout = layout, .image = t.image,
        .subresourceRange = {.aspectMask = t.aspects, .levelCount = 1, .layerCount = 1}};
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    t.layout = layout;
}

void GtsDlssPass::copyInput(VideoCore::Image& source, Texture& dest) {
    scheduler.EndRendering();
    runtime.Transit(&source, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    transit(dest, vk::ImageLayout::eTransferDstOptimal);
    const auto aspect = dest.aspects & ~vk::ImageAspectFlagBits::eStencil;
    const vk::ImageCopy region{.srcSubresource = {aspect, 0, 0, 1},
                               .dstSubresource = {aspect, 0, 0, 1},
                               .extent = {renderSize.width, renderSize.height, 1}};
    scheduler.CommandBuffer().copyImage(source.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
        dest.image, vk::ImageLayout::eTransferDstOptimal, region);
    transit(dest, vk::ImageLayout::eShaderReadOnlyOptimal);
}

bool GtsDlssPass::Capture(u64 frame, VideoCore::Image& color, VideoCore::Image& sourceDepth,
                         VideoCore::Image& sourceMotion, VideoCore::Image& resolved,
                         const std::array<float, 28>& constants) {
    if (!Enabled() || failed || !supportedDisplay || requestedSize.width == 0 ||
        scheduler.ThreadRing() || sceneFrame == frame) return false;
    const auto input = Size(color);
    // FSR-200: FSR upscales to the window, or (a window smaller than the scene) anti-aliases at the
    // scene's size and the presenter scales down.
    // FSR-209: or the launcher's preset sets it as a multiple of the scene (the presenter then
    // scales it to the window: supersampling when the window is smaller).
    const float ratio = RytyFsr3::UpscaleRatio();
    const auto scaled = vk::Extent2D{std::min(u32(std::lround(input.width * ratio)), 8192u),
                                     std::min(u32(std::lround(input.height * ratio)), 8192u)};
    const auto output = (useFsr || useXess)
                            ? ratio >= 1.0f ? scaled
                                            : vk::Extent2D{std::max(requestedSize.width, input.width),
                                                           std::max(requestedSize.height, input.height)}
                        : quality == RYGT_DLSS_QUALITY_DLAA ? input
                                                            : requestedSize;
    if (input != renderSize || output != outputSize || depth.format != sourceDepth.info.pixel_format) {
        RygtDlssOptimalSettings settings{};
        if (!useFsr && !useXess &&
            (!RytyDlss::GetOptimalSettings(output.width, output.height, quality, settings) ||
             input.width < settings.minimum_width || input.height < settings.minimum_height ||
             input.width > settings.maximum_width || input.height > settings.maximum_height)) {
            LOG_WARNING(Render_Vulkan, "Temporal DLSS: {}x{} is outside mode {} for {}x{}; keeping game TAA",
                        input.width, input.height, u32(quality), output.width, output.height);
            failed = true;
            return false;
        }
        scheduler.EndRendering();
        runtime.FlushBarriers();
        scheduler.Finish();
        renderSize = input;
        outputSize = output;
        create(this->color, color.info.pixel_format, input);
        create(depth, sourceDepth.info.pixel_format, input, sourceDepth.aspect_mask);
        create(motion, sourceMotion.info.pixel_format, input);
        if (useFsr || useXess) {
            create(fsrDepth, vk::Format::eR32Sfloat, input);
            depthCopy = std::make_unique<VideoCore::UniqueBuffer>(instance.GetDevice(),
                                                                  instance.GetAllocator());
            vk::BufferCreateInfo bufferInfo{
                .size = vk::DeviceSize{input.width} * input.height * 4,
                .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst};
            depthCopy->Create(bufferInfo, VideoCore::MemoryType::DeviceLocal, nullptr);
        }
        create(reconstruction, vk::Format::eR16G16B16A16Sfloat, output);
        create(native, vk::Format::eA2B10G10R10UnormPack32, output);
        reconstruction.presentView = Check<"create DLSS BGR copy view">(
            instance.GetDevice().createImageViewUnique({
                .image = reconstruction.image, .viewType = vk::ImageViewType::e2D,
                .format = reconstruction.format,
                .components = {.r = vk::ComponentSwizzle::eB, .g = vk::ComponentSwizzle::eG,
                               .b = vk::ComponentSwizzle::eR, .a = vk::ComponentSwizzle::eOne},
                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .levelCount = 1, .layerCount = 1}}));
        if (!colorCopy) colorCopy = std::make_unique<VideoCore::BlitHelper>(instance, scheduler);
        reset = true;
        if (useFsr || useXess) {
            LOG_INFO(Render_Vulkan, "FSR-200: actual scene {}x{} -> {}x{}", input.width,
                     input.height, output.width, output.height);
        } else {
            LOG_INFO(Render_Vulkan, "Temporal DLSS: actual scene {}x{} -> {}x{}, preset M, mode {}",
                     input.width, input.height, output.width, output.height, u32(quality));
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastScene.time_since_epoch().count()) {
        frameTimeMs = std::clamp(std::chrono::duration<float, std::milli>(now-lastScene).count(),
                                   0.01f, 1000.f);
        reset |= frameTimeMs > 250.f;
    }
    lastScene = now;
    reset |= sceneFrame == ~u64{} || frame != sceneFrame + 1;
    sceneFrame = frame;
    sceneUid = resolved.image_uid;
    outputFrame = ~u64{};
    displayUid = 0;
    jitterX = constants[20] * constants[14];
    jitterY = constants[21] * constants[15];
    copyInput(sourceDepth, depth);
    if (useFsr || useXess) copyDepthForFsr();
    copyInput(sourceMotion, motion);
    copyInput(color, this->color);
    return true;
}

void GtsDlssPass::copyDepthForFsr() {
    // FSR-205: depth plane of the scene depth -> buffer -> R32_SFLOAT image (32-bit float depth
    // copies out of D32_SFLOAT and D32_SFLOAT_S8_UINT alike).
    transit(depth, vk::ImageLayout::eTransferSrcOptimal);
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .imageExtent = {renderSize.width, renderSize.height, 1}};
    auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.copyImageToBuffer(depth.image, vk::ImageLayout::eTransferSrcOptimal,
                             depthCopy->buffer, region);
    const vk::BufferMemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .buffer = depthCopy->buffer,
        .size = VK_WHOLE_SIZE};
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                               .pBufferMemoryBarriers = &written});
    transit(fsrDepth, vk::ImageLayout::eTransferDstOptimal);
    const vk::BufferImageCopy toImage{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {renderSize.width, renderSize.height, 1}};
    cmdbuf.copyBufferToImage(depthCopy->buffer, fsrDepth.image,
                             vk::ImageLayout::eTransferDstOptimal, toImage);
    transit(fsrDepth, vk::ImageLayout::eShaderReadOnlyOptimal);
    transit(depth, vk::ImageLayout::eShaderReadOnlyOptimal);
}

bool GtsDlssPass::HasScene(u64 frame, u64 uid) const {
    return !failed && sceneFrame == frame && sceneUid == uid && outputFrame != frame;
}

void GtsDlssPass::PrepareScene(u64 frame, VideoCore::Image& resolved) {
    if (!HasScene(frame, resolved.image_uid)) return;
    const auto previous = resolved.backing->state;
    scheduler.EndRendering();
    transit(color, vk::ImageLayout::eTransferSrcOptimal);
    runtime.Transit(&resolved, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    const vk::ImageCopy region{.srcSubresource = ColorLayer, .dstSubresource = ColorLayer,
                              .extent = {renderSize.width, renderSize.height, 1}};
    scheduler.CommandBuffer().copyImage(color.image, vk::ImageLayout::eTransferSrcOptimal,
        resolved.GetImage(), vk::ImageLayout::eTransferDstOptimal, region);
    resolved.MarkGpuModified();
    runtime.Transit(&resolved, previous.layout, previous.pl_stage, previous.access_mask);
    runtime.FlushBarriers();
}

void GtsDlssPass::Resolve(u64 frame, VideoCore::Image& display) {
    if (failed || frame != sceneFrame || outputFrame == frame || Size(display) != renderSize ||
        display.info.pixel_format != vk::Format::eA2B10G10R10UnormPack32) return;
    scheduler.EndRendering();
    runtime.Transit(&display, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    transit(reconstruction, vk::ImageLayout::eGeneral);
    if (useFsr || useXess) {
        // FSR-200: FSR reads the picture in its own A2B10G10R10 layout, so its output needs no
        // red/blue swap (the DLSS path reads it swapped and swaps back when copying).
        VideoCore::ImageViewInfo colorInfo{};
        colorInfo.format = display.info.pixel_format;
        const RytyFsr3::TemporalFrame fsrFrame{
            .color = display.GetImage(),
            .color_format = display.info.pixel_format,
            .depth = fsrDepth.image,
            .depth_format = fsrDepth.format,
            .motion = motion.image,
            .motion_format = motion.format,
            .output = reconstruction.image,
            .output_format = reconstruction.format,
            .color_view = useXess ? *display.FindView(colorInfo).image_view : vk::ImageView{},
            .depth_view = fsrDepth.view.get(),
            .motion_view = motion.view.get(),
            .output_view = reconstruction.view.get(),
            .render_size = renderSize,
            .output_size = outputSize,
            .jitter_x = jitterX,
            .jitter_y = jitterY,
            .motion_scale_x = -float(renderSize.width) * .5f,
            .motion_scale_y = float(renderSize.height) * .5f,
            .frame_time_ms = frameTimeMs,
            .sharpness = EmulatorSettings.IsRcasEnabled()
                             ? 1.f - EmulatorSettings.GetRcasAttenuation() / 2000.f
                             : -1.f,
            .reset = reset,
        };
        const bool ok =
            useXess ? xess.Evaluate(instance.GetInstance(), instance.GetPhysicalDevice(),
                                    instance.GetDevice(), scheduler.DirectCommandBuffer(), fsrFrame)
                    : fsr.Evaluate(instance.GetPhysicalDevice(), instance.GetDevice(),
                                   scheduler.DirectCommandBuffer(), fsrFrame);
        scheduler.GetDynamicState().Invalidate();
        if (!ok) { failed = true; return; }
        ++evaluatedFrames;
        static u32 loggedFrames = 0;
        if (loggedFrames++ < 8) {
            LOG_INFO(Render_Vulkan, "{}: frame {} reset {} jitter ({}, {})",
                     useXess ? "XESS-200" : "FSR-200", frame, reset, jitterX, jitterY);
        }
        reset = false;
        // FSR-208: a transfer blit into the HUD image instead of the copy draw (FSR and XeSS need
        // no red/blue swap); the draw faulted the GPU on an RX 7600.
        transit(reconstruction, vk::ImageLayout::eTransferSrcOptimal);
        transit(native, vk::ImageLayout::eTransferDstOptimal);
        scheduler.DirectCommandBuffer().blitImage(
            reconstruction.image, vk::ImageLayout::eTransferSrcOptimal, native.image,
            vk::ImageLayout::eTransferDstOptimal, Blit(outputSize, outputSize), vk::Filter::eNearest);
        transit(reconstruction, vk::ImageLayout::eShaderReadOnlyOptimal);
        transit(native, vk::ImageLayout::eColorAttachmentOptimal);
        displayUid = display.image_uid;
        outputFrame = frame;
        nativeDirty = true;
        return;
    }
    VideoCore::ImageViewInfo viewInfo{};
    viewInfo.format = vk::Format::eA2R10G10B10UnormPack32;
    const auto view = *display.FindView(viewInfo).image_view;
    RygtDlssFrame desc{};
    desc.struct_size = sizeof(desc);
    desc.version = RYGT_DLSS_TEMPORAL_API_VERSION;
    desc.frame_id = frame;
    desc.color = {static_cast<VkImage>(display.GetImage()), static_cast<VkImageView>(view),
                  static_cast<VkFormat>(viewInfo.format),
                  renderSize.width, renderSize.height};
    desc.depth = depth.Descriptor();
    desc.motion = motion.Descriptor();
    desc.output = reconstruction.Descriptor();
    desc.quality = quality;
    desc.flags = reset ? RYGT_DLSS_FRAME_RESET : 0;
    desc.jitter_x = jitterX; desc.jitter_y = jitterY;
    desc.motion_scale_x = -float(renderSize.width) * .5f;
    desc.motion_scale_y = float(renderSize.height) * .5f;
    desc.pre_exposure = 1.f; desc.exposure_scale = 1.f;
    desc.frame_time_ms = frameTimeMs;
    const bool ok = RytyDlss::EvaluateTemporal(scheduler.DirectCommandBuffer(), desc);
    scheduler.GetDynamicState().Invalidate();
    if (!ok) { failed = true; return; }
    ++evaluatedFrames;
    static u32 loggedFrames = 0;
    if (loggedFrames++ < 8) {
        LOG_INFO(Render_Vulkan, "Temporal DLSS: frame {} reset {} jitter ({}, {}) MV scale ({}, {})",
                 frame, reset, jitterX, jitterY, desc.motion_scale_x, desc.motion_scale_y);
    }
    reset = false;
    transit(reconstruction, vk::ImageLayout::eShaderReadOnlyOptimal);
    transit(native, vk::ImageLayout::eColorAttachmentOptimal);
    colorCopy->CopyColorViews(outputSize, native.format, reconstruction.presentView.get(), native.view.get());
    displayUid = display.image_uid;
    outputFrame = frame;
    nativeDirty = true;
}

bool GtsDlssPass::isActive(u64 frame, u64 uid) const {
    return !failed && uid && uid == displayUid && frame == outputFrame;
}

bool GtsDlssPass::OverrideHud(u64 frame, u64 uid, RenderState& state) {
    if (!isActive(frame, uid) || state.num_color_attachments != 1 ||
        state.depth_stencil_attachment.has_depth || state.depth_stencil_attachment.has_stencil) return false;
    state.width = outputSize.width;
    state.height = outputSize.height;
    state.color_attachments[0].image_view = native.view.get();
    state.color_attachments[0].image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    state.color_attachments[0].is_clear = 0;
    nativeDirty = true;
    ++nativeHudDraws;
    return true;
}

void GtsDlssPass::SyncGuestRead(u64 frame, VideoCore::Image& display) {
    if (!isActive(frame, display.image_uid) || !nativeDirty) return;
    const auto previous = display.backing->state;
    copyBack(display);
    ++guestSyncs;
    runtime.Transit(&display, previous.layout, previous.pl_stage, previous.access_mask);
    runtime.FlushBarriers();
    transit(native, vk::ImageLayout::eColorAttachmentOptimal);
}

void GtsDlssPass::FallbackHud(u64 frame, VideoCore::Image& display, vk::ImageLayout restoreLayout) {
    if (!isActive(frame, display.image_uid)) return;
    copyBack(display);
    runtime.Transit(&display, restoreLayout, vk::PipelineStageFlagBits2::eAllCommands,
                    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite);
    runtime.FlushBarriers();
    outputFrame = ~u64{};
    ++fallbacks;
    static bool logged = false;
    if (!logged) {
        LOG_WARNING(Render_Vulkan, "Temporal DLSS: unsupported HUD attachment; using guest-sized composition for this frame");
        logged = true;
    }
}

void GtsDlssPass::ScaleHud(u64 frame, u64 uid, Viewports& viewports, Scissors& scissors) const {
    if (!isActive(frame, uid)) return;
    const float sx = float(outputSize.width) / renderSize.width;
    const float sy = float(outputSize.height) / renderSize.height;
    for (auto& v : viewports) { v.x *= sx; v.width *= sx; v.y *= sy; v.height *= sy; }
    for (auto& s : scissors) {
        const s32 left = std::clamp<s32>(std::floor(s.offset.x*sx), 0, outputSize.width);
        const s32 top = std::clamp<s32>(std::floor(s.offset.y*sy), 0, outputSize.height);
        const s32 right = std::clamp<s32>(std::ceil((float(s.offset.x)+s.extent.width)*sx), left, outputSize.width);
        const s32 bottom = std::clamp<s32>(std::ceil((float(s.offset.y)+s.extent.height)*sy), top, outputSize.height);
        s = vk::Rect2D{{left, top}, {u32(right-left), u32(bottom-top)}};
    }
}

void GtsDlssPass::copyBack(VideoCore::Image& display) {
    scheduler.EndRendering();
    transit(native, vk::ImageLayout::eTransferSrcOptimal);
    runtime.Transit(&display, vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
    runtime.FlushBarriers();
    scheduler.DirectCommandBuffer().blitImage(native.image, vk::ImageLayout::eTransferSrcOptimal,
        display.GetImage(), vk::ImageLayout::eTransferDstOptimal, Blit(outputSize, renderSize), vk::Filter::eLinear);
    display.MarkGpuModified();
    nativeDirty = false;
}

vk::ImageView GtsDlssPass::PreparePresent(u64 frame, VideoCore::Image& display, vk::Format viewFormat) {
    if (!isActive(frame, display.image_uid)) return {};
    if (nativeDirty) copyBack(display);
    transit(native, vk::ImageLayout::eShaderReadOnlyOptimal);
    ++nativePresentations;
    if (nativePresentations == 1 || nativePresentations % 600 == 0) {
        LOG_INFO(Render_Vulkan,
                 "Temporal DLSS: presenting {}x{}; evaluated {}, native presentations {}, native HUD draws {}, guest reads {}, fallbacks {}",
                 outputSize.width, outputSize.height, evaluatedFrames, nativePresentations,
                 nativeHudDraws, guestSyncs, fallbacks);
    }
    if (!native.presentView) {
        native.presentView = Check<"create temporal presentation view">(
            instance.GetDevice().createImageViewUnique({
                .image = native.image, .viewType = vk::ImageViewType::e2D,
                .format = viewFormat, .components = {.a = vk::ComponentSwizzle::eOne},
                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                     .levelCount = 1, .layerCount = 1}}));
    }
    return native.presentView.get();
}
} // namespace Vulkan
