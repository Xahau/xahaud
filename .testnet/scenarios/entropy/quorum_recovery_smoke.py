""":descr: drop 2 nodes (3/5 stall), restart both, verify recovery"""

from __future__ import annotations


async def scenario(ctx, log):
    await ctx.wait_for_ledger_close(timeout=120)

    feature = ctx.feature_check("ConsensusEntropy", node_id=0)
    if not feature or not feature.get("enabled", False):
        raise AssertionError(f"ConsensusEntropy not enabled: {feature}")

    await ctx.wait_for_ledgers(1, node_id=0, timeout=60)
    log("Baseline OK")

    # Drop 2 nodes → validation stall.
    ctx.stop_node(3)
    ctx.stop_node(4)
    await ctx.wait_for_nodes_down(nodes=[3, 4], timeout=30)

    info = ctx.rpc.server_info(node_id=0)
    val_before = info.get("info", {}).get("validated_ledger", {}).get("seq", 0)
    log(f"Stalled at validated seq {val_before}")

    # Let it sit for a few rounds in degraded state.
    await ctx.sleep(6)

    # Bring both nodes back.
    ctx.start_node(3)
    ctx.start_node(4)
    log("Restarted n3 and n4, waiting for recovery...")

    # Recovery: wait for ANY validated ledger advance on n0.
    await ctx.wait_for_ledger_close(node_id=0, timeout=60)

    info = ctx.rpc.server_info(node_id=0)
    val_after = info.get("info", {}).get("validated_ledger", {}).get("seq", 0)
    log(f"Recovered: validated seq {val_before} → {val_after}")

    if val_after <= val_before:
        raise AssertionError(
            f"Validated ledger did not advance after recovery "
            f"({val_before} → {val_after})"
        )

    log("PASS")
