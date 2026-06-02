""":descr: Submit ttEXPORT with 2 nodes suppressing export sigs, verify it
retries via terRETRY_EXPORT until LLS expiry (insufficient signatures).

Nodes 3 and 4 have XAHAUD_NO_EXPORT_SIG=1, so only 3/5 nodes provide
export signatures. With 80% quorum = ceil(5*0.8) = 4 required, the
export cannot reach quorum and should expire via tecEXPORT_EXPIRED.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT with tight LLS
  3. Export retries (only 3/5 sigs available, need 4)
  4. Verify export expires with tecEXPORT_EXPIRED
  5. Verify subsequent payment still works (sequence not permanently blocked)
"""

from __future__ import annotations

from export_helpers import require_export, assert_shadow_ticket


async def scenario(ctx, log):
    await require_export(ctx, log)

    # --- Setup ---
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    log(f"Current ledger: {current_seq}")
    log("Nodes 3,4 have XAHAUD_NO_EXPORT_SIG=1 (3/5 sigs, need 4)")

    # --- Submit ttEXPORT (should retry then expire -- only 3/5 sigs) ---
    export_start = ctx.mark("export-degradation-submit-start")
    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + 8,
            "Fee": "1000000",
            "ExportedTxn": {
                "TransactionType": "Payment",
                "Account": alice.address,
                "Destination": bob.address,
                "Amount": "1000000",
                "Fee": "10",
                "Sequence": 0,
                "TicketSequence": 1,
                "FirstLedgerSequence": current_seq + 1,
                "LastLedgerSequence": current_seq + 6,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=60,
    )
    export_end = ctx.mark("export-degradation-submit-end")

    final_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    log(f"Export completed at ledger {final_seq}, result: {engine_result}")

    # With only 3/5 sigs and 80% quorum (4 required), export MUST fail
    if engine_result == "tesSUCCESS":
        raise AssertionError(
            "Export should NOT have succeeded with only 3/5 sigs "
            "(need 4 for 80% quorum) -- check XAHAUD_NO_EXPORT_SIG config"
        )

    # Should be tecEXPORT_EXPIRED (LLS reached without quorum)
    if engine_result != "tecEXPORT_EXPIRED":
        log(f"WARNING: expected tecEXPORT_EXPIRED, got {engine_result}")

    log(f"Export failed as expected ({engine_result})")

    retry_logs = ctx.assert_log(
        r"Export: insufficient signatures .*result=terRETRY_EXPORT",
        since=export_start,
        until=export_end,
    )
    log(f"Export insufficient-signature retries: {retry_logs.count}")

    expired_logs = ctx.assert_log(
        r"Export: last ledger expired .*result=tecEXPORT_EXPIRED",
        since=export_start,
        until=export_end,
    )
    log(f"Export LLS expiry logs: {expired_logs.count}")

    # No shadow ticket should exist (export never reached quorum)
    assert_shadow_ticket(ctx, alice.address, log, expect_exists=False)

    # --- Verify subsequent payment works regardless ---
    log("Submitting payment from alice to bob...")
    pay_result = await ctx.submit_and_wait(
        {
            "TransactionType": "Payment",
            "Destination": bob.address,
            "Amount": "1000000",
            "Fee": "12",
        },
        alice.wallet,
        timeout=30,
    )

    pay_engine = pay_result.get("engine_result", "")
    log(f"Payment result: {pay_engine}")

    if pay_engine != "tesSUCCESS":
        raise AssertionError(
            f"Payment failed after expired export: {pay_engine} "
            f"-- sequence may be blocked"
        )

    log("Payment succeeded -- account not permanently blocked")
    log("PASS")
