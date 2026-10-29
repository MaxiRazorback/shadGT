// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// NAT-021: self test of the native image registry, outside the game. For a micro-tiled, a
// macro-tiled and a display-tiled layout with mips, a pattern is put in a render target, written
// out in the guest's tiled layout (RecordDownload), handed back as a texture in "guest memory"
// (RecordUploads untiles it) and compared texel by texel.

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "ryty_gpu/native/image_registry.h"
#include "ryty_gpu/native/rhi/rhi_vulkan.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/renderer_vulkan/upload_chunk_cache.h"
#include <deque>

namespace {

using namespace Native;

VideoCore::ImageInfo MakeInfo(AmdGpu::TileMode tile_mode, u32 width, u32 height, u32 mips,
                              VAddr address) {
    VideoCore::ImageInfo info{};
    info.props = {};
    info.props.is_tiled = tile_mode != AmdGpu::TileMode::DisplayLinearAligned;
    info.props.is_pow2 = std::has_single_bit(width) && std::has_single_bit(height);
    info.tile_mode = tile_mode;
    info.array_mode = AmdGpu::GetArrayMode(tile_mode);
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.num_samples = 1;
    info.num_bits = 32;
    info.type = AmdGpu::ImageType::Color2D;
    info.size = {width, height, 1};
    info.pitch = width;
    info.resources = {static_cast<u16>(mips), 1};
    info.guest_address = address;
    info.UpdateSize();
    return info;
}

u32 Texel(u32 x, u32 y, u32 mip) {
    return x | (y << 10) | (mip << 20) | 0xA0000000u;
}

bool RunCase(Rhi::Device& device, ImageRegistry& registry, Rhi::Fence& fence, u64& tick,
             AmdGpu::TileMode tile_mode, std::string& log,
             u32 kWidth = 256, u32 kHeight = 256) {
    constexpr u32 kMips = 4;
    const auto name = AmdGpu::NameOf(tile_mode);
    // 1. A render target filled with the pattern from a linear buffer.
    const VAddr target_address = 0x7000'0000'0000ULL + (u64(tile_mode) << 24);
    const auto target_info = MakeInfo(tile_mode, kWidth, kHeight, kMips,
                                     target_address + (kWidth > 256 ? 0x100000000ULL : 0));
    const u64 size = target_info.guest_size;
    ImageEntry* target = registry.Find(target_info, true, 1);
    auto pattern = device.CreateBuffer({.size = size,
                                        .usage = Rhi::BufferUsageTransferSrc,
                                        .memory = Rhi::MemoryKind::Upload});
    auto tiled = device.CreateBuffer({.size = size,
                                      .usage = Rhi::BufferUsageStorage | Rhi::BufferUsageTransferDst,
                                      .memory = Rhi::MemoryKind::Readback});
    if (!target || !pattern || !tiled) {
        log += fmt::format("{}: setup failed\n", name);
        return false;
    }
    std::memset(pattern->Mapped(), 0, size);
    for (u32 m = 0; m < kMips; ++m) {
        const auto& mip = target_info.mips_layout[m];
        const u32 width = std::max(kWidth >> m, 1u), height = std::max(kHeight >> m, 1u);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 value = Texel(x, y, m);
                std::memcpy(pattern->Mapped() + mip.offset + (u64(y) * mip.pitch + x) * 4, &value, 4);
            }
        }
    }
    Rhi::CommandList* list = device.AcquireCommandList();
    list->Begin();
    registry.RecordUploads(*list);
    list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeTransfer);
    list->CopyBufferToImage(pattern.get(), target->image.get(), MipCopies(target_info));
    list->Barrier(Rhi::ScopeTransfer, Rhi::ScopeTransfer);
    if (!registry.RecordDownload(*list, *target, *tiled)) {
        log += fmt::format("{}: download not recorded\n", name);
        return false;
    }
    list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeHost);
    list->End();
    Rhi::CommandList* lists[] = {list};
    device.GraphicsQueue().Submit(lists, {}, {&fence, ++tick});
    if (!fence.Wait(tick, 5'000'000'000ULL)) {
        log += fmt::format("{}: download did not finish\n", name);
        return false;
    }
    // 2. The tiled bytes as guest memory, found as a texture and untiled by the upload.
    std::vector<u8> guest(size + 4096);
    u8* guest_memory = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(guest.data()) + 4095) & ~uintptr_t{4095});
    std::memcpy(guest_memory, tiled->Mapped(), size);
    const auto texture_info =
        MakeInfo(tile_mode, kWidth, kHeight, kMips, reinterpret_cast<VAddr>(guest_memory));
    ImageEntry* texture = registry.Find(texture_info, false, 2);
    auto readback = device.CreateBuffer({.size = size,
                                         .usage = Rhi::BufferUsageTransferDst,
                                         .memory = Rhi::MemoryKind::Readback});
    if (!texture || !readback) {
        log += fmt::format("{}: texture setup failed\n", name);
        return false;
    }
    std::memset(readback->Mapped(), 0, size);
    list = device.AcquireCommandList();
    list->Begin();
    const u32 uploads = registry.RecordUploads(*list);
    const auto* saved_params = texture->download_params.get();
    if (!saved_params) {
        log += fmt::format("{}: missing cached upload parameters\n", name);
        return false;
    }
    // Several uploads in the same list must retain their immutable parameter buffer
    // and still produce the expected pixels after the final dispatch/copy barriers.
    for (u32 repeat = 0; repeat < 8; ++repeat) {
        list->Barrier(Rhi::ScopeTransfer | Rhi::ScopeCompute, Rhi::ScopeTransfer | Rhi::ScopeCompute);
        registry.RequestUpload(*texture);
        if (registry.RecordUploads(*list) != 1 || texture->download_params.get() != saved_params) {
            log += fmt::format("{}: repeated upload parameter reuse failed\n", name);
            return false;
        }
    }
    // Upload a different picture through the same scratch, then read both images.
    // A missing WAR barrier can overwrite the first picture's bytes before its copy.
    std::vector<u8> other_guest(size + 4096);
    auto* other_memory = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(other_guest.data()) + 4095) & ~uintptr_t{4095});
    for (u64 i = 0; i < size; ++i) other_memory[i] = guest_memory[i] ^ 0x5a;
    const auto other_info = MakeInfo(tile_mode, kWidth, kHeight, kMips, reinterpret_cast<VAddr>(other_memory));
    ImageEntry* other_texture = registry.Find(other_info, false, 2);
    auto other_readback = device.CreateBuffer({.size = size,
        .usage = Rhi::BufferUsageTransferDst, .memory = Rhi::MemoryKind::Readback});
    if (!other_texture || !other_readback || registry.RecordUploads(*list) != 1) {
        log += fmt::format("{}: second scratch image setup failed\n", name);
        return false;
    }
    list->CopyImageToBuffer(other_texture->image.get(), other_readback.get(), MipCopies(other_info));
    list->CopyImageToBuffer(texture->image.get(), readback.get(), MipCopies(texture_info));
    list->Barrier(Rhi::ScopeTransfer, Rhi::ScopeHost);
    list->End();
    lists[0] = list;
    device.GraphicsQueue().Submit(lists, {}, {&fence, ++tick});
    if (!fence.Wait(tick, 5'000'000'000ULL)) {
        log += fmt::format("{}: upload did not finish\n", name);
        return false;
    }
    u64 wrong = 0, total = 0;
    for (u32 m = 0; m < kMips; ++m) {
        const auto& mip = texture_info.mips_layout[m];
        const u32 width = std::max(kWidth >> m, 1u), height = std::max(kHeight >> m, 1u);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                u32 value;
                std::memcpy(&value, readback->Mapped() + mip.offset + (u64(y) * mip.pitch + x) * 4, 4);
                wrong += value != Texel(x, y, m);
                u32 other_value;
                std::memcpy(&other_value, other_readback->Mapped() + mip.offset + (u64(y) * mip.pitch + x) * 4, 4);
                wrong += other_value != (Texel(x, y, m) ^ 0x5a5a5a5au);
                total += 2;
            }
        }
    }
    // The tiled bytes must differ from the linear ones (otherwise nothing was tiled).
    const bool changed = std::memcmp(tiled->Mapped(), pattern->Mapped(), size) != 0;
    log += fmt::format("{}: {} uploaded, guest size {} bytes, tiled layout {}, {} of {} texels {}\n",
                       name, uploads, size, changed ? "differs from linear" : "SAME AS LINEAR",
                       total - wrong, total, wrong == 0 ? "correct" : "WRONG");
    return wrong == 0 && uploads == 1 && changed;
}

