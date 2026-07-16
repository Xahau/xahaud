""":descr: a 2-of-5 Export committee recovers without a new intent

The intent selects only validators n0 and n4, so qC is 2. Validator n4 is
stopped before admission: the network still validates the intent with 4/5, but
one selected share cannot form a witness. Restarting n4 must republish its share
for the same live latch and complete the witness without resubmitting Export.
"""

from __future__ import annotations

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    assert_export_latch,
    bitmap_positions,
    find_export_signature_witness,
    find_export_txns,
    require_export,
    submit_direct_export,
    wait_for_export_signature_witness,
)


async def scenario(ctx, log):
    await require_export(ctx, log)
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    alice = ctx.account("alice")
    bob = ctx.account("bob")

    if not ctx.stop_node(4):
        raise AssertionError("Failed to stop selected validator n4")
    await ctx.wait_for_nodes_down(nodes=[4], timeout=30)
    log("Stopped selected validator n4; selected committee is n0+n4")

    current = ctx.validated_ledger_index(0)
    result = await submit_direct_export(
        ctx,
        log,
        {
            "TransactionType": "Export",
            "Fee": "1000000",
            "ExportedTxn": {
                "TransactionType": "Payment",
                "Account": alice.address,
                "Destination": bob.address,
                "Amount": "1000000",
                "Fee": "10",
                "Sequence": 0,
                "TicketSequence": 1,
                "FirstLedgerSequence": current + 1,
                "LastLedgerSequence": current + EXPORT_RETRY_LEDGER_WINDOW,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        committee_node_ids=[0, 4],
    )
    if result.get("engine_result") != "tesSUCCESS":
        raise AssertionError(f"Export intent failed: {result}")

    origin = result.get("hash")
    origin_seq = int(result.get("ledger_index"))
    if not origin:
        raise AssertionError(f"Validated Export missing hash: {result}")

    assert_export_latch(
        ctx,
        alice.address,
        log,
        origin_hash=origin,
        expect_witness=False,
    )
    selected = {0, 1}

    await ctx.wait_for_ledger(origin_seq + 1, node_id=0, timeout=30)
    if find_export_signature_witness(ctx, origin_seq + 1, origin):
        raise AssertionError(
            "Witness formed while one of two selected signers was down"
        )
    assert_export_latch(
        ctx,
        alice.address,
        log,
        origin_hash=origin,
        expect_witness=False,
    )
    log("No witness with only one selected signer; original latch remains pending")

    if not ctx.start_node(4):
        raise AssertionError("Failed to restart selected validator n4")
    await ctx.wait_for_nodes(
        lambda node_id: ctx.rpc.server_info(node_id) is not None,
        nodes=[4],
        timeout=30,
        poll_interval=0.5,
        name="selected-export-validator-up",
    )
    log("Restarted selected validator n4")

    witness = await wait_for_export_signature_witness(
        ctx, log, origin, after_ledger=origin_seq
    )
    contributors = bitmap_positions(witness["EntropyContributors"])
    if contributors != selected:
        raise AssertionError(
            f"Recovered witness contributors {contributors} != selected {selected}"
        )
    if len(witness["_WitnessSigners"]) != 2:
        raise AssertionError(
            f"Recovered witness has {len(witness['_WitnessSigners'])} signers, need 2"
        )
    witness_seq = int(witness["LedgerSequence"])
    for seq in range(origin_seq + 1, witness_seq + 1):
        if any(tx.get("Account") == alice.address for tx in find_export_txns(ctx, seq)):
            raise AssertionError(
                f"A second Export was submitted before recovery in ledger {seq}"
            )
    assert_export_latch(
        ctx,
        alice.address,
        log,
        origin_hash=origin,
        expect_witness=True,
    )
    log(f"Same Export {origin} completed after selected validator recovery")
    log("PASS")
