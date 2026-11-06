// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/tess_lds_scratch.h"

TEST(TessLdsScratch, BoundedAndOverflowSafe) {
    using Vulkan::TessLdsScratchSize;
    ASSERT_TRUE(TessLdsScratchSize(16, 2, 4, 64));
    EXPECT_EQ(*TessLdsScratchSize(16, 2, 4, 64), 2048);
    EXPECT_EQ(*TessLdsScratchSize(0, 0, 0, 0), 64);
    EXPECT_FALSE(TessLdsScratchSize(~0ull, ~0ull, 1, 64));
    EXPECT_FALSE(TessLdsScratchSize(1, 1, ~0ull, 64));
    EXPECT_FALSE(TessLdsScratchSize(1, 1, 1, ~0ull));
}

TEST(TessLdsScratch, RejectsUnsafeDraws) {
    using Vulkan::TessLdsSafeShape;
    EXPECT_TRUE(TessLdsSafeShape(1, false, true, false));
    EXPECT_FALSE(TessLdsSafeShape(0, false, true, false));
    EXPECT_FALSE(TessLdsSafeShape(2, false, true, false));
    EXPECT_FALSE(TessLdsSafeShape(1, true, true, false));
    EXPECT_FALSE(TessLdsSafeShape(1, false, false, false));
    EXPECT_FALSE(TessLdsSafeShape(1, false, true, true));
    EXPECT_FALSE(TessLdsSafeShape(1, false, true, false, false));
}
