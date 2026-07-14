""":descr: Export fails closed without a ledger-anchored UNLReport view.

Network-mode Export must not derive authority from a node-local trusted-config
view. An explicit parent binding still fails if that parent has no UNLReport,
and no shadow ticket is created.
"""

from __future__ import annotations

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    assert_shadow_ticket,
    export_authority,
    require_export,
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

    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + EXPORT_RETRY_LEDGER_WINDOW,
            "Fee": "1000000",
            **export_authority(ctx, require_unl_report=False),
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

    final_seq = ctx.validated_ledger_index(0)
    engine_result = result.get("engine_result", "")
    log(f"Export completed at ledger {final_seq}, result: {engine_result}")

    if engine_result == "tesSUCCESS":
        raise AssertionError(
            "Export should not succeed without a ledger-anchored UNLReport view"
        )

    if engine_result != "tecEXPORT_UNIVERSE_MISMATCH":
        raise AssertionError(
            "Expected tecEXPORT_UNIVERSE_MISMATCH without UNLReport view, "
            f"got {engine_result}"
        )

    assert_shadow_ticket(ctx, alice.address, log, expect_exists=False)

    log("PASS")
