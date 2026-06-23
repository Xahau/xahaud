""":descr: Export succeeds when quorum sidecar material exists but one active
validator withholds exportSigSetHash observation.

Node 4 has runtime_config no_export_sig_hash=true. It still attaches export
signatures, but it does not publish its exportSigSetHash in proposals. The
remaining 4/5 active validators can still align on the same export sidecar
hash, so the round must not retry/expire just because fullObservation is false.
"""

from __future__ import annotations

from export_helpers import (
    require_export,
    assert_export_result,
    assert_shadow_ticket,
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
    export_end = ctx.mark("export-no-veto-submit-end")

    final_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    meta = result.get("meta", {})

    log(f"Export completed at ledger {final_seq}, result: {engine_result}")
    if engine_result != "tesSUCCESS":
        raise AssertionError(f"Expected tesSUCCESS, got {engine_result}")

    export_result = assert_export_result(meta, log, require_signers=True)
    signers = export_result.get("ExportedTxn", {}).get("Signers", [])
    if len(signers) < 4:
        raise AssertionError(f"Expected at least 4 signers, got {len(signers)}")
    log(f"Export signer count: {len(signers)}")

    no_veto_logs = ctx.assert_log(
        r"Export: missing exportSigSetHash observation ignored",
        since=export_start,
        until=export_end,
    )
    log(f"Export no-veto missing-observation logs: {no_veto_logs.count}")

    withhold_logs = ctx.assert_log(
        r"Export: withholding exportSigSetHash",
        since=export_start,
        until=export_end,
    )
    log(f"Export sidecar hash withholding logs: {withhold_logs.count}")

    assert_shadow_ticket(ctx, alice.address, log, expect_exists=True)

    log("PASS")