bool RunResolveFormatCase(ImageRegistry& registry, std::string& log) {
    auto scalar_info = MakeInfo(AmdGpu::TileMode::Thin1DThin, 480, 270, 1, 0x7100'0000'0000ULL);
    scalar_info.pixel_format = vk::Format::eR32Sfloat;
    auto* scalar = registry.Find(scalar_info, true, 1);
    auto color_info = scalar_info;
    color_info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    auto* aliased = registry.Find(color_info, true, 2);
    auto* color = registry.Find(color_info, true, 3, true, true);
    auto* repeated = registry.Find(color_info, true, 4, true, true);
    auto sampled_info = color_info;
    sampled_info.size.width = 430;
    sampled_info.UpdateSize();
    auto* sampled = registry.Find(sampled_info, false, 5);
    bool linked = false;
    if (sampled) {
        for (const auto& link : sampled->sources) {
            linked |= link.source == color;
        }
    }
    auto ms_scalar_info = scalar_info;
    ms_scalar_info.guest_address += 0x1000000;
    ms_scalar_info.num_samples = 2;
    ms_scalar_info.UpdateSize();
    auto* ms_scalar = registry.Find(ms_scalar_info, true, 6);
    auto ms_color_info = ms_scalar_info;
    ms_color_info.pixel_format = color_info.pixel_format;
    auto* ms_color = registry.Find(ms_color_info, true, 7);
    const bool ok =
        scalar && aliased == scalar && color && color != scalar && repeated == color &&
        color->image->Desc().format == Rhi::FormatFromVk(u32(color_info.pixel_format)) && linked &&
        ms_scalar && ms_color && ms_scalar != ms_color &&
        ms_color->image->Desc().format == color->image->Desc().format;
    log += fmt::format("resolve format isolation and cropped texture linking: {}\n",
                       ok ? "passed" : "FAILED");
    return ok;
}

