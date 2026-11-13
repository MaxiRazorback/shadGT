#pragma once
#include "ryty_gpu/native/parallel_pass_coordinator.h"
namespace Native::Parallel {
struct RotationDiag {u64 attempts{},success{},invalid{},preflight{},acquire{},command{},prefix{},exception{};};
// Rotate without NativeRenderer::Begin/GlobalBarrier recursion. Caller must
// preserve all cross-segment hazards, uploads and pool ownership, and must
// pass a real submission-window resource lease. This helper is not connected
// to the live renderer until its failure/retirement contract is proven.
// PERF-GTS-399: `acquire` gives the list the recorder goes on in (not begun): a secondary of
// this thread, or one recorded through the command ring. The caller makes its command stream
// for the new `list` after a successful rotation.
template<class ResetState,class Acquire>
bool RotateSegment(PassCoordinator& ordered,Rhi::Device& device,u64 epoch,
                   Rhi::CommandList*& list,Acquire&& acquire,
                   std::shared_ptr<const void> lease,std::size_t charged_bytes,
                   ResetState&& reset_state,RotationDiag* diag=nullptr) {
#if !defined(NDEBUG)
    PassCoordinator::TimingProbe timing{ordered,PassCoordinator::TimingPhase::Rotation};
#endif
    if(diag)++diag->attempts;
    if(!list || !lease){if(diag)++diag->invalid;return false;}
    Rhi::CommandList* replacement=nullptr;bool queued=false;
    try {
        // A full budget may become available immediately afterwards; in that
        // case this draw conservatively follows the existing serial fallback.
        if(!ordered.PrefixPreflight(epoch,list,lease,charged_bytes)){if(diag)++diag->preflight;return false;}
        replacement=acquire();
        if(!replacement){if(diag)++diag->acquire;return false;}
        replacement->Begin();
        if(!replacement->NativeHandle() && !replacement->NativeHandleSlot()) {if(diag)++diag->command;device.ReleaseUnsubmitted(replacement);return false;}
        if(!ordered.Prefix(epoch,list,std::move(lease),charged_bytes)) {
            if(diag)++diag->prefix;
            device.ReleaseUnsubmitted(replacement);return false;
        }
        // RHI End and cache invalidation must not throw after the prefix has
        // entered the ordered queue; callers must treat violation as fatal.
        queued=true;
        list->End();list=replacement;reset_state();if(diag)++diag->success;
#if !defined(NDEBUG)
        timing.Success();
#endif
        return true;
    } catch(...) {
        if(queued) std::terminate(); // no safe rollback after queue publication
        if(diag)++diag->exception;
        if(replacement)device.ReleaseUnsubmitted(replacement);
        return false;
    }
}
}
