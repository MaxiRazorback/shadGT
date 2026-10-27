// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace Vulkan {
// Immutable descriptor sets, scoped to one command list and its descriptor pools.
// Hashes select a slot only: the complete descriptor values must also match.
template<class Set, std::size_t Capacity = 256> class DescriptorSetMemo {
    struct Entry { std::vector<std::uint64_t> key; Set set{}; };
    std::array<Entry, Capacity> entries{};
public:
    Set Find(std::uint64_t hash, std::span<const std::uint64_t> key) const {
        const auto& e = entries[hash % Capacity];
        return e.set && std::ranges::equal(e.key, key) ? e.set : Set{};
    }
    void Store(std::uint64_t hash, std::span<const std::uint64_t> key, Set set) {
        auto& e = entries[hash % Capacity];
        e.key.assign(key.begin(), key.end()); e.set = set;
    }
    void Clear() { for (auto& e : entries) { e.set = Set{}; e.key.clear(); } }
};
}
