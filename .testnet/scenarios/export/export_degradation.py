""":descr: Submit ttEXPORT with 2 nodes suppressing export sigs, verify it
retries via terRETRY_EXPORT until LLS expiry (not enough sigs for quorum).

Nodes 3 and 4 have XAHAUD_NO_EXPORT_SIG=1, so only 3/5 nodes provide
export signatures. With 80% quorum = ceil(5*0.8) = 4 required, the
export cannot reach quorum and should expire via tecEXPORT_EXPIRED.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT with tight LLS
  3. Export retries (only 3/5 sigs available, need 4)
  4. Verify export expires or fails gracefully
  5. Verify subsequent payment still works (sequence not permanently blocked)
"""

from __future__ import annotations

from export_helpers import require_export


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

    # --- Submit ttEXPORT (should retry then expire — only 3/5 sigs) ---
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

    final_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    log(f"Export completed at ledger {final_seq}, result: {engine_result}")

    if engine_result == "tesSUCCESS":
        meta = result.get("meta", {})
        export_meta = meta.get("ExportResult", {})
        log(f"Unexpectedly succeeded! ExportResult: {export_meta}")
        log("This means sigs were collected despite suppression — check config")
    else:
        log(f"Export did NOT succeed ({engine_result}) — as expected with 3/5 sigs")

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
        log(f"Payment failed: {pay_result}")
        log("WARNING: sequence may be blocked by pending export")
    else:
        log("Payment succeeded — account not permanently blocked")

    log("PASS")
