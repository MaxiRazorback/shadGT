// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
namespace Vulkan {
// Caller holds the queue mutex. Already-running/completed jobs are absent.
// Rotate only the prefix so speculative jobs retain their relative order.
template<class Queue, class Job> bool PrioritizeTranslation(Queue& queue, const Job& job) {
    const auto found = std::find(queue.begin(), queue.end(), job);
    if (found == queue.end()) return false;
    std::rotate(queue.begin(), found, std::next(found));
    return true;
}
}
