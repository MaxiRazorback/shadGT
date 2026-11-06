// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include <map>
#include "video_core/renderer_vulkan/stable_region_bindings.h"
#include "ryty_gpu/native/region_init_gaps.h"

TEST(StableRegionBindings, RebuildsAfterARegionIsReplaced) {
    unsigned generation = 0, binds = 0, resets = 0;
    const bool ok = Vulkan::BindRegionsStable([&] { return generation; }, [&] {
        if (++binds == 1) ++generation;
        return true;
    }, [&] { ++resets; });
    EXPECT_TRUE(ok);
    EXPECT_EQ(binds, 2u);
    EXPECT_EQ(resets, 1u);
}

TEST(StableRegionBindings, RefusedReadsPreserveRegionsAndWritesInvalidateThem) {
    using Native::RegionMergeDisposition;
    using Native::RegionMergePolicy;
    EXPECT_EQ(RegionMergePolicy(true, false, 4, 16), RegionMergeDisposition::Replace);
    EXPECT_EQ(RegionMergePolicy(false, false, 4, 16), RegionMergeDisposition::Preserve);
    EXPECT_EQ(RegionMergePolicy(false, true, 4, 16), RegionMergeDisposition::Invalidate);
    EXPECT_EQ(RegionMergePolicy(false, true, 17, 16), RegionMergeDisposition::Preserve);
}

TEST(StableRegionBindings, RegionReplacementIsTransactionalOnAdmissionAndAllocationFailure) {
    struct Region { std::uint64_t begin, end; int contents; };
    std::map<std::uint64_t, Region> regions{{0, {0, 8, 7}}, {8, {8, 16, 9}}};
    std::vector<Region> retired;
    auto remove = [&](Region&& r) { retired.push_back(r); };
    auto try_replace = [&](bool valid, bool write, bool allocated) {
        std::uint64_t begin = 4, end = 12;
        return Native::ReplaceOverlappingRegions(regions, begin, end, write, 8, 16,
            [&](auto, auto) { return valid; }, [&](auto, auto) { return allocated; }, remove);
    };
    EXPECT_EQ(try_replace(false, false, true), Native::RegionMergeDisposition::Preserve);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(try_replace(true, true, false), Native::RegionMergeDisposition::Preserve);
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions.at(0).contents, 7);
    EXPECT_EQ(regions.at(8).contents, 9);
    EXPECT_TRUE(retired.empty());
    EXPECT_EQ(try_replace(false, true, true), Native::RegionMergeDisposition::Invalidate);
    EXPECT_TRUE(regions.empty());
    ASSERT_EQ(retired.size(), 2u);
    EXPECT_EQ(retired[0].contents, 7);
    EXPECT_EQ(retired[1].contents, 9);
}

TEST(StableRegionBindings, RejectsUnstableOrFailedBindings) {
    unsigned generation = 0, resets = 0;
    EXPECT_FALSE(Vulkan::BindRegionsStable([&] { return generation; }, [&] {
        ++generation;
        return true;
    }, [&] { ++resets; }));
    EXPECT_EQ(resets, 4u);
    EXPECT_FALSE(Vulkan::BindRegionsStable([&] { return generation; }, [] { return false; },
                                          [&] { ++resets; }));
    EXPECT_EQ(resets, 4u);
}
