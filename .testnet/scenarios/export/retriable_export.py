""":descr: Submit ttEXPORT directly (no hook), verify the intent is admitted
and a later validated ledger records its signature witness. Then submit a
payment from the same account to verify sequence handling remains independent.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT with an explicit parent-universe declaration
  3. Validation releases shares; a later ledger records ExportSignatures
  4. alice submits a Payment to bob -> should succeed (sequence not blocked)
"""

from __future__ import annotations

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    assert_shadow_ticket,
    export_authority,
    require_export,
    wait_for_export_signature_witness,
)


async def scenario(ctx, log):
    await require_export(ctx, log)

    # --- Setup ---
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    log(f"Current ledger: {current_seq}")

    # --- 1. Submit ttEXPORT ---
    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + EXPORT_RETRY_LEDGER_WINDOW,
            "Fee": "1000000",
            **export_authority(ctx),
            "ExportedTxn": {
                "TransactionType": "Payment",
                "Account": alice.address,
                "Destination": bob.address,
                "Amount": "1000000",
                "Fee": "10",
                "Sequence": 0,
                "TicketSequence": 1,
                "FirstLedgerSequence": current_seq + 1,
                "LastLedgerSequence": current_seq + EXPORT_RETRY_LEDGER_WINDOW,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=60,
    )

    export_seq = result.get("ledger_index", ctx.validated_ledger_index(0))
    origin_hash = result.get("hash")
    engine_result = result.get("engine_result", "")
    log(f"Export completed at ledger {export_seq}, result: {engine_result}")

    if engine_result != "tesSUCCESS":
        raise AssertionError(f"Expected tesSUCCESS for export, got {engine_result}")

    if not origin_hash:
        raise AssertionError(f"Validated Export missing hash: {result}")

    await wait_for_export_signature_witness(
        ctx, log, origin_hash, after_ledger=export_seq
    )

    assert_shadow_ticket(
        ctx,
        alice.address,
        log,
        expect_exists=True,
        origin_hash=origin_hash,
        expect_witness=True,
    )

    # --- 2. Submit Payment from same account ---
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
        raise AssertionError(f"Payment failed: {pay_engine}")

    log(
        f"Both transactions succeeded: "
        f"Export at ledger {export_seq}, Payment at ledger {ctx.validated_ledger_index(0)}"
    )
    log("Sequence handling OK - export didn't block subsequent txns")
    log("PASS")
