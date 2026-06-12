""":descr: 4/5 liveness, 3/5 fallback-entropy (consensus_fallback), recovery"""

from __future__ import annotations

from helpers import ZERO_DIGEST, require_entropy, get_entropy_tx, entropy_fields


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
    # ledger created during this sub-quorum window carries FALLBACK entropy
    # (consensus_fallback: non-zero consensus-bound digest, count 0) — never
    # validator-tier entropy.
    degraded_fallback = 0
    degraded_end = val_after or val_before
    if val_before and degraded_end and degraded_end > val_before:
        for seq in range(val_before + 1, degraded_end + 1):
            ce, _ = get_entropy_tx(ctx, seq)
            digest, entropy_count, is_fallback = entropy_fields(ce)
            tier = ce.get("EntropyTier")

            # consensus_fallback (EntropyTier=1): explicit tier, count 0,
            # deterministic NON-zero digest.
            if tier != 1:
                raise AssertionError(
                    f"Ledger {seq}: expected EntropyTier==1 "
                    f"(consensus_fallback) during 3/5 window, got {tier} "
                    f"(EntropyCount={entropy_count})"
                )
            if entropy_count != 0:
                raise AssertionError(
                    f"Ledger {seq}: fallback EntropyCount must be 0, got "
                    f"{entropy_count}"
                )
            if not digest or digest == ZERO_DIGEST:
                raise AssertionError(
                    f"Ledger {seq}: fallback digest must be non-zero "
                    f"(consensus_fallback), got {digest[:16]}..."
                )
            assert is_fallback  # tier==1 implies fallback

            degraded_fallback += 1
            log(
                f"  Degraded ledger {seq}: EntropyCount={entropy_count} "
                f"FALLBACK"
            )

    log(f"3/5 entropy summary: {degraded_fallback} fallback")

    # Log checks tied to current transition mechanics:
    # - commit-set SHAMap publication is the observable output of entering the
    #   commit sidecar phase
    # - ConvergingCommit transition is the gateway out of seq=0-only behavior
    # - reason=impossible-quorum is the explicit degraded-window fallback path
    ctx.log_level("LedgerConsensus", "trace")
    ctx.log_level("ConsensusExtensions", "trace")
    op = await ctx.sleep(6, name="stall_window")

    ctx.assert_not_log(
        r"RNG: transitioned to ConvergingCommit", within=op.window, nodes=[0, 1, 2]
    )
    ctx.assert_not_log(
        r"RNG: built commitSet SHAMap", within=op.window, nodes=[0, 1, 2]
    )

    gate_blocked = ctx.search_logs(
        r"STALLDIAG: establish gate blocked reason=(pause|no-tx-consensus)",
        within=op.window,
        nodes=[0, 1, 2],
    )
    log(f"3/5: establish gate-blocked logs in 6s: {gate_blocked.count}")

    impossible = ctx.search_logs(
        r"RNG: skipping commit wait reason=impossible-quorum",
        within=op.window,
        nodes=[0, 1, 2],
    )
    log(f"3/5: RNG impossible-quorum skips in 6s: {impossible.count}")

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
    # Once the network is back at quorum, validator-tier entropy is expected
    # again (transitional fallback ledgers are fine) and must be quorum-met.
    fallback_count = 0
    validator_count = 0
    for seq in range(pre_recovery + 1, val_recovered + 1):
        ce, _ = get_entropy_tx(ctx, seq)
        digest, entropy_count, is_fallback = entropy_fields(ce)

        if is_fallback:
            fallback_count += 1
        else:
            validator_count += 1
            if entropy_count < 4:
                raise AssertionError(
                    f"Ledger {seq}: validator entropy with sub-quorum "
                    f"EntropyCount={entropy_count} (need >= 4)"
                )

        log(
            f"  Ledger {seq}: EntropyCount={entropy_count} "
            f"{'FALLBACK' if is_fallback else 'VALIDATOR'}"
        )

    log(
        f"Entropy summary: {fallback_count} fallback, "
        f"{validator_count} validator"
    )

    log("PASS")
