""":descr: Submit ttEXPORT with 2 nodes suppressing export signatures and
verify the admitted intent remains unwitnessed through its publication window.

Nodes 3 and 4 have runtime_config no_export_sig=true, so only 3/5 nodes
provide export signatures. With 80% quorum = ceil(5*0.8) = 4 required,
the export cannot reach quorum and no ExportSignatures witness may be recorded.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT with an explicit authority declaration
  3. Only 3/5 post-validation shares become available (need 4)
  4. Verify the publication window closes without a witness
  5. Verify subsequent payment still works (sequence not permanently blocked)
"""

from __future__ import annotations

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    assert_export_latch,
    require_export,
    submit_direct_export,
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
    log("Nodes 3,4 have runtime_config no_export_sig=true (3/5 sigs, need 4)")

    #@@start test-export-below-quorum-expiry
    # --- Submit intent; only 3/5 validators release shares. ---
    result = await submit_direct_export(
        ctx,
        log,
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + EXPORT_RETRY_LEDGER_WINDOW,
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
                "LastLedgerSequence": current_seq + EXPORT_RETRY_LEDGER_WINDOW,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=60,
    )

    final_seq = result.get("ledger_index", ctx.validated_ledger_index(0))
    origin_hash = result.get("hash")
    engine_result = result.get("engine_result", "")
    log(f"Export intent admitted at ledger {final_seq}, result: {engine_result}")

    if engine_result != "tesSUCCESS":
        raise AssertionError(f"Expected admitted intent, got {engine_result}")
    if not origin_hash:
        raise AssertionError(f"Validated Export missing hash: {result}")

    await wait_for_export_signature_witness(
        ctx,
        log,
        origin_hash,
        after_ledger=final_seq,
        expect_witness=False,
    )
    assert_export_latch(
        ctx,
        alice.address,
        log,
        expect_exists=True,
        origin_hash=origin_hash,
        expect_witness=False,
    )
    #@@end test-export-below-quorum-expiry

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
            f"Payment failed after unwitnessed export: {pay_engine} "
            f"-- sequence may be blocked"
        )

    log("Payment succeeded -- account not permanently blocked")
    log("PASS")
