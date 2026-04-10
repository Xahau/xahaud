""":descr: 4/5 liveness, 3/5 zero-entropy fallback, recovery"""

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

    # --- 3/5 degraded window ---
    ctx.stop_node(3)
    await ctx.wait_for_nodes_down(nodes=[3], timeout=30)

    # 10s ≈ 3 rounds at 3s cadence.
    await ctx.sleep(10)

    val_after = ctx.validated_ledger_index(0)
    log(f"3/5: validated ledger {val_before} → {val_after}")

    # Accepted/built ledgers may still later appear as validated once the full
    # network rejoins. For ConsensusEntropy the key invariant is that every
    # ledger created during this sub-quorum window carries ZERO entropy.
    degraded_zero = 0
    degraded_end = val_after or val_before
    if val_before and degraded_end and degraded_end > val_before:
        for seq in range(val_before + 1, degraded_end + 1):
            ce, _ = get_entropy_tx(ctx, seq)
            digest, entropy_count, is_zero = entropy_fields(ce)

            if not is_zero:
                raise AssertionError(
                    f"Ledger {seq}: expected ZERO entropy during 3/5 window, "
                    f"got Digest={digest[:16]}... EntropyCount={entropy_count}"
                )

            degraded_zero += 1
            log(f"  Degraded ledger {seq}: EntropyCount={entropy_count} ZERO")

    log(f"3/5 entropy summary: {degraded_zero} zero")

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
    pre_recovery = max(v for v in [val_before, val_after] if v is not None)
    log(f"Recovered: validated seq {pre_recovery} → {val_recovered}")

    if not val_recovered or val_recovered <= pre_recovery:
        raise AssertionError(
            f"Validated ledger did not advance after recovery "
            f"({pre_recovery} → {val_recovered})"
        )

    # Inspect post-recovery ledgers separately from the degraded window above.
    # Once the network is back at quorum, non-zero entropy is valid again but
    # must still be quorum-met.
    zero_count = 0
    nonzero_count = 0
    for seq in range(pre_recovery + 1, val_recovered + 1):
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

    log("PASS")
