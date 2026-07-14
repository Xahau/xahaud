""":descr: Test Export witness quorum behavior. Every valid intent is admitted;
enough selected validators produce a later witness, while a below-quorum intent
remains unwitnessed through its bounded publication window.

Parameterized via `expect_success` kwarg from suite.yml.

Flow:
  1. Fund alice and bob
  2. alice submits ttEXPORT
  3. Verify the intent validates with tesSUCCESS
  4. Verify a later witness exists only when the committee reaches quorum
  5. Verify subsequent payment works regardless
"""

from __future__ import annotations

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    assert_shadow_ticket,
    require_export,
    submit_direct_export,
    wait_for_export_signature_witness,
)


async def scenario(ctx, log, expect_success=True):
    await require_export(ctx, log)

    # --- Setup ---
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    log(f"Current ledger: {current_seq}")
    outcome = "success" if expect_success else "failure (below quorum)"
    log(f"Expecting export {outcome}")

    # --- Submit ttEXPORT ---
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

    log(f"Export at ledger {final_seq}, result: {engine_result}")
    if engine_result != "tesSUCCESS":
        raise AssertionError(f"Expected intent tesSUCCESS, got {engine_result}")
    if not origin_hash:
        raise AssertionError(f"Validated Export missing hash: {result}")

    if expect_success:
        await wait_for_export_signature_witness(
            ctx, log, origin_hash, after_ledger=final_seq
        )
        assert_shadow_ticket(
            ctx,
            alice.address,
            log,
            expect_exists=True,
            origin_hash=origin_hash,
            expect_witness=True,
        )

        log("Export succeeded as expected (active-view quorum reached)")
    else:
        await wait_for_export_signature_witness(
            ctx,
            log,
            origin_hash,
            after_ledger=final_seq,
            expect_witness=False,
        )
        assert_shadow_ticket(
            ctx,
            alice.address,
            log,
            expect_exists=True,
            origin_hash=origin_hash,
            expect_witness=False,
        )
        log("Intent remained unwitnessed as expected (below committee quorum)")

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

    log("Payment succeeded -- account not blocked")
    log("PASS")
