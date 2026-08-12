#include <xrpld/app/hook/HookHostOperations.h>
#include <xrpld/app/hook/applyHook.h>

using namespace ripple;

// Cause the originating transaction to go through, save state changes and emit
// emitted tx, exit hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    accept,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code)
{
    HOOK_SETUP();
    return hook::raw::accept(
        hookCtx,
        hook::HookGuestMemory{memory, memory_length},
        read_ptr,
        read_len,
        error_code);
    HOOK_TEARDOWN();
}
// Cause the originating transaction to be rejected, discard state changes and
// discard emitted tx, exit hook
DEFINE_HOOK_FUNCTION(
    int64_t,
    rollback,
    uint32_t read_ptr,
    uint32_t read_len,
    int64_t error_code)
{
    HOOK_SETUP();
    return hook::raw::rollback(
        hookCtx,
        hook::HookGuestMemory{memory, memory_length},
        read_ptr,
        read_len,
        error_code);
    HOOK_TEARDOWN();
}
// Guard function... very important. Enforced on SetHook transaction, keeps
// track of how many times a runtime loop iterates and terminates the hook if
// the iteration count rises above a preset number of iterations as determined
// by the hook developer
DEFINE_HOOK_FUNCTION(int32_t, _g, uint32_t id, uint32_t maxitr)
{
    HOOK_SETUP();  // populates memory_ctx, memory, memory_length, applyCtx,
                   // hookCtx on current stack

    if (hookCtx.guard_map.find(id) == hookCtx.guard_map.end())
        hookCtx.guard_map[id] = 1;
    else
        hookCtx.guard_map[id]++;

    if (hookCtx.guard_map[id] > maxitr)
    {
        if (id > 0xFFFFU)
        {
            JLOG(j.trace())
                << "HookInfo[" << HC_ACC() << "]: Macro guard violation. "
                << "Src line: " << (id & 0xFFFFU) << " "
                << "Macro line: " << (id >> 16) << " "
                << "Iterations: " << hookCtx.guard_map[id];
        }
        else
        {
            JLOG(j.trace()) << "HookInfo[" << HC_ACC() << "]: Guard violation. "
                            << "Src line: " << id << " "
                            << "Iterations: " << hookCtx.guard_map[id];
        }
        hookCtx.result.exitType = hook_api::ExitType::ROLLBACK;
        hookCtx.result.exitCode = (int64_t)GUARD_VIOLATION;
        return RC_ROLLBACK;
    }
    return 1U;

    HOOK_TEARDOWN();
}
