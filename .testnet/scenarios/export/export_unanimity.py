""":descr: Test Export without CE (unanimity mode). When all 5 nodes are up,
100% quorum is reachable and the export should succeed. When 1 node
suppresses sigs (4/5), unanimity fails and the export should expire.

Parameterized via `expect_success` kwarg from suite.yml.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT
  3. Verify result matches expectation (tesSUCCESS or tecEXPORT_EXPIRED)
  4. Verify subsequent payment works regardless
"""

from __future__ import annotations

from export_helpers import require_export


async def scenario(ctx, log, expect_success=True):
    await require_export(ctx, log)

    # --- Setup ---
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    log(f"Current ledger: {current_seq}")
    log(f"Expecting export {'success' if expect_success else 'failure (unanimity)'}")

    # --- Submit ttEXPORT ---
    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + 10,
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
                "LastLedgerSequence": current_seq + 8,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=60,
    )

    final_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    meta = result.get("meta", {})
    export_result = meta.get("ExportResult", {})

    log(f"Export at ledger {final_seq}, result: {engine_result}")
    if export_result:
        log(f"  ExportResult: {export_result}")

    if expect_success:
        if engine_result != "tesSUCCESS":
            raise AssertionError(
                f"Expected tesSUCCESS, got {engine_result}"
            )
        if not export_result:
            raise AssertionError("ExportResult not found in metadata")
        log("Export succeeded as expected (all validators signed)")
    else:
        if engine_result == "tesSUCCESS":
            raise AssertionError(
                "Export should NOT have succeeded with sub-unanimity"
            )
        log(f"Export failed as expected ({engine_result})")

    # --- Verify subsequent payment works ---
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

    log("Payment succeeded — account not blocked")
    log("PASS")
