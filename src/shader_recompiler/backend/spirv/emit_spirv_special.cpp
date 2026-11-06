// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/debug_print.h"
#include "shader_recompiler/ir/microinstruction.h"

namespace Shader::Backend::SPIRV {

void EmitPrologue(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Fragment) {
        ctx.DefineAmdPerVertexAttribs();
        if (ctx.runtime_info.hw.fs.quad_sample_mask) {
            const Id coordinates =
                ctx.OpConvertFToU(ctx.U32[4], ctx.OpLoad(ctx.F32[4], ctx.frag_coord));
            const Id x = ctx.OpBitwiseAnd(
                ctx.U32[1], ctx.OpCompositeExtract(ctx.U32[1], coordinates, 0u), ctx.u32_one_value);
            const Id y = ctx.OpBitwiseAnd(
                ctx.U32[1], ctx.OpCompositeExtract(ctx.U32[1], coordinates, 1u), ctx.u32_one_value);
            const Id row_ptr = ctx.OpAccessChain(
                ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[1]), ctx.push_data_block,
                ctx.ConstU32(PushData::QuadSampleMaskIndex), y);
            const Id row = ctx.OpLoad(ctx.U32[1], row_ptr);
            const Id shift = ctx.OpShiftLeftLogical(ctx.U32[1], x, ctx.ConstU32(4u));
            ctx.quad_sample_mask = ctx.OpBitwiseAnd(
                ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], row, shift), ctx.ConstU32(0xffffu));
            ctx.OpStore(ctx.OpAccessChain(ctx.output_u32, ctx.sample_mask, ctx.u32_zero_value),
                        ctx.quad_sample_mask);
        }
    }
    if (ctx.info.loads.Get(IR::Attribute::WorkgroupIndex)) {
        ctx.DefineWorkgroupIndex();
    }
    ctx.DefineBufferProperties();
}

void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void ConvertPositionToClipSpace(EmitContext& ctx) {
    ASSERT_MSG(!ctx.info.stores.GetAny(IR::Attribute::ViewportIndex),
               "Multi-viewport with shader clip space conversion not yet implemented.");

    const Id type{ctx.F32[1]};
    Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id xoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::XOffsetIndex))};
    const Id xoffset{ctx.OpLoad(type, xoffset_ptr)};
    const Id yoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::YOffsetIndex))};
    const Id yoffset{ctx.OpLoad(type, yoffset_ptr)};
    const Id xscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::XScaleIndex))};
    const Id xscale{ctx.OpLoad(type, xscale_ptr)};
    const Id yscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::YScaleIndex))};
    const Id yscale{ctx.OpLoad(type, yscale_ptr)};
    const Id vport_w =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_width / 2, 8_KB)));
    const Id wnd_x = ctx.OpFAdd(type, ctx.OpFMul(type, x, xscale), xoffset);
    const Id ndc_x = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_x, vport_w), ctx.Constant(type, 1.f));
    const Id vport_h =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_height / 2, 8_KB)));
    const Id wnd_y = ctx.OpFAdd(type, ctx.OpFMul(type, y, yscale), yoffset);
    const Id ndc_y = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_y, vport_h), ctx.Constant(type, 1.f));
    const Id vector{ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>({ndc_x, ndc_y, z, w}))};
    ctx.OpStore(ctx.output_position, vector);
}

// ryGT HDR-UI: the first colour output is SDR (sRGB encoded, BT.709) blended into an HDR10 display
// buffer; it is encoded as BT.2020 PQ with the UI white at kUiWhiteNits (GT Sport's own
// actualSDRnit) before the blend.
[[maybe_unused]] static void EncodeHdrUi(EmitContext& ctx) {
    constexpr float kUiWhiteNits = 250.0f;
    const auto& info = ctx.frag_outputs[0];
    if (!Sirit::ValidId(info.id) || info.is_integer || info.num_components < 3 ||
        info.component_type.value != ctx.F32[1].value) {
        return;
    }
    const Id f = ctx.F32[1];
    const auto ptr = [&](u32 element) {
        return ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(element));
    };
    const Id zero = ctx.ConstF32(0.0f);
    const Id one = ctx.ConstF32(1.0f);
    const auto decode = [&](Id c) {
        c = ctx.OpFMin(f, ctx.OpFMax(f, c, zero), one);
        const Id low = ctx.OpFDiv(f, c, ctx.ConstF32(12.92f));
        const Id high = ctx.OpPow(
            f, ctx.OpFDiv(f, ctx.OpFAdd(f, c, ctx.ConstF32(0.055f)), ctx.ConstF32(1.055f)),
            ctx.ConstF32(2.4f));
        return ctx.OpSelect(f, ctx.OpFOrdLessThanEqual(ctx.U1[1], c, ctx.ConstF32(0.04045f)),
                            low, high);
    };
    const Id r = decode(ctx.OpLoad(f, ptr(0)));
    const Id g = decode(ctx.OpLoad(f, ptr(1)));
    const Id b = decode(ctx.OpLoad(f, ptr(2)));
    const auto row = [&](float x, float y, float z) {
        const Id sum = ctx.OpFAdd(f, ctx.OpFAdd(f, ctx.OpFMul(f, r, ctx.ConstF32(x)),
                                                ctx.OpFMul(f, g, ctx.ConstF32(y))),
                                  ctx.OpFMul(f, b, ctx.ConstF32(z)));
        return ctx.OpFMul(f, sum, ctx.ConstF32(kUiWhiteNits / 10000.0f));
    };
    const auto pq = [&](Id y) {
        const Id p = ctx.OpPow(f, ctx.OpFMax(f, y, zero), ctx.ConstF32(0.1593017578125f));
        const Id num = ctx.OpFAdd(f, ctx.ConstF32(0.8359375f), ctx.OpFMul(f, p, ctx.ConstF32(18.8515625f)));
        const Id den = ctx.OpFAdd(f, one, ctx.OpFMul(f, p, ctx.ConstF32(18.6875f)));
        return ctx.OpPow(f, ctx.OpFDiv(f, num, den), ctx.ConstF32(78.84375f));
    };
    const Id r2 = pq(row(0.6274f, 0.3293f, 0.0433f));
    const Id g2 = pq(row(0.0691f, 0.9195f, 0.0114f));
    const Id b2 = pq(row(0.0164f, 0.0880f, 0.8956f));
    ctx.OpStore(ptr(0), r2);
    ctx.OpStore(ptr(1), g2);
    ctx.OpStore(ptr(2), b2);
}

