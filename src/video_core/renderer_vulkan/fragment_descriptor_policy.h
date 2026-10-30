// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan {
// Keep stable fragment materials in immutable descriptor sets on NVIDIA. The
// diagnostic override only applies to this Native NVIDIA policy.
constexpr bool CacheFragmentDescriptors(std::uint32_t vendor, bool native,
                                         const char* override_value) {
    if (!native || vendor != 0x10de) return false;
    if (override_value && override_value[0] == '0' && override_value[1] == '\0') return false;
    return true;
}
}
