#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <utility>

namespace Vulkan {
inline constexpr std::uint64_t UploadCacheBudget = 128ull << 20;
inline constexpr std::uint64_t UploadCacheMaxChunk = 64ull << 20;
constexpr std::uint64_t UploadChunkCapacity(std::uint64_t required,
                                          std::uint64_t standard_size) {
    const auto minimum = std::max(required, standard_size);
    return minimum <= UploadCacheMaxChunk ? std::bit_ceil(minimum) : minimum;
}
template<class Chunks, class Retired>
void CacheCompletedUploadChunks(Chunks& free, Retired& retired, std::uint64_t completed) {
    std::uint64_t bytes = 0;
    for (const auto& chunk : free) bytes += chunk.buffer->Size();
    while (!free.empty() && bytes > UploadCacheBudget) {
        bytes -= free.back().buffer->Size();
        free.pop_back();
    }
    while (!retired.empty() && retired.front().retire_tick <= completed) {
        const auto size = retired.front().buffer->Size();
        if (size <= UploadCacheMaxChunk) {
            // Large texture snapshots must not be excluded forever by the
            // already-full reserve of small constant-data buffers.
            while (!free.empty() && bytes + size > UploadCacheBudget) {
                bytes -= free.front().buffer->Size();
                free.erase(free.begin());
            }
            if (bytes + size <= UploadCacheBudget) {
                free.push_back(std::move(retired.front()));
                bytes += size;
            }
        }
        retired.pop_front();
    }
}
}