void EmitEpilogue(EmitContext& ctx) {
    if (ctx.hw_stage == HwStage::Fragment && ctx.runtime_info.hw.fs.color_buffers[0].hdr_ui_alpha1) {
        const auto& info = ctx.frag_outputs[0];
        if (Sirit::ValidId(info.id) && !info.is_integer && info.num_components == 4 &&
            info.component_type.value == ctx.F32[1].value) {
            ctx.OpStore(ctx.OpAccessChain(info.pointer_type, info.id, ctx.ConstU32(3U)),
                        ctx.ConstF32(1.0f));
        }
    }
    if (ctx.hw_stage == HwStage::Vertex &&
        ctx.runtime_info.hw.vs.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    if (ctx.hw_stage == HwStage::Vertex && ctx.runtime_info.hw.vs.clip_disable) {
        ConvertPositionToClipSpace(ctx);
    }
}

// FIX-GTS-011: shaders FIX-223 leaves as they were. GT Sport's photo mode filters (Cross
// Processing and others) need their discarded pixels to stay helpers: ended at once, the filter
// washed the picture out to purple and white. A change to this list needs a new
// ShaderBinaryVersionDiscardKill (vk_pipeline_serialization.cpp), or stored shaders keep the old.
static bool DiscardKills(EmitContext& ctx) {
    if (!ctx.profile.discard_kills) {
        return false;
    }
    static constexpr std::array<u64, 2> keep{
        0x0e8aec17, // full-screen pass reading the depth buffer (photo mode filters)
        0x226b9c89, // text drawn into a 512x512 picture
    };
    const u64 hash = ctx.info.pgm_hash & 0xffffffff;
    return std::ranges::find(keep, hash) == keep.end();
}

// FIX-223: on AMD, pixels demoted to helpers left junk colours on discard edges
// (magenta HUD and cockpit lights, green and purple dots on thin geometry).
void EmitDiscard(EmitContext& ctx) {
    if (DiscardKills(ctx)) {
        const Id kill_label{ctx.OpLabel()};
        const Id merge_label{ctx.OpLabel()};
        ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(ctx.true_value, kill_label, merge_label);
        ctx.AddLabel(kill_label);
        ctx.OpKill();
        ctx.AddLabel(merge_label);
        return;
    }
    ctx.OpDemoteToHelperInvocationEXT();
}

void EmitDiscardCond(EmitContext& ctx, Id condition) {
    const Id kill_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, kill_label, merge_label);
    ctx.AddLabel(kill_label);
    if (DiscardKills(ctx)) {
        ctx.OpKill();
    } else {
        ctx.OpDemoteToHelperInvocationEXT();
        ctx.OpBranch(merge_label);
    }
    ctx.AddLabel(merge_label);
}

void EmitEmitVertex(EmitContext& ctx) {
    ctx.OpEmitVertex();
}

void EmitEmitPrimitive(EmitContext& ctx) {
    ctx.OpEndPrimitive();
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitDebugPrint(EmitContext& ctx, IR::Inst* inst, Id fmt, Id arg0, Id arg1, Id arg2, Id arg3) {
    IR::DebugPrintFlags flags = inst->Flags<IR::DebugPrintFlags>();
    std::array<Id, IR::DEBUGPRINT_NUM_FORMAT_ARGS> fmt_args = {arg0, arg1, arg2, arg3};
    auto fmt_args_span = std::span<Id>(fmt_args.begin(), fmt_args.begin() + flags.num_args);
    ctx.OpDebugPrintf(fmt, fmt_args_span);
}

Id EmitMemtime(EmitContext& ctx) {
    if (ctx.profile.supports_shader_subgroup_clock) {
        return ctx.OpReadClockKHR(ctx.U64, ctx.ConstU32(std::to_underlying(spv::Scope::Subgroup)));
    } else {
        return ctx.Constant(ctx.U64, 1U);
    }
}

} // namespace Shader::Backend::SPIRV
