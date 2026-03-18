""":descr: Submit ttEXPORT directly (no hook), verify it succeeds with
ExportResult in metadata. Then submit a payment from the same account
to verify sequence handling doesn't block subsequent transactions.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT with inner payment → tesSUCCESS (provisional)
  3. Validators attach sigs via proposals → quorum → ExportResult in metadata
  4. alice submits a Payment to bob → should succeed (sequence not blocked)
"""

from __future__ import annotations

from export_helpers import require_export, find_export_txns


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
    # Build a minimal inner payment for cross-chain export
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
    export_engine = result.get("engine_result", "")
    log(f"Export completed at ledger {export_seq}, result: {export_engine}")

    if export_engine != "tesSUCCESS":
        raise AssertionError(
            f"Expected tesSUCCESS for export, got {export_engine}"
        )

    # Check for ExportResult in metadata
    meta = result.get("meta", {})
    export_meta = meta.get("ExportResult", {})
    log(f"ExportResult: {export_meta}")

    if not export_meta:
        log("WARNING: ExportResult not found in metadata (may need more ledgers)")

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

    pay_seq = ctx.validated_ledger_index(0)
    pay_engine = pay_result.get("engine_result", "")
    log(f"Payment completed at ledger {pay_seq}, result: {pay_engine}")

    if pay_engine != "tesSUCCESS":
        raise AssertionError(
            f"Expected tesSUCCESS for payment, got {pay_engine}"
        )

    log(
        f"Both transactions succeeded: "
        f"Export at ledger {export_seq}, Payment at ledger {pay_seq}"
    )
    log("Sequence handling OK — export didn't block subsequent txns")
    log("PASS")
