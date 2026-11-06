// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/stable_region_bindings.h"

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
