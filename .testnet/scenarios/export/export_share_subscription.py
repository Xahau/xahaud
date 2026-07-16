""":descr: subscribe over a real WebSocket to post-validation Export shares

The subscriber opens before the Export is submitted. It proves the stream
publishes a quorum of independently attributable shares for the exact validated
origin and that those same signature records form the later ledger witness.
"""

from __future__ import annotations

import asyncio
import contextlib
import json

import websockets
from xahaud_scripts.testnet.config import _decode_node_public_key

from export_helpers import (
    EXPORT_RETRY_LEDGER_WINDOW,
    assert_export_latch,
    bitmap_positions,
    require_export,
    submit_direct_export,
    wait_for_export_signature_witness,
)


def _witness_records(witness):
    positions = sorted(bitmap_positions(witness["EntropyContributors"]))
    signers = witness["_WitnessSigners"]
    if len(positions) != len(signers):
        raise AssertionError("Witness bitmap and signer count differ")
    return {
        (
            position,
            signer["SigningPubKey"].upper(),
            signer["TxnSignature"].upper(),
        )
        for position, signer in zip(positions, signers, strict=True)
    }


async def scenario(ctx, log):
    await require_export(ctx, log)
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    alice = ctx.account("alice")
    bob = ctx.account("bob")

    network = json.loads((ctx.base_dir / "network.json").read_text())
    node0 = next(node for node in network["nodes"] if int(node["id"]) == 0)
    ws_url = f"ws://127.0.0.1:{node0['port_ws']}"
    events = []

    async with websockets.connect(ws_url, open_timeout=10) as websocket:
        await websocket.send(
            json.dumps(
                {
                    "id": 1,
                    "command": "subscribe",
                    "streams": ["export_signatures"],
                }
            )
        )
        ack = json.loads(await asyncio.wait_for(websocket.recv(), timeout=10))
        if ack.get("status") != "success":
            raise AssertionError(f"export_signatures subscription failed: {ack}")
        log("Subscribed to export_signatures over WebSocket")

        async def receive_events():
            while True:
                event = json.loads(await websocket.recv())
                if event.get("stream") == "export_signatures":
                    events.append(event)

        reader = asyncio.create_task(receive_events())
        try:
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
                        "LastLedgerSequence": current
                        + EXPORT_RETRY_LEDGER_WINDOW,
                        "Flags": 2147483648,
                        "SigningPubKey": "",
                    },
                },
                alice.wallet,
            )
            if result.get("engine_result") != "tesSUCCESS":
                raise AssertionError(f"Export failed: {result}")

            origin = result.get("hash")
            origin_seq = int(result.get("ledger_index"))
            if not origin:
                raise AssertionError(f"Validated Export missing hash: {result}")
            origin_ledger = ctx.ledger(origin_seq) or {}
            origin_hash = origin_ledger.get("ledger_hash") or origin_ledger.get(
                "ledger", {}
            ).get("hash")
            if not origin_hash:
                raise AssertionError(
                    f"Validated origin ledger {origin_seq} missing hash"
                )

            latches = assert_export_latch(
                ctx,
                alice.address,
                log,
                origin_hash=origin,
                expect_witness=False,
            )
            selected = bitmap_positions(latches[0]["ExportCommittee"])
            quorum = (4 * len(selected) + 4) // 5

            witness = await wait_for_export_signature_witness(
                ctx, log, origin, after_ledger=origin_seq
            )
            expected_records = _witness_records(witness)

            deadline = asyncio.get_running_loop().time() + 10
            while asyncio.get_running_loop().time() < deadline:
                matching = [event for event in events if event.get("origin_txid") == origin]
                observed_records = {
                    (
                        int(event["universe_position"]),
                        _decode_node_public_key(event["signing_key"]),
                        event["signature"].upper(),
                    )
                    for event in matching
                }
                if expected_records <= observed_records:
                    break
                await asyncio.sleep(0.1)
            else:
                raise AssertionError(
                    "WebSocket stream did not publish every signature used by "
                    f"the witness: expected={expected_records}, "
                    f"observed={observed_records}"
                )

            unique_positions = set()
            for event in matching:
                if event.get("type") != "exportSignatureReceived":
                    raise AssertionError(f"Unexpected Export stream event: {event}")
                if event.get("version") != 1:
                    raise AssertionError(f"Unexpected Export share version: {event}")
                if event.get("owner") != alice.address:
                    raise AssertionError(f"Export stream owner mismatch: {event}")
                if int(event.get("origin_ledger_seq", 0)) != origin_seq:
                    raise AssertionError(f"Export stream origin sequence mismatch: {event}")
                if event.get("origin_ledger_hash") != origin_hash:
                    raise AssertionError(f"Export stream origin hash mismatch: {event}")
                if event.get("trigger_txid") != origin:
                    raise AssertionError(f"Export stream trigger mismatch: {event}")
                position = int(event.get("universe_position", -1))
                if position not in selected:
                    raise AssertionError(f"Unselected validator streamed a share: {event}")
                unique_positions.add(position)

            if len(unique_positions) < quorum:
                raise AssertionError(
                    f"Stream exposed only {len(unique_positions)} selected positions; "
                    f"need quorum {quorum}"
                )
            witness_seq = int(witness["LedgerSequence"])
            if not any(
                int(event.get("ledger_index", 0)) < witness_seq
                for event in matching
            ):
                raise AssertionError(
                    "No Export share stream event preceded the ledger witness"
                )
            log(
                f"WebSocket exposed {len(unique_positions)} selected shares; "
                f"witness used {len(expected_records)}"
            )
        finally:
            reader.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await reader

    log("PASS")
