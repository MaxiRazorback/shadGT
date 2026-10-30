#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>

namespace Native {
// One registry-wide device-local scratch buffer, not one retained per texture.
constexpr uint64_t UploadScratchCapacity(uint64_t required) {
    // 2048x2048 mip chains observed on track need about 22 MiB. Keep one
    // 32 MiB buffer for them instead of allocating it on every upload.
    constexpr uint64_t Limit = 32ull << 20;
    return required == 0 || required > Limit
               ? 0
               : std::bit_ceil(std::max<uint64_t>(required, 4096));
}
} // namespace Native
