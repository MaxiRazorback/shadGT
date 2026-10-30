#pragma once
#include <cstdint>

namespace Native {
// Initial uploads of large CPU-owned textures need no persistent host-memory
// import. Preserve the ordered GPU-source path for every GPU-owned/aliased case.
constexpr bool PreferCpuTextureStaging(std::uint64_t size, bool nvidia,
                                      bool from_cpu, bool initial_upload,
                                      bool gpu_written, bool has_gpu_sources,
                                      bool overlaps_region, bool single_import,
                                      bool readable) {
    return nvidia && size > (4ull << 20) && size <= (64ull << 20) &&
           from_cpu && initial_upload && !gpu_written && !has_gpu_sources &&
           !overlaps_region && !single_import && readable;
}
}
