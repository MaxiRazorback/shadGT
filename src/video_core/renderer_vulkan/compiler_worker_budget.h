// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
namespace Vulkan {
struct CompilerWorkerBudget {
    // Concurrent tessellation compiles exhausted system commit in NVIDIA LLVM.
    // Share one admission limit between dispatch and direct driver calls.
    static constexpr unsigned HeavyPipelineLimit = 1;
    unsigned translation;
    unsigned pipeline;
    static constexpr CompilerWorkerBudget ForLogicalThreads(unsigned logical) {
        // Logical CPUs share execution and cache resources. Reserve half for game,
        // recording and the driver; do not start an almost machine-wide compile storm.
        // Bound large machines too: each driver compile can hold substantial memory.
        const unsigned total = logical > 2 ? std::min(8u, logical / 2) : 1;
        const unsigned translation = total > 1 ? std::max(1u, total / 4) : 0;
        return {translation, total - translation};
    }
};
} // namespace Vulkan
