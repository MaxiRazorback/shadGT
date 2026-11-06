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
    bool physical = false, write = false, branch = false;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        ASSERT_GT(count, 0u);
        ASSERT_LE(offset + count, code.size());
        const auto op = spv::Op(code[offset] & 0xffff);
        physical |= op == spv::Op::OpConvertUToPtr;
        write |= op == spv::Op::OpStore;
        branch |= op == spv::Op::OpBranchConditional;
        offset += count;
    }
    EXPECT_TRUE(physical && write && branch);
    if (const char* dir = std::getenv("RYGT_TEST_SPV_DIR")) {
        std::ofstream file{std::filesystem::path(dir) / "tess_lds.spv", std::ios::binary};
        ASSERT_TRUE(file);
        file.write(reinterpret_cast<const char*>(code.data()), std::streamsize(code.size() * sizeof(u32)));
        ASSERT_TRUE(file);
    }
}
