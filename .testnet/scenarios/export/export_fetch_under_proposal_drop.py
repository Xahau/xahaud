""":descr: Drop n4->n0 proposals after setup so node 0 misses n4's inline
sidecar leaves; node 0 must FETCH the agreed commit/reveal/export-signature
sidecars (whose hash it learns from n1/n2/n3 proposals) via the acquisition path
to reconstruct and validate the export. Asserts the sidecar FETCH path actually
carries the load on n0 (SIDECARFETCH logs) and the export still converges.

This is the resilience counterpart to export_degradation:
  - export_degradation: n3/n4 do NOT sign (no_export_sig) -> only 3/5 sigs exist
    -> export EXPIRES (tecEXPORT_EXPIRED). The sigs genuinely don't exist.
  - this test: n4 DOES sign, but its proposals to n0 are 100% dropped after
    account setup. n0 stays above quorum from direct proposals and obtains n4's
    contribution through the sidecar fetch/acquisition path. With fetch working,
    n0 reconstructs the agreed set and the export SUCCEEDS.

So this is the empirical proof that the sidecar fetch path buys real resilience:
a node on lossy proposal links still reconstructs the agreed sidecars and stays
in consensus, turning a would-be-missing-data round into a validated export.

Flow:
  1. Fund alice and bob
  2. Enable runtime_config drop for n4->n0 proposals
  3. alice submits ttEXPORT (all 5 nodes sign; n0 misses n4 inline leaves)
  4. n0 fetches the agreed sidecars (assert SIDECARFETCH triggered + merged on n0)
  5. Export succeeds (tesSUCCESS) and a shadow ticket latch is created
"""

from __future__ import annotations

import json

from export_helpers import require_export, assert_shadow_ticket


def _peer_key_for_node(ctx, node_id: int) -> str:
    network_file = ctx.base_dir / "network.json"
    with network_file.open() as f:
        network = json.load(f)

    for node in network["nodes"]:
        if node["id"] == node_id:
            return f"peer:127.0.0.1:{node['port_peer']}"

    raise AssertionError(f"node {node_id} missing from {network_file}")


def _drop_proposals_from_n4_to_n0(ctx, log) -> None:
    n0_peer_key = _peer_key_for_node(ctx, 0)
    params = {
        "set": {
            n0_peer_key: {
                "send_drop_pct": 100.0,
                "message_types": ["proposal"],
            }
        }
    }

    result = ctx.rpc.runtime_config(4, params)
    if not result or result.get("error"):
        raise AssertionError(f"Failed to configure n4->n0 proposal drop: {result}")

    applied = ctx.rpc.runtime_config(4)
    log(f"Applied n4 runtime_config drop to {n0_peer_key}: {applied}")


async def scenario(ctx, log):
    await require_export(ctx, log)

    # --- Setup ---
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    log("Accounts funded")

    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)
    log(f"Current ledger: {current_seq}")

    #@@start test-export-sidecar-fetch-under-proposal-drop
    _drop_proposals_from_n4_to_n0(ctx, log)
    log(
        "n4->n0 proposals are now 100% dropped (msg=proposal): node 0 must "
        "reconstruct n4 sidecar leaves via the fetch/acquisition path"
    )

    # --- Submit ttEXPORT (all nodes sign; n0 only misses n4 inline) ---
    fetch_start = ctx.mark("export-fetch-drop-start")
    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + 12,
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
                "LastLedgerSequence": current_seq + 10,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=90,
    )
    fetch_end = ctx.mark("export-fetch-drop-end")

    engine_result = result.get("engine_result", "")
    final_seq = ctx.validated_ledger_index(0)
    log(f"Export completed at ledger {final_seq}, result: {engine_result}")

    # The export signatures exist (all 5 nodes sign); n0 obtains n4's
    # contributions ONLY via fetch. With the fetch path working, n0 reconstructs
    # the agreed set, quorum is reached, and the export succeeds.
    if engine_result != "tesSUCCESS":
        raise AssertionError(
            f"Expected tesSUCCESS via sidecar fetch, got {engine_result} -- the "
            "fetch path failed to deliver n4 sidecar contributions to n0 "
            "(or the proposal drop starved consensus on n0)"
        )
    log("Export succeeded despite dropped n4 proposals -- fetch carried it")

    # Prove the SIDECAR FETCH path actually ran on n0 (not that consensus merely
    # converged some other way). assert_log raises if the pattern is absent on
    # node 0, so these ARE the load-bearing assertions of this test.
    triggered = ctx.assert_log(
        r"SIDECARFETCH: triggering network fetch",
        since=fetch_start,
        until=fetch_end,
        nodes=[0],
        min_count=1,
    )
    log(f"n0 SIDECARFETCH network-fetch triggers: {triggered.count}")

    merged = ctx.assert_log(
        r"SIDECARFETCH: merged acquired set.*entriesMerged=[1-9]",
        since=fetch_start,
        until=fetch_end,
        nodes=[0],
        min_count=1,
    )
    log(f"n0 SIDECARFETCH merged-with-entries: {merged.count}")

    # Export completed -> a shadow-ticket callback latch exists for alice
    # (contrast: export_degradation asserts expect_exists=False on expiry).
    assert_shadow_ticket(ctx, alice.address, log, expect_exists=True)
    #@@end test-export-sidecar-fetch-under-proposal-drop

    log(
        "PASS -- proposal drop forced node 0 onto the sidecar fetch path and "
        "the export still converged (fetch resilience demonstrated)"
    )
