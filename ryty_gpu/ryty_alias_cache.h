// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cassert>
#include <cstdint>
#include <list>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Ryty {
// The caller serializes lookup, insertion and use. Removed alias pointers are
// returned to the caller for unmapping after releasing that synchronization.
template<std::size_t Capacity = 4096>
class AliasEntries {
    static_assert(Capacity > 0);
    using Order = std::list<std::pair<std::uint64_t, void*>>;
    Order order;
    std::unordered_map<std::uint64_t, typename Order::iterator> entries;
    std::set<std::uint64_t> pages;
public:
    std::optional<void*> Find(std::uint64_t page) {
        const auto it = entries.find(page);
        if (it == entries.end()) return std::nullopt;
        order.splice(order.begin(), order, it->second);
        return it->second->second; // A cached failed mapping remains distinguishable.
    }
    void* Insert(std::uint64_t page, void* alias) {
        assert(!entries.contains(page));
        order.emplace_front(page, alias);
        try {
            entries.emplace(page, order.begin());
            try {
                pages.emplace(page);
            } catch (...) {
                entries.erase(page);
                throw;
            }
        } catch (...) {
            order.pop_front();
            throw;
        }
        void* retired = nullptr;
        if (entries.size() > Capacity) {
            retired = order.back().second;
            pages.erase(order.back().first);
            entries.erase(order.back().first);
            order.pop_back();
        }
        return retired;
    }
    void RemoveRange(std::uint64_t first, std::uint64_t end, std::vector<void*>& retired) {
        if (first >= end) return;
        auto begin = pages.lower_bound(first);
        const auto finish = pages.lower_bound(end);
        std::size_t count = 0;
        for (auto it = begin; it != finish; ++it) {
            if (entries.find(*it)->second->second) ++count;
        }
        if (count > retired.max_size() - retired.size()) throw std::length_error("alias retirement overflow");
        retired.reserve(retired.size() + count);
        while (begin != finish) {
            const auto entry = entries.find(*begin);
            if (entry->second->second) retired.push_back(entry->second->second);
            order.erase(entry->second);
            entries.erase(entry);
            begin = pages.erase(begin);
        }
    }
    std::size_t Size() const { return entries.size(); }
};
} // namespace Ryty
