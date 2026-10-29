// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
// Descriptor records/images are fixed after preparation. Ordered commit updates
// buffer-info slots in place, so refresh those slots before hashing or lookup.
struct PreparedDescriptorKey {
    struct BufferPatch { const vk::DescriptorBufferInfo* info; std::size_t word; };
    std::vector<std::uint64_t> words;
    std::vector<BufferPatch> buffers;
    bool cacheable = false;
    std::uint64_t fixed_hash = 0, hash = 0;
    static std::uint64_t Mix(std::uint64_t value) {
        value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27; value *= 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }
    static std::uint64_t WordHash(std::uint64_t value, std::size_t index) {
        return Mix(value + 0x9e3779b97f4a7c15ULL * (index + 1));
    }

    void Build(vk::DescriptorSetLayout layout, std::span<const vk::WriteDescriptorSet> writes) {
        words.clear(); buffers.clear(); cacheable = true;
        words.push_back(std::uint64_t(VkDescriptorSetLayout(layout)));
        for (const auto& write : writes) {
            words.insert(words.end(), {std::uint64_t(write.dstBinding), std::uint64_t(write.dstArrayElement),
                std::uint64_t(write.descriptorCount), std::uint64_t(write.descriptorType)});
            if (write.pNext || write.pTexelBufferView) { cacheable = false; break; }
            if (write.pBufferInfo) {
                for (std::uint32_t i = 0; i < write.descriptorCount; ++i) {
                    const auto& info = write.pBufferInfo[i];
                    buffers.push_back({&info, words.size()});
                    words.insert(words.end(), {std::uint64_t(VkBuffer(info.buffer)), info.offset, info.range});
                }
            } else if (write.pImageInfo) {
                for (std::uint32_t i = 0; i < write.descriptorCount; ++i) {
                    const auto& info = write.pImageInfo[i];
                    words.insert(words.end(), {std::uint64_t(VkSampler(info.sampler)),
                        std::uint64_t(VkImageView(info.imageView)), std::uint64_t(info.imageLayout)});
                }
            } else { cacheable = false; break; }
        }
        fixed_hash = Mix(words.size());
        if (cacheable) {
            std::size_t patch = 0;
            for (std::size_t word = 0; word < words.size(); ++word) {
                if (patch < buffers.size() && buffers[patch].word == word) {
                    word += 2; ++patch;
                } else {
                    fixed_hash ^= WordHash(words[word], word);
                }
            }
        }
        RefreshBuffers();
    }
    template<bool Hash = true> bool RefreshBuffers() {
        if constexpr (Hash) hash = fixed_hash;
        if (!cacheable) return false;
        bool changed = false;
        for (const auto& patch : buffers) {
            const std::uint64_t buffer = std::uint64_t(VkBuffer(patch.info->buffer));
            changed |= words[patch.word] != buffer ||
                       words[patch.word + 1] != patch.info->offset ||
                       words[patch.word + 2] != patch.info->range;
            words[patch.word] = buffer;
            words[patch.word + 1] = patch.info->offset;
            words[patch.word + 2] = patch.info->range;
            if constexpr (Hash) {
                hash ^= WordHash(words[patch.word], patch.word);
                hash ^= WordHash(words[patch.word + 1], patch.word + 1);
                hash ^= WordHash(words[patch.word + 2], patch.word + 2);
            }
        }
        return changed;
    }
};
} // namespace Vulkan
