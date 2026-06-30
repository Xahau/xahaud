""":descr: Drop n4->n0 proposals after setup and assert n0 uses sidecar fetch.

The x-testnet default topology is a full mesh, so a one-edge proposal drop does
not prove n0 missed every copy of n4's proposal-carried material: n1/n2/n3 may
relay around the cut. This scenario is therefore a fetch-path exercise and
export-convergence test, not a strict proof that fetch recovered a contributor
that proposal relay could not deliver. For positive CE recovery, see
entropy_fetch_recovers_dropped_claims.

This is the resilience counterpart to export_degradation:
  - export_degradation: n3/n4 do NOT sign (no_export_sig) -> only 3/5 sigs exist
    -> export EXPIRES (tecEXPORT_EXPIRED). The sigs genuinely don't exist.
  - this test: n4 DOES sign, but its proposals to n0 are 100% dropped after
    account setup. n0 stays above quorum from direct proposals and obtains n4's
    contribution through the sidecar fetch/acquisition path. With fetch working,
    n0 reconstructs the agreed set and the export SUCCEEDS.

So this is a regression test for sidecar acquisition on lossy proposal links:
the fetch path must run on n0, and export must still converge. It is deliberately
weaker than a topology-isolated proof that n0 missed a specific contributor.

Flow:
  1. Fund alice and bob
  2. Enable runtime_config drop for n4->n0 proposals
  3. alice submits ttEXPORT (all 5 nodes sign; n0 loses direct n4 proposals)
  4. n0 fetches agreed sidecars (assert SIDECARFETCH triggered + merged on n0)
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
        "n4->n0 proposals are now 100% dropped (msg=proposal): in the full "
        "mesh this exercises sidecar acquisition without proving all relayed "
        "copies of n4 material were suppressed"
    )

    # --- Submit ttEXPORT (all nodes sign; n0 only misses n4 inline) ---
    fetch_start = ctx.mark("export-fetch-drop-start")
    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + 5,
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
                "LastLedgerSequence": current_seq + 5,
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

    # The export signatures exist (all 5 nodes sign). With acquisition working,
    # n0 can reconstruct the agreed set even while direct n4->n0 proposals are
    # dropped.
    if engine_result != "tesSUCCESS":
        raise AssertionError(
            f"Expected tesSUCCESS via sidecar fetch, got {engine_result} -- the "
            "fetch path failed to deliver n4 sidecar contributions to n0 "
            "(or the proposal drop starved consensus on n0)"
        )
    log("Export succeeded despite dropped direct n4->n0 proposals")

    # Prove the SIDECAR FETCH path actually ran on n0. In a full mesh,
    # entriesMerged proves the acquired set was replayed through the merge path;
    # it does not prove local state gained a previously unseen contributor.
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
        "PASS -- proposal drop exercised node 0 sidecar acquisition and the "
        "export still converged"
    )
