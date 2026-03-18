""":descr: Submit ttEXPORT directly (no hook), verify it succeeds with
ExportResult in metadata. Then submit a payment from the same account
to verify sequence handling doesn't block subsequent transactions.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT with inner payment -> tesSUCCESS (provisional)
  3. Validators attach sigs via proposals -> quorum -> ExportResult in metadata
  4. alice submits a Payment to bob -> should succeed (sequence not blocked)
"""

from __future__ import annotations

from export_helpers import require_export, assert_export_result, assert_shadow_ticket


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
            "LastLedgerSequence": current_seq + 15,
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
                "LastLedgerSequence": current_seq + 10,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=60,
    )

    export_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    log(f"Export completed at ledger {export_seq}, result: {engine_result}")

    if engine_result != "tesSUCCESS":
        raise AssertionError(
            f"Expected tesSUCCESS for export, got {engine_result}"
        )

    # Assert ExportResult is well-formed with signers
    meta = result.get("meta", {})
    assert_export_result(meta, log, require_signers=True)

    # Assert shadow ticket was created
    assert_shadow_ticket(ctx, alice.address, log, expect_exists=True)

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
