// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <algorithm>
#include <array>
#include <fstream>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "video_core/shader_store_policy.h"

TEST(ShaderStorePolicy, PreservesOriginalAndReusesCompatibleSibling) {
    const auto parent=std::filesystem::temp_directory_path() /
        ("ryty-store-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto original=parent/"shader-store";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    { std::ofstream(original/"store-version.txt") << "old-policy\n"; }
    { std::ofstream(original/"sentinel.bin") << "keep"; }
    { std::ofstream(parent/"shaders-pack.txt") << "pack"; }
    { std::ofstream(parent/"game.else-scope-fix") << "old-marks"; }
    const auto same=VideoCore::OpenCompatibleShaderStore(original,"old-policy");
    ASSERT_TRUE(same); EXPECT_EQ(*same,original);
    const auto next=VideoCore::OpenCompatibleShaderStore(original,"new-policy");
    ASSERT_TRUE(next); EXPECT_NE(*next,original);
    EXPECT_EQ(VideoCore::ShaderStoreMarksPath(parent,"game",original,false),parent/"game.else-scope-fix");
    EXPECT_EQ(VideoCore::ShaderStoreMarksPath(parent,"game",*next,true),*next/"else-scope-fix");
    { std::ofstream(VideoCore::ShaderStoreMarksPath(parent,"game",*next,true)) << "new-marks"; }
    std::string old_marks;
    { std::ifstream input(parent/"game.else-scope-fix"); std::getline(input,old_marks); }
    EXPECT_EQ(old_marks,"old-marks");
    { std::ofstream(*next/"new.bin") << "new"; }
    const auto again=VideoCore::OpenCompatibleShaderStore(original,"new-policy");
    ASSERT_TRUE(again); EXPECT_EQ(*again,*next);
    EXPECT_TRUE(std::filesystem::exists(original/"sentinel.bin"));
    EXPECT_TRUE(std::filesystem::exists(parent/"shaders-pack.txt"));
    const auto third=VideoCore::OpenCompatibleShaderStore(original,"third-policy");
    ASSERT_TRUE(third); EXPECT_NE(*third,*next); EXPECT_NE(*third,original);
    EXPECT_TRUE(std::filesystem::exists(*next/"new.bin"));
    std::filesystem::remove(original/"sentinel.bin");
    std::filesystem::remove(original/"store-version.txt");
    std::filesystem::remove(*next/"new.bin");
    std::filesystem::remove(*next/"else-scope-fix");
    std::filesystem::remove(*next/"store-version.txt");
    std::filesystem::remove(*third/"store-version.txt");
    std::filesystem::remove(parent/"shaders-pack.txt");
    std::filesystem::remove(parent/"game.else-scope-fix");
    std::filesystem::remove(original); std::filesystem::remove(*next);
    std::filesystem::remove(*third); std::filesystem::remove(parent);
}

TEST(ShaderStorePolicy, NonDirectoryAndWriteFailureLeaveExistingFileUntouched) {
    const auto parent=std::filesystem::temp_directory_path() /
        ("ryty-store-invalid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto file=parent/"not-a-directory";
    { std::ofstream(file) << "keep"; }
    EXPECT_FALSE(VideoCore::OpenCompatibleShaderStore(file/"child","policy"));
#ifdef _WIN32
    EXPECT_FALSE(VideoCore::OpenCompatibleShaderStore(parent/"invalid?name","policy"));
#endif
    std::string content; { std::ifstream input(file); std::getline(input,content); }
    EXPECT_EQ(content,"keep");
    std::filesystem::remove(file); std::filesystem::remove(parent);
}

TEST(ShaderStorePolicy, ConcurrentSamePolicyCreatorsPreserveOriginalAndReuseACompatibleStore) {
    const auto parent=std::filesystem::temp_directory_path() /
        ("ryty-store-race-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directory(parent));
    const auto original=parent/"store";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    { std::ofstream(original/"store-version.txt") << "old\n"; }
    { std::ofstream(original/"sentinel") << "keep"; }
    std::array<std::optional<std::filesystem::path>,8> choices;
    std::vector<std::jthread> threads;
    for (size_t i=0;i<choices.size();++i)
        threads.emplace_back([&,i] { choices[i]=VideoCore::OpenCompatibleShaderStore(original,"new"); });
    threads.clear();
    for (const auto& choice:choices) ASSERT_TRUE(choice);
    const auto again=VideoCore::OpenCompatibleShaderStore(original,"new");
    ASSERT_TRUE(again);
    EXPECT_TRUE(std::ranges::any_of(choices,[&](const auto& choice) { return *choice==*again; }));
    std::string content; { std::ifstream input(original/"sentinel"); std::getline(input,content); }
    EXPECT_EQ(content,"keep");
    for (const auto& choice:choices) {
        std::filesystem::remove(*choice/"store-version.txt");
        std::filesystem::remove(*choice);
    }
    std::filesystem::remove(original/"sentinel");
    std::filesystem::remove(original/"store-version.txt");
    std::filesystem::remove(original); std::filesystem::remove(parent);
}
