// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
namespace Vulkan {
struct CompilerWorkerBudget {
    unsigned translation;
    unsigned pipeline;
    static constexpr CompilerWorkerBudget ForLogicalThreads(unsigned logical) {
        // Unknown topology and tiny CPUs use one compiler, without unsigned underflow.
        const unsigned total = logical > 2 ? logical - 2 : 1;
        const unsigned translation = total > 1 ? std::max(1u, total / 4) : 0;
        return {translation, total - translation};
    }
};
} // namespace Vulkan
