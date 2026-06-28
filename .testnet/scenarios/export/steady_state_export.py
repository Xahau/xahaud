""":descr: install xport hook, trigger export, verify emitted ttEXPORT lifecycle

  1. Fund alice (hook holder), bob (trigger), carol (export destination)
  2. Install xport hook on alice
  3. bob pays alice with DST=carol → hook calls xport() → emits ttEXPORT
  4. Emitted ttEXPORT enters open ledger, validators attach sigs via proposals
  5. Verify Export transaction appears in a subsequent ledger
"""

from __future__ import annotations

from export_helpers import (
    require_export,
    find_export_txns,
    dst_param,
    assert_hook_accepted,
    assert_export_result,
    assert_shadow_ticket,
)

# C source for the xport hook — verbatim from src/test/app/Export_test_hooks.h
# On Payment to the hook account, exports a 1 XAH payment to the DST param.
XPORT_HOOK_C = r"""
#include <stdint.h>
extern int32_t _g(uint32_t id, uint32_t maxiter);
extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len);
extern int64_t xport_reserve(uint32_t count);
extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
extern int64_t otxn_param(uint32_t write_ptr, uint32_t write_len, uint32_t name_ptr, uint32_t name_len);
extern int64_t otxn_type(void);
extern int64_t ledger_seq(void);

#define SBUF(x) (uint32_t)(x), sizeof(x)
#define ASSERT(x) if (!(x)) rollback((uint32_t)#x, sizeof(#x), __LINE__)

#define ttPAYMENT 0
#define tfCANONICAL 0x80000000UL
#define amAMOUNT 1
#define amFEE 8
#define atACCOUNT 1
#define atDESTINATION 3

#define ENCODE_TT(buf_out, tt) \
    buf_out[0] = 0x12U; buf_out[1] = (tt >> 8) & 0xFFU; buf_out[2] = tt & 0xFFU; buf_out += 3;

#define ENCODE_FLAGS(buf_out, flags) \
    buf_out[0] = 0x22U; buf_out[1] = (flags >> 24) & 0xFFU; buf_out[2] = (flags >> 16) & 0xFFU; \
    buf_out[3] = (flags >> 8) & 0xFFU; buf_out[4] = flags & 0xFFU; buf_out += 5;

#define ENCODE_SEQUENCE(buf_out, seq) \
    buf_out[0] = 0x24U; buf_out[1] = (seq >> 24) & 0xFFU; buf_out[2] = (seq >> 16) & 0xFFU; \
    buf_out[3] = (seq >> 8) & 0xFFU; buf_out[4] = seq & 0xFFU; buf_out += 5;

#define ENCODE_FLS(buf_out, fls) \
    buf_out[0] = 0x20U; buf_out[1] = 0x1AU; buf_out[2] = (fls >> 24) & 0xFFU; \
    buf_out[3] = (fls >> 16) & 0xFFU; buf_out[4] = (fls >> 8) & 0xFFU; \
    buf_out[5] = fls & 0xFFU; buf_out += 6;

#define ENCODE_LLS(buf_out, lls) \
    buf_out[0] = 0x20U; buf_out[1] = 0x1BU; buf_out[2] = (lls >> 24) & 0xFFU; \
    buf_out[3] = (lls >> 16) & 0xFFU; buf_out[4] = (lls >> 8) & 0xFFU; \
    buf_out[5] = lls & 0xFFU; buf_out += 6;

#define ENCODE_DROPS(buf_out, drops, amt_type) \
    buf_out[0] = 0x60U + amt_type; buf_out[1] = 0x40U + ((drops >> 56) & 0x3FU); \
    buf_out[2] = (drops >> 48) & 0xFFU; buf_out[3] = (drops >> 40) & 0xFFU; \
    buf_out[4] = (drops >> 32) & 0xFFU; buf_out[5] = (drops >> 24) & 0xFFU; \
    buf_out[6] = (drops >> 16) & 0xFFU; buf_out[7] = (drops >> 8) & 0xFFU; \
    buf_out[8] = drops & 0xFFU; buf_out += 9;

#define ENCODE_SIGNING_PUBKEY_EMPTY(buf_out) \
    buf_out[0] = 0x73U; buf_out[1] = 0x00U; buf_out += 2;

#define ENCODE_ACCOUNT(buf_out, acc, acc_type) \
    buf_out[0] = 0x80U + acc_type; buf_out[1] = 0x14U; \
    for (int i = 0; i < 20; ++i) buf_out[2+i] = acc[i]; buf_out += 22;

#define PREPARE_PAYMENT_SIMPLE_SIZE 270U

int64_t hook(uint32_t reserved) {
    _g(1, 1);

    if (otxn_type() != ttPAYMENT)
        return accept(0, 0, 0);

    ASSERT(xport_reserve(1) == 1);

    uint8_t dst[20];
    int64_t dst_len = otxn_param(SBUF(dst), "DST", 3);
    ASSERT(dst_len == 20);

    uint8_t acc[20];
    ASSERT(hook_account(SBUF(acc)) == 20);

    uint32_t cls = (uint32_t)ledger_seq();

    uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
    uint8_t* buf = tx;

    ENCODE_TT(buf, ttPAYMENT);
    ENCODE_FLAGS(buf, tfCANONICAL);
    ENCODE_SEQUENCE(buf, 0);
    ENCODE_FLS(buf, cls + 1);
    ENCODE_LLS(buf, cls + 5);
    // sfTicketSequence = UINT32 field 41 = 0x20 0x29
    buf[0] = 0x20U; buf[1] = 0x29U;
    buf[2] = 0; buf[3] = 0; buf[4] = 0; buf[5] = 1;
    buf += 6;

    uint64_t drops = 1000000;
    ENCODE_DROPS(buf, drops, amAMOUNT);
    ENCODE_DROPS(buf, 10, amFEE);

    ENCODE_SIGNING_PUBKEY_EMPTY(buf);
    ENCODE_ACCOUNT(buf, acc, atACCOUNT);
    ENCODE_ACCOUNT(buf, dst, atDESTINATION);

    uint8_t hash[32];
    int64_t xport_result = xport(SBUF(hash), (uint32_t)tx, buf - tx);
    ASSERT(xport_result == 32);

    return accept(0, 0, 0);
}
"""