bool RunUploadChunkCacheCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick, std::string& log) {
    struct Chunk { std::unique_ptr<Rhi::Buffer> buffer; u64 retire_tick; };
    std::vector<Chunk> free;
    std::deque<Chunk> retired;
    constexpr u64 bytes = 0x1556800;
    std::array<std::unique_ptr<Rhi::Buffer>, 2> reads;
    for (u32 i=0; i<2; ++i) {
        auto buffer = device.CreateBuffer({
            .size=Vulkan::UploadChunkCapacity(bytes,8ull<<20),
            .usage=Rhi::BufferUsageTransferSrc, .memory=Rhi::MemoryKind::Upload});
        reads[i] = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferDst,
                                       .memory=Rhi::MemoryKind::Readback});
        if (!buffer || !buffer->Mapped() || !reads[i] || !reads[i]->Mapped()) return false;
        std::memset(buffer->Mapped(), i ? 0xa7 : 0x35, bytes);
        auto* list = device.AcquireCommandList();
        list->Begin();
        list->CopyBuffer(buffer.get(),0,reads[i].get(),0,bytes);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);
        list->End();
        Rhi::CommandList* lists[]{list};
        device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
        retired.push_back({std::move(buffer),tick});
        // No CPU wait between submissions; neither upload may be recycled early.
        Vulkan::CacheCompletedUploadChunks(free,retired,0);
        if (!free.empty()) return false;
    }
    if (!fence.Wait(tick,5'000'000'000ULL)) return false;
    Vulkan::CacheCompletedUploadChunks(free,retired,fence.Completed());
    bool ok = retired.empty() && free.size()==2;
    for (u32 i=0;i<2;++i) {
        const auto* data=reads[i]->Mapped();
        const u8 expected=i ? 0xa7 : 0x35;
        for (u64 at=0;at<bytes;++at) if (data[at]!=expected) {ok=false;break;}
    }
    log += fmt::format("large upload chunk cache: {} bytes, two in-flight snapshots, {}\n",
                       2*bytes,ok ? "passed" : "FAILED");
    return ok;
}

bool RunRegionMergeOrderCase(Rhi::Device& device, Rhi::Fence& fence, u64& tick,
                            std::string& log) {
    constexpr u64 bytes = 4 * 1024 * 1024;
    auto base = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferSrc,
                                    .memory=Rhi::MemoryKind::Upload});
    auto retained = device.CreateBuffer({.size=32768,
        .usage=Rhi::BufferUsageTransferSrc | Rhi::BufferUsageTransferDst,
        .memory=Rhi::MemoryKind::DeviceLocal});
    auto patch = device.CreateBuffer({.size=32768, .usage=Rhi::BufferUsageTransferSrc,
                                     .memory=Rhi::MemoryKind::Upload});
    auto merged = device.CreateBuffer({.size=bytes,
        .usage=Rhi::BufferUsageTransferSrc | Rhi::BufferUsageTransferDst,
        .memory=Rhi::MemoryKind::DeviceLocal});
    auto read = device.CreateBuffer({.size=bytes, .usage=Rhi::BufferUsageTransferDst,
                                    .memory=Rhi::MemoryKind::Readback});
    if (!base || !retained || !patch || !merged || !read || !base->Mapped() ||
        !patch->Mapped() || !read->Mapped()) return false;
    struct Span { u64 dst, src, size; };
    // Descriptor tables, a scalar loop bound, counts, and a larger GPU-owned range.
    const std::array<Span,4> spans{{{4096,0,48}, {8192,4096,16},
                                   {2*1024*1024+64,8192,676},
                                   {3*1024*1024,16384,16384}}};
    std::vector<u8> expected(bytes);
    bool ok = true;
    for (u32 pass=0; pass<16 && ok; ++pass) {
        for (u64 i=0;i<bytes;++i) base->Mapped()[i]=u8(i*13+pass*7);
        for (u64 i=0;i<32768;++i) patch->Mapped()[i]=u8(i*29+pass*31+0x57);
        std::memcpy(expected.data(),base->Mapped(),bytes);
        for (const auto& span:spans)
            std::memcpy(expected.data()+span.dst,patch->Mapped()+span.src,span.size);
        auto* list=device.AcquireCommandList();
        list->Begin();
        list->CopyBuffer(patch.get(),0,retained.get(),0,32768);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        list->CopyBuffer(base.get(),0,merged.get(),0,bytes);
        // Same transfer-write dependency used by MakeRegion before its overlays.
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        for (const auto& span:spans)
            list->CopyBuffer(retained.get(),span.src,merged.get(),span.dst,span.size);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeTransfer);
        list->CopyBuffer(merged.get(),0,read.get(),0,bytes);
        list->Barrier(Rhi::ScopeTransfer,Rhi::ScopeHost);
        list->End();
        Rhi::CommandList* lists[]{list};
        device.GraphicsQueue().Submit(lists,{}, {&fence,++tick});
        ok=fence.Wait(tick,5'000'000'000ULL) &&
           std::memcmp(read->Mapped(),expected.data(),bytes)==0;
    }
    log+=fmt::format("region merge ordering: 16 mixed CPU/GPU patterns, {} bytes, {}\n",
                     bytes*16,ok ? "passed" : "FAILED");
    return ok;
}

