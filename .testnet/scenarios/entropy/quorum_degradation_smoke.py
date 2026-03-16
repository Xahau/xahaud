""":descr: 4/5 liveness, 3/5 stall, zero entropy (bootstrap skip), recovery"""

from __future__ import annotations

from helpers import require_entropy, get_entropy_tx, entropy_fields


async def scenario(ctx, log):
    await require_entropy(ctx, log)

    # Baseline: wait 1 ledger to confirm network is healthy.
    await ctx.wait_for_ledgers(1, node_id=0, timeout=30)

    # --- 4/5 liveness ---
    ctx.stop_node(4)
    await ctx.wait_for_nodes_down(nodes=[4], timeout=30)
    await ctx.wait_for_ledgers(1, node_id=0, timeout=30)
    log("4/5: liveness OK")

    # Snapshot validated seq before dropping to 3/5.
    val_before = ctx.validated_ledger_index(0)

    # --- 3/5 validation stall ---
    ctx.stop_node(3)
    await ctx.wait_for_nodes_down(nodes=[3], timeout=30)

    # 10s ≈ 3 rounds at 3s cadence — enough to validate if quorum existed.
    await ctx.sleep(10)

    val_after = ctx.validated_ledger_index(0)
    log(f"3/5: validated ledger {val_before} → {val_after}")

    if val_after and val_before and val_after > val_before:
        raise AssertionError(
            f"Validated ledger advanced ({val_before}→{val_after}) "
            f"with only 3/5 validators"
        )

    # Log checks tied to actual transition mechanics:
    # - seq=1 proposals are emitted once commit-set phase is entered
    # - ConvergingCommit transition is the gateway out of seq=0-only behavior
    # - establish gate blocked indicates tx-consensus/pause prevented accept
    ctx.log_level("LedgerConsensus", "trace")
    op = await ctx.sleep(6, name="stall_window")

    ctx.assert_not_log(
        r"RNG: transitioned to ConvergingCommit", within=op.window, nodes=[0, 1, 2]
    )
    ctx.assert_not_log(r"RNG: propose seq=1", within=op.window, nodes=[0, 1, 2])

    gate_blocked = ctx.search_logs(
        r"STALLDIAG: establish gate blocked reason=(pause|no-tx-consensus)",
        within=op.window,
        nodes=[0, 1, 2],
    )
    log(f"3/5: establish gate-blocked logs in 6s: {gate_blocked.count}")

    skips = ctx.search_logs(r"RNG: bootstrap skip", within=op.window, nodes=[0, 1, 2])
    log(f"3/5: RNG bootstrap skips in 6s: {skips.count}")

    # --- Recovery: restart nodes, verify ledger advancement ---
    ctx.start_node(3)
    ctx.start_node(4)
    await ctx.wait_for_ledgers(1, node_id=0, timeout=120)

    val_recovered = ctx.validated_ledger_index(0)
    log(f"Recovered: validated seq {val_before} → {val_recovered}")

    if not val_recovered or not val_before or val_recovered <= val_before:
        raise AssertionError(
            f"Validated ledger did not advance after recovery "
            f"({val_before} → {val_recovered})"
        )

    # Inspect recovery ledgers: during the stall, entropy falls to zero
    # (bootstrap skip). The first ledger after val_before may still carry
    # real entropy from an in-flight round before node 3 went down.
    zero_count = 0
    nonzero_count = 0
    for seq in range(val_before + 1, val_recovered + 1):
        ce, _ = get_entropy_tx(ctx, seq)
        digest, entropy_count, is_zero = entropy_fields(ce)

        if is_zero:
            zero_count += 1
        else:
            nonzero_count += 1
            if entropy_count < 4:
                raise AssertionError(
                    f"Ledger {seq}: non-zero entropy with sub-quorum "
                    f"EntropyCount={entropy_count} (need >= 4)"
                )

        log(
            f"  Ledger {seq}: EntropyCount={entropy_count} "
            f"{'ZERO' if is_zero else 'REAL'}"
        )

    log(f"Entropy summary: {zero_count} zero, {nonzero_count} non-zero")

    # With quorum-gated entropy, sub-quorum reveals produce zero entropy.
    # Allow at most 1 non-zero in case node 3 completes the full pipeline
    # (commit + reveal) before dying — that's a valid quorum-met round.
    if nonzero_count > 1:
        raise AssertionError(
            f"Expected at most 1 non-zero entropy ledger during stall, "
            f"got {nonzero_count}"
        )

    log("PASS")
