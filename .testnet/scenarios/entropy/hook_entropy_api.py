""":descr: compile, install, and invoke a Hook using the entropy_cr_* API"""

from __future__ import annotations

from helpers import require_entropy


ENTROPY_HOOK_C = r"""
#include <stdint.h>

extern int32_t _g(uint32_t id, uint32_t maxiter);
extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier);
extern int64_t entropy_cr_random(
    uint32_t write_ptr, uint32_t write_len, uint32_t min_tier);
extern int64_t entropy_cr_status(void);

#define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter) + 1)
#define ENTROPY_TIER(x) (((uint64_t)(x) >> 32U) & 0xFFU)
#define ENTROPY_COUNT(x) (((uint64_t)(x) >> 16U) & 0xFFFFU)
#define ENTROPY_DENOMINATOR(x) ((uint64_t)(x) & 0xFFFFU)

int64_t
hook(uint32_t reserved)
{
    _g(1, 1);

    int64_t status = entropy_cr_status();
    if (status < 0)
        return accept(0, 0, 10);

    uint32_t tier = ENTROPY_TIER(status);
    uint32_t count = ENTROPY_COUNT(status);
    uint32_t denominator = ENTROPY_DENOMINATOR(status);
    if (tier < 3 || count < 4 || denominator < count)
        return accept(0, 0, 11);

    int64_t die = entropy_cr_dice(6, 3);
    if (die < 0 || die >= 6)
        return accept(0, 0, 12);

    uint8_t random_bytes[32];
    for (int i = 0; GUARD(32), i < 32; ++i)
        random_bytes[i] = 0;

    if (entropy_cr_random((uint32_t)random_bytes, 32, 3) != 32)
        return accept(0, 0, 13);

    int nonzero = 0;
    for (int i = 0; GUARD(32), i < 32; ++i)
        if (random_bytes[i] != 0)
            nonzero = 1;
    if (!nonzero)
        return accept(0, 0, 14);

    return accept(0, 0, 0);
}
"""


def assert_success(result, operation):
    meta = result.get("meta", result.get("metaData", {}))
    tx_result = meta.get("TransactionResult", result.get("engine_result", ""))
    if tx_result != "tesSUCCESS":
        raise AssertionError(f"{operation} failed: {result}")
    return meta


async def scenario(ctx, log):
    await require_entropy(ctx, log)
    await ctx.wait_for_ledgers(3, node_id=0, timeout=60)

    await ctx.fund_accounts({"entropy_api": 1000})
    account = ctx.account("entropy_api")

    wasm = ctx.compile_hook(ENTROPY_HOOK_C, label="entropy-cr-api")
    install = await ctx.submit_and_wait(
        {
            "TransactionType": "SetHook",
            "Hooks": [
                {
                    "Hook": {
                        "CreateCode": wasm.hex().upper(),
                        "HookOn": "0" * 64,
                        "HookNamespace": "0" * 64,
                        "HookApiVersion": 0,
                        "Flags": 1,
                    }
                }
            ],
            "Fee": "100000000",
        },
        account.wallet,
    )
    assert_success(install, "SetHook")
    log("entropy_cr_* Hook installed")

    invoke = await ctx.submit_and_wait(
        {
            "TransactionType": "Invoke",
            "Fee": "1000000",
        },
        account.wallet,
    )
    meta = assert_success(invoke, "Invoke")

    executions = meta.get("HookExecutions", [])
    if len(executions) != 1:
        raise AssertionError(f"Expected one HookExecution, got: {executions}")

    execution = executions[0].get("HookExecution", {})
    if execution.get("HookResult") != 3:
        raise AssertionError(f"Hook did not ACCEPT: {execution}")

    return_code = execution.get("HookReturnCode", "")
    if return_code and str(return_code) != "0":
        raise AssertionError(f"entropy_cr_* Hook check failed: {execution}")

    log("entropy_cr_status, entropy_cr_dice, and entropy_cr_random passed")
    log("PASS")