bool RunImageSelfTest(std::string& log) {
    std::string error;
    auto device = Rhi::CreateStandaloneVulkanDevice(&error);
    if (!device) {
        log += "no device: " + error + "\n";
        return false;
    }
    auto fence = device->CreateFence(0);
    u64 tick = 0;
    bool ok = true;
    ok = RunUploadChunkCacheCase(*device,*fence,tick,log) && ok;
    ok = RunRegionMergeOrderCase(*device,*fence,tick,log) && ok;
    {
        ImageRegistry registry{*device};
        ok = RunResolveFormatCase(registry, log) && ok;
        for (const auto mode : {AmdGpu::TileMode::Thin1DThin, AmdGpu::TileMode::Thin2DThin,
                                AmdGpu::TileMode::Display2DThin}) {
            ok = RunCase(*device, registry, *fence, tick, mode, log) && ok;
        }
        const auto& reuse = registry.GetUploadStats();
        // Exercise the actual large CPU staging + untiling/copy path with two
        // distinct textures, repeated uploads and shared scratch barriers.
        ok = RunCase(*device, registry, *fence, tick, AmdGpu::TileMode::Thin2DThin,
                     log, 2048, 2048) && ok;
        const bool scratch_ok = reuse.scratch_created.load() < reuse.scratch_reused.load() &&
                                reuse.scratch_transient.load() == 0;
        ok = scratch_ok && ok;
        log += fmt::format("shared upload scratch: {} created, {} uses, {} transient; {}\n",
            reuse.scratch_created.load(), reuse.scratch_reused.load(),
            reuse.scratch_transient.load(), scratch_ok ? "passed" : "FAILED");
        const auto& stats = registry.GetStats();
        log += fmt::format("registry: {} lookups, {} textures, {} render targets, {} uploads\n",
                           stats.lookups.load(), stats.created_textures.load(),
                           stats.created_targets.load(), stats.uploads.load());
    }
    std::ignore = fence->Wait(tick, 5'000'000'000ULL);
    device->Collect();
    return ok;
}

} // namespace

extern "C" __declspec(dllexport) int RytyNativeImageSelfTest(char* report, int report_size) {
    std::string log;
    const bool ok = RunImageSelfTest(log);
    log += ok ? "NAT-021 image self test passed\n" : "NAT-021 image self test FAILED\n";
    if (report && report_size > 0) {
        const size_t n = std::min<size_t>(log.size(), static_cast<size_t>(report_size) - 1);
        std::memcpy(report, log.data(), n);
        report[n] = '\0';
    }
    return ok ? 0 : 1;
}
