// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace Vulkan {
// Page state and protection changes remain serialized for each address range.
// A large texture cannot monopolize tracking for every unrelated game allocation.
class PageWatchTracker {
public:
    static constexpr std::uint64_t PageSize = 4096;
    static constexpr std::uint64_t RangeSize = 1ull << 20;
    static constexpr std::size_t ShardCount = 64;
    template<class Apply> std::uint64_t Track(std::uint64_t begin, std::uint64_t end, Apply&& apply) {
        return Change<true>(begin,end,apply);
    }
    template<class Apply> std::uint64_t Untrack(std::uint64_t begin, std::uint64_t end, Apply&& apply) {
        return Change<false>(begin,end,apply);
    }
private:
    // One allocation and four words per populated MiB, rather than 256 hash
    // nodes for its 4 KiB pages. Empty blocks are removed on untracking.
    using Words = std::array<std::uint64_t, RangeSize / PageSize / 64>;
    struct Shard { std::mutex mutex; std::unordered_map<std::uint64_t, Words> ranges; };
    std::array<Shard,ShardCount> shards;
    template<bool Track, class Apply>
    std::uint64_t Change(std::uint64_t begin, std::uint64_t end, Apply& apply) {
        std::uint64_t changed = 0;
        while(begin < end) {
            const auto limit = begin + std::min(end-begin, RangeSize-(begin%RangeSize));
            auto& shard = shards[(begin/RangeSize)%ShardCount];
            {
                std::scoped_lock lock{shard.mutex};
                const auto block = begin / RangeSize;
                auto found = shard.ranges.find(block);
                if (found == shard.ranges.end()) {
                    if constexpr (Track) {
                        found = shard.ranges.emplace(block, Words{}).first;
                    } else {
                        begin = limit;
                        continue;
                    }
                }
                auto& words = found->second;
                std::uint64_t run = 0;
                bool have_run = false;
                for(auto page=begin;page<limit;page+=PageSize) {
                    const auto index = (page % RangeSize) / PageSize;
                    auto& word = words[index / 64];
                    const auto bit = std::uint64_t{1} << (index % 64);
                    const bool present = (word & bit) != 0;
                    const bool update = Track ? !present : present;
                    if constexpr (Track) word |= bit;
                    else word &= ~bit;
                    if(update) {
                        ++changed;
                        if(!have_run) {run=page;have_run=true;}
                    } else if(have_run) {
                        apply(run,page-run);have_run=false;
                    }
                }
                if(have_run) apply(run,limit-run);
                if constexpr (!Track) {
                    if (std::all_of(words.begin(), words.end(), [](auto word) { return word == 0; })) {
                        shard.ranges.erase(found);
                    }
                }
            }
            begin=limit;
        }
        return changed;
    }
};
}
