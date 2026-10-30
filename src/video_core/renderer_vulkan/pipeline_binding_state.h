// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace Vulkan {
// Graphics and compute bindings are independent command-buffer state. External
// helper commands conservatively invalidate both; a new command buffer does too.
template <typename Pipeline>
struct PipelineBindingState {
    std::array<Pipeline, 2> handles{};
    std::array<uint64_t, 2> emitted{}, reused{};

    void Invalidate() { handles = {}; }

    template <typename Emit>
    void Graphics(Pipeline pipeline, Emit&& emit) {
        Bind(0, pipeline, false, emit);
    }
    template <typename Emit>
    void Compute(Pipeline pipeline, bool force, Emit&& emit) {
        Bind(1, pipeline, force, emit);
    }
    void ResetCounts() { emitted = {}; reused = {}; }

private:
    template <typename Emit>
    void Bind(unsigned domain, Pipeline pipeline, bool force, Emit& emit) {
        if (force || handles[domain] != pipeline) {
            emit();
            handles[domain] = pipeline;
            ++emitted[domain];
        } else {
            ++reused[domain];
        }
    }
};
}
