""":descr: Export succeeds when quorum sidecar material exists but one active
validator withholds exportSigSetHash observation.

Node 4 has runtime_config no_export_sig_hash=true. It still attaches export
signatures, but it does not publish its exportSigSetHash in proposals. The
remaining 4/5 active validators can still align on the same export sidecar
hash, so the round must not retry/expire just because fullObservation is false.
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

    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    log(f"Current ledger: {current_seq}")
    log("Node 4 withholds exportSigSetHash but still attaches export signatures")

    export_start = ctx.mark("export-no-veto-submit-start")
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

    log(f"Export completed at ledger {final_seq}, result: {engine_result}")
    if engine_result != "tesSUCCESS":
        raise AssertionError(f"Expected tesSUCCESS, got {engine_result}")
    if not origin_hash:
        raise AssertionError(f"Validated Export missing hash: {result}")

    witness = await wait_for_export_signature_witness(
        ctx, log, origin_hash, after_ledger=final_seq
    )
    signers = witness.get("_WitnessSigners", [])
    if len(signers) < 4:
        raise AssertionError(f"Expected at least 4 signers, got {len(signers)}")
    log(f"Export signer count: {len(signers)}")

    # The validated witness proves the missing observation did not veto the
    # round. Pin the injected fault separately; the internal no-veto diagnostic
    # may be flushed after witness observation and is not part of the contract.
    withhold_logs = ctx.assert_log(
        r"Export: withholding exportSigSetHash",
        since=export_start,
    )
    log(f"Export sidecar hash withholding logs: {withhold_logs.count}")

    assert_export_latch(
        ctx,
        alice.address,
        log,
        expect_exists=True,
        origin_hash=origin_hash,
        expect_witness=True,
    )

    log("PASS")
