// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Native {
// PERF-GTS-376: the descriptor sets kept across command lists, found by the two halves of a
// 128-bit hash of their key. One array, linear probing: a lookup reads one cache line in the
// common case (the node map it replaces read the bucket, the node and the key's words).
template <class Set>
class KeptSetTable {
    enum : std::uint8_t { Empty, Full, Removed };
    struct Slot {
        std::uint64_t key = 0, check = 0, layout = 0;
        Set set{};
        std::uint8_t state = Empty;
    };
    std::vector<Slot> slots;
    std::size_t count = 0, removed = 0;

    void Rebuild(std::size_t capacity) {
        std::vector<Slot> old(capacity);
        old.swap(slots);
        count = removed = 0;
        for (const auto& slot : old) {
            if (slot.state == Full) Insert(slot.key, slot.check, slot.layout, slot.set);
        }
    }

public:
    KeptSetTable() : slots(std::size_t{1} << 14) {}

    Set Find(std::uint64_t key, std::uint64_t check) const {
        const std::size_t mask = slots.size() - 1;
        for (std::size_t i = key & mask;; i = (i + 1) & mask) {
            const auto& slot = slots[i];
            if (slot.state == Empty) return {};
            if (slot.state == Full && slot.key == key && slot.check == check) return slot.set;
        }
    }

    void Insert(std::uint64_t key, std::uint64_t check, std::uint64_t layout, Set set) {
        if ((count + removed + 1) * 2 > slots.size()) {
            Rebuild((count + 1) * 4 > slots.size() ? slots.size() * 2 : slots.size());
        }
        const std::size_t mask = slots.size() - 1;
        std::size_t i = key & mask;
        while (slots[i].state == Full) i = (i + 1) & mask;
        if (slots[i].state == Removed) --removed;
        slots[i] = {key, check, layout, set, Full};
        ++count;
    }

    /// Takes the entry out; false when there is none.
    bool Erase(std::uint64_t key, std::uint64_t check, Set& set, std::uint64_t& layout) {
        const std::size_t mask = slots.size() - 1;
        for (std::size_t i = key & mask;; i = (i + 1) & mask) {
            auto& slot = slots[i];
            if (slot.state == Empty) return false;
            if (slot.state == Full && slot.key == key && slot.check == check) {
                set = slot.set;
                layout = slot.layout;
                slot.state = Removed;
                slot.set = Set{};
                --count;
                ++removed;
                return true;
            }
        }
    }

    std::size_t Size() const {
        return count;
    }

    void Clear() {
        slots.assign(std::size_t{1} << 14, Slot{});
        count = removed = 0;
    }
};
} // namespace Native
