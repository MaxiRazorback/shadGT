#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>
namespace Native {
struct RegionInitRange { std::uint64_t begin{}, end{}; };
inline std::vector<RegionInitRange> RegionInitGaps(
    std::uint64_t begin, std::uint64_t end, std::span<const RegionInitRange> covered) {
    std::vector<RegionInitRange> ordered, gaps;
    if (end <= begin) return gaps;
    ordered.reserve(covered.size());
    for (const auto range : covered) {
        const auto lo = std::max(begin,range.begin), hi = std::min(end,range.end);
        if (lo < hi) ordered.push_back({lo,hi});
    }
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b) { return a.begin < b.begin; });
    auto cursor = begin;
    for (const auto range : ordered) {
        if (cursor < range.begin) gaps.push_back({cursor,range.begin});
        cursor = std::max(cursor,range.end);
    }
    if (cursor < end) gaps.push_back({cursor,end});
    return gaps;
}
}