async def scenario(ctx, log):
    # Wait for network to start and amendments to activate
    await require_export(ctx, log)

    # --- Setup ---
    await ctx.fund_accounts({"alice": 10000, "bob": 10000, "carol": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    carol = ctx.account("carol")

    # Compile and install xport hook on alice
    wasm = ctx.compile_hook(XPORT_HOOK_C, label="xport")
    await ctx.submit_and_wait(
        {
            "TransactionType": "SetHook",
            "Hooks": [
                {
                    "Hook": {
                        "CreateCode": wasm.hex().upper(),
                        "HookOn": "0" * 64,
                        "HookNamespace": "0" * 64,
                        "HookApiVersion": 0,
                        "Flags": 1,  # hsfOVERRIDE
                    }
                }
            ],
            "Fee": "100000000",
        },
        alice.wallet,
    )
    log(
        f"Hook installed on alice ({alice.address[:12]}...) "
        f"ledger {ctx.validated_ledger_index(0)}"
    )

    # --- Trigger ---
    # bob pays alice → hook calls xport() → emits ttEXPORT
    trigger_result = await ctx.submit_and_wait(
        {
            "TransactionType": "Payment",
            "Destination": alice.address,
            "Amount": "100000000",
            "Fee": "1000000",
            "HookParameters": [dst_param(carol.address)],
        },
        ctx.account("bob").wallet,
    )
    trigger_seq = ctx.validated_ledger_index(0)
    log(f"Export triggered at ledger {trigger_seq}")

    # xport() schedules a ttEXPORT through the emitted directory, but hook
    # metadata reports it separately from ordinary HookEmissions.
    trigger_meta = trigger_result.get("meta", {})
    assert_hook_accepted(trigger_meta, log, expected_emits=0, expected_exports=1)

    # --- Verify: check each ledger close for the Export transaction ---
    max_ledgers = 10
    for i in range(max_ledgers):
        await ctx.wait_for_ledgers(1, node_id=0, timeout=30)
        seq = ctx.validated_ledger_index(0)
        exports = find_export_txns(ctx, seq)
        if exports:
            export_tx = exports[0]
            meta = export_tx.get("meta", export_tx.get("metaData", {}))
            result = meta.get("TransactionResult", "")
            log(f"Ledger {seq}: Export txn found, result={result}")

            if result != "tesSUCCESS":
                raise AssertionError(f"Export did not succeed: {result}")

            # Assert ExportResult is well-formed with signers and inner tx
            assert_export_result(meta, log, ctx=ctx, require_signers=True)

            # Assert shadow ticket was created
            assert_shadow_ticket(ctx, alice.address, log, expect_exists=True)

            log("PASS")
            return
        log(f"Ledger {seq}: no Export txn yet")

    raise AssertionError(
        f"No Export transaction found after {max_ledgers} ledger closes"
    )
