// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp11>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/post_order.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/renderer_vulkan/tess_lds_scratch.h"

TEST(TessLdsShader, RawMaskChangesRuntimeSpecialization) {
    Shader::RuntimeInfo full{}, partial{};
    full.Initialize(Shader::HwStage::Hull, Shader::SwStage::TessellationControl);
    partial = full;
    full.sw.tcs.lds_emulation = u8(Vulkan::TessLdsModeForMask(2, ~0u, ~0u));
    full.sw.tcs.num_patches = full.sw.tcs.lds_emulation ? 4 : 0;
    partial.sw.tcs.lds_emulation = u8(Vulkan::TessLdsModeForMask(2, ~0u, 0xffff0000u));
    partial.sw.tcs.num_patches = partial.sw.tcs.lds_emulation ? 4 : 0;
    EXPECT_NE(full, partial);
    EXPECT_EQ(partial.sw.tcs.lds_emulation, 0);
}

TEST(TessLdsShader, EmitsRealWriterAndReaderModule) {
    using namespace Shader;
    Info info{};
    info.hw_stage = HwStage::Hull;
    info.sw_stage = SwStage::TessellationControl;
    info.stores_tess_level_outer = true;
    info.stores_tess_level_inner = true;
    IR::Program program{info};
    Pools pools{};
    auto* block = pools.block_pool.Create(pools.inst_pool);
    program.blocks.push_back(block);
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Block;
    program.syntax_list.back().data.block = block;
    program.syntax_list.emplace_back();
    program.syntax_list.back().type = IR::AbstractSyntaxNode::Type::Return;
    program.post_order_blocks = IR::PostOrder(block);
    IR::IREmitter ir{*block};
    ir.Prologue();
    const auto value = ir.GetTessGenericAttribute(ir.Imm32(1u), ir.Imm32(0u), ir.Imm32(0u));
    ir.SetTcsGenericAttribute(value, ir.Imm32(0u), ir.Imm32(0u));
    ir.SetPatch(IR::Patch::TessellationLodLeft, ir.Imm32(1.0f));
    ir.SetPatch(IR::Patch::TessellationLodTop, ir.Imm32(1.0f));
    ir.SetPatch(IR::Patch::TessellationLodRight, ir.Imm32(1.0f));
    ir.SetPatch(IR::Patch::TessellationLodInteriorU, ir.Imm32(1.0f));
    ir.Epilogue();
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Hull, SwStage::TessellationControl);
    runtime.sw.tcs.lds_emulation = 2;
    runtime.sw.tcs.num_patches = 4;
    runtime.sw.tcs.num_input_control_points = 1;
    runtime.sw.tcs.num_threads = 1;
    runtime.sw.tcs.ls_stride = 16;
    runtime.sw.tcs.hs_output_cp_stride = 16;
    Backend::Bindings bindings{};
    const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    ASSERT_GT(code.size(), 5u);
    bool physical = false, write = false, branch = false, scratch_at_112 = false;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        ASSERT_GT(count, 0u);
        ASSERT_LE(offset + count, code.size());
        const auto op = spv::Op(code[offset] & 0xffff);
        physical |= op == spv::Op::OpConvertUToPtr;
        write |= op == spv::Op::OpStore;
        branch |= op == spv::Op::OpBranchConditional;
        if (op == spv::Op::OpMemberDecorate && count == 5 &&
            code[offset + 2] == PushData::LdsScratchIndex &&
            code[offset + 3] == u32(spv::Decoration::Offset) && code[offset + 4] == 112)
            scratch_at_112 = true;
        offset += count;
    }
    EXPECT_TRUE(physical && write && branch);
    EXPECT_TRUE(scratch_at_112);
    if (const char* dir = std::getenv("RYGT_TEST_SPV_DIR")) {
        std::ofstream file{std::filesystem::path(dir) / "tess_lds.spv", std::ios::binary};
        ASSERT_TRUE(file);
        file.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * sizeof(u32)));
        ASSERT_TRUE(file);
    }
}
