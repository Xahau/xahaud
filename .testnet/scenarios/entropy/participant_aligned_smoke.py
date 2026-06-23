""":descr: 5/6 validator_quorum, 4/6 participant_aligned (tier 2), recovery

Requires node_count: 6 (see suite.yml) — the smallest NON-degenerate Tier 2
size. At n=6: tier2 floor = 4, validator quorum = 5, validation quorum = 5. So
  6/6, 5/6 present -> validator_quorum  (EntropyTier=3)
  4/6 present      -> participant_aligned (EntropyTier=2, count 4)   <-- the band
  3/6 present      -> consensus_fallback (EntropyTier=1)
n=5 has NO tier-2 band (tier2 == quorum == 4), which is why the existing
degradation smoke at 5 nodes only ever sees tier 3 / fallback.

KEY: the 4/6 window is BELOW the 80% validation quorum (5). The 4 survivors
keep CLOSING ledgers that carry tier-2 entropy, but those ledgers do NOT
validate until the network recovers — exactly the transition window Tier 2
serves. So validated_ledger_index() stalls; we instead inspect a surviving
node's CLOSED ledger (its LCL) directly, and cross-check the injection from the
cohort's logs.
"""

from __future__ import annotations

from helpers import (
    require_entropy,
    get_entropy_tx,
    assert_participant_aligned,
    assert_validator_quorum,
)


def _closed_entropy(result):
    """(seq, ConsensusEntropy tx) from a ctx.ledger('closed', transactions=True)
    result, or (None, None) if the fetch returned no usable ledger.

    Enforces the per-ledger invariant that an entropy-enabled closed ledger
    carries EXACTLY ONE ConsensusEntropy pseudo-tx (mirroring get_entropy_tx):
    a duplicate or missing injection raises here with a clear error instead of
    being silently skipped and resurfacing later as a generic 'no tier-2 ledger'.
    """
    if not result or not isinstance(result.get("ledger"), dict):
        return None, None
    led = result["ledger"]
    try:
        seq = int(led.get("ledger_index"))
    except (TypeError, ValueError):
        return None, None
    ce = [
        t
        for t in led.get("transactions", [])
        if isinstance(t, dict) and t.get("TransactionType") == "ConsensusEntropy"
    ]
    if len(ce) != 1:
        raise AssertionError(
            f"Closed ledger {seq}: expected 1 ConsensusEntropy txn, got {len(ce)}"
        )
    return seq, ce[0]


async def scenario(ctx, log):
    await require_entropy(ctx, log)

    # Baseline: healthy 6/6 produces validator_quorum entropy.
    await ctx.wait_for_ledgers(1, node_id=0, timeout=30)

    # --- 5/6: settles back to validator_quorum (5 present >= quorum 5) ---
    val_before_drop = ctx.validated_ledger_index(0)
    ctx.stop_node(5)
    await ctx.wait_for_nodes_down(nodes=[5], timeout=30)
    # Settle a few ledgers past the membership change. The ledger right at a
    # validator drop can carry a transient consensus_fallback (tier 1, count 0,
    # deterministic and by design) before the commit/reveal pipeline re-primes,
    # so we do NOT assume any single post-drop ledger is already tier 3.
    await ctx.wait_for_ledgers(4, node_id=0, timeout=90)

    # 5/6 is at/above the 80% quorum (5), so steady state is validator_quorum.
    # Scan the post-drop validated ledgers (all carry the 5-node cohort, so a
    # tier-3 here has count == 5) and require at least one clean validator_quorum
    # — EntropyTier=3, count >= quorum, non-zero digest — tolerating the
    # transition fallback instead of depending on where the tip happened to land.
    val_5of6 = ctx.validated_ledger_index(0)
    t3_seq = None
    for seq in range(val_5of6, val_before_drop, -1):
        ce, _ = get_entropy_tx(ctx, seq)
        tier = ce.get("EntropyTier")
        log(f"  5/6 ledger {seq}: tier={tier} count={ce.get('EntropyCount')}")
        if tier == 3:
            assert_validator_quorum(ce, seq, min_count=5)
            t3_seq = seq
            break
    if t3_seq is None:
        raise AssertionError(
            f"5/6: no validator_quorum (tier 3) entropy in post-drop validated "
            f"ledgers {val_before_drop + 1}..{val_5of6}"
        )
    log(f"5/6: validator_quorum at validated seq {t3_seq}")

    #@@start test-participant-aligned-window
    # --- 4/6: participant_aligned (Tier 2) degraded window ---
    ctx.stop_node(4)
    await ctx.wait_for_nodes_down(nodes=[4], timeout=30)

    # ~12s window: confirm tier-2 INJECTION from the cohort's logs, and that the
    # round is NOT the impossible/fallback path (which is what distinguishes the
    # tier-2 band from the tier-1 fallback regime).
    op = await ctx.sleep(12, name="tier2_window")
    selected_t2 = ctx.search_logs(
        r"RNG: entropy selected seq=\d+ tier=2 count=4",
        within=op.window,
        nodes=[0, 1, 2, 3],
    )
    log(f"4/6: 'entropy selected tier=2 count=4' logs: {selected_t2.count}")
    if selected_t2.count == 0:
        raise AssertionError(
            "4/6 window injected no participant_aligned (tier 2) entropy: no "
            "'RNG: entropy selected ... tier=2 count=4' on the surviving cohort"
        )
    ctx.assert_not_log(
        r"reason=impossible-entropy-gate", within=op.window, nodes=[0, 1, 2, 3]
    )

    # Verify the on-ledger EntropyTier=2 DIRECTLY: validation is stalled (4 < 5),
    # so sample the surviving cohort's CLOSED ledger (its LCL — built but not yet
    # validated). At least one must be participant_aligned with EntropyCount=4.
    tier2_on_ledger = 0
    last_seq = None
    for _ in range(5):
        seq, ce = _closed_entropy(
            ctx.ledger("closed", transactions=True, node_id=0)
        )
        if ce is not None and seq is not None and seq != last_seq:
            last_seq = seq
            tier = ce.get("EntropyTier")
            count = ce.get("EntropyCount", -1)
            log(f"  closed ledger {seq}: tier={tier} count={count}")
            if tier == 2:
                assert_participant_aligned(ce, seq, expected_count=4)
                tier2_on_ledger += 1
        await ctx.sleep(3)

    if tier2_on_ledger == 0:
        raise AssertionError(
            "no closed participant_aligned (tier 2) ledger observed during the "
            "4/6 window (tier 2 was injected per logs, but not seen on a closed "
            "ledger)"
        )
    log(f"4/6: {tier2_on_ledger} participant_aligned closed ledger(s) verified")
    #@@end test-participant-aligned-window

    # --- Recovery: liveness — validation resumes once quorum is restored ---
    ctx.start_node(4)
    ctx.start_node(5)
    await ctx.wait_for_ledgers(1, node_id=0, timeout=120)

    val_recovered = ctx.validated_ledger_index(0)
    if not val_recovered or val_recovered <= val_5of6:
        raise AssertionError(
            f"Validated ledger did not advance after recovery "
            f"({val_5of6} -> {val_recovered})"
        )
    log(f"Recovered: validated seq {val_5of6} -> {val_recovered}")

    log("PASS")
