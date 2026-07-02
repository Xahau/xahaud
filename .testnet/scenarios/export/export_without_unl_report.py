""":descr: Export retries/expires without a ledger-anchored UNLReport view.

All validators may sign, but network-mode Export must not assemble quorum
material from a node-local trusted-config view. Without UNLReport, the export
should retry until LastLedgerSequence and expire without creating a shadow
ticket.
"""

from __future__ import annotations

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    require_export,
    assert_shadow_ticket,
)


async def scenario(ctx, log):
    await require_export(ctx, log, require_unl_report=False)

    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    log(f"Current ledger: {current_seq}")
    log("UNLReport intentionally absent; export must not use local config view")

    export_start = ctx.mark("export-without-unlreport-submit-start")
    result = await ctx.submit_and_wait(
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
    export_end = ctx.mark("export-without-unlreport-submit-end")

    final_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    log(f"Export completed at ledger {final_seq}, result: {engine_result}")

    if engine_result == "tesSUCCESS":
        raise AssertionError(
            "Export should not succeed without a ledger-anchored UNLReport view"
        )

    # Be exact: without a UNLReport view the export should retry until LLS and
    # expire, not fail by some unrelated terminal code.
    if engine_result != "tecEXPORT_EXPIRED":
        raise AssertionError(
            "Expected tecEXPORT_EXPIRED without UNLReport view, "
            f"got {engine_result}"
        )

    warning_logs = ctx.assert_log(
        r"Export: retrying without ledger-anchored validator view",
        since=export_start,
        until=export_end,
    )
    log(f"Export no-UNLReport retry warnings: {warning_logs.count}")

    retry_logs = ctx.assert_log(
        r"Export: insufficient signatures .*result=terRETRY_EXPORT",
        since=export_start,
        until=export_end,
    )
    log(f"Export retry logs: {retry_logs.count}")

    expired_logs = ctx.assert_log(
        r"Export: last ledger expired .*result=tecEXPORT_EXPIRED",
        since=export_start,
        until=export_end,
    )
    log(f"Export expiry logs: {expired_logs.count}")

    assert_shadow_ticket(ctx, alice.address, log, expect_exists=False)

    log("PASS")
