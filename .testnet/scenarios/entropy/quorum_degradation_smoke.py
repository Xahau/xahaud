""":descr: 4/5 liveness, 3/5 fail-closed sub-quorum window, recovery"""

from __future__ import annotations

from helpers import (
    require_entropy,
    get_entropy_tx,
    entropy_fields,
    assert_consensus_fallback,
)


def _closed_entropy(result):
    """Return (seq, ConsensusEntropy tx) from a closed-ledger RPC result.

    The 3/5 window is below validation quorum, so validated-ledger history is
    expected to stall. Sampling a surviving node's closed ledger catches any
    local LCL that advanced despite the sub-quorum condition.
    """
    if not result or not isinstance(result.get("ledger"), dict):
        return None, None
    ledger = result["ledger"]
    try:
        seq = int(ledger.get("ledger_index"))
    except (TypeError, ValueError):
        return None, None
    ce = [
        tx
        for tx in ledger.get("transactions", [])
        if isinstance(tx, dict) and tx.get("TransactionType") == "ConsensusEntropy"
    ]
    if len(ce) != 1:
        raise AssertionError(
            f"Closed ledger {seq}: expected 1 ConsensusEntropy txn, got {len(ce)}"
        )
    return seq, ce[0]


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

    if val_after and val_before and val_after > val_before:
        raise AssertionError(
            f"3/5 sub-quorum window unexpectedly validated ledgers "
            f"({val_before} -> {val_after})"
        )

    # If the surviving cohort exposes an advanced closed ledger despite being
    # below validation quorum, it must fail closed to consensus_fallback. This
    # keeps the entropy assertion live without pretending validated history
    # should advance at 3/5.
    degraded_fallback = 0
    last_closed = None
    for _ in range(5):
        seq, ce = _closed_entropy(ctx.ledger("closed", transactions=True, node_id=0))
        if seq and val_before and seq > val_before and seq != last_closed:
            last_closed = seq
            digest, count = assert_consensus_fallback(ce, seq)
            degraded_fallback += 1
            log(
                f"  3/5 closed ledger {seq}: EntropyCount={count} "
                f"Digest={digest[:16]}... FALLBACK"
            )
        await ctx.sleep(2)

    log(f"3/5 closed-ledger fallback samples: {degraded_fallback}")

    # Log checks tied to current transition mechanics:
    # - commit-set SHAMap publication is the observable output of entering the
    #   commit sidecar phase
    # - ConvergingCommit transition is the gateway out of seq=0-only behavior
    # - rng-commit-timeout-below-quorum is the degraded-window fallback path
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

    below_quorum = ctx.search_logs(
        r"STALLDIAG: rng-commit-timeout-below-quorum",
        within=op.window,
        nodes=[0, 1, 2],
    )
    log(f"3/5: RNG commit timeout below quorum logs in 6s: {below_quorum.count}")

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
