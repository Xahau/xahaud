""":descr: 5/6 validator_quorum, 4/6 participant_aligned (tier 2), recovery

Requires node_count: 6 (see suite.yml) — the smallest NON-degenerate Tier 2
size. At n=6: tier2 floor = 4, validator quorum = 5, validation quorum = 5. So
  6/6, 5/6 present -> validator_quorum  (EntropyTier=3)
  4/6 present      -> participant_aligned (EntropyTier=2, count 4)   <-- the band
  3/6 present      -> consensus_fallback (EntropyTier=1)
n=5 has NO tier-2 band (tier2 == quorum == 4), which is why the existing
degradation smoke at 5 nodes only ever sees tier 3 / fallback.

KEY: the 4/6 window is BELOW the 80% validation quorum (5). The 4 survivors
keep building ledgers carrying tier-2 entropy, but those ledgers do NOT validate
until the network recovers — exactly the transition window Tier 2 serves. So we
confirm tier-2 injection from the cohort's LOGS during the window, then verify
the on-ledger EntropyTier=2 POST-RECOVERY, once the provisional ledgers become
canonical and validate (same mechanism the degradation smoke relies on).
"""

from __future__ import annotations

from helpers import require_entropy, get_entropy_tx, assert_participant_aligned


async def scenario(ctx, log):
    await require_entropy(ctx, log)

    # Baseline: healthy 6/6 produces validator_quorum entropy.
    await ctx.wait_for_ledgers(1, node_id=0, timeout=30)

    # --- 5/6: still validator_quorum (5 present >= quorum 5; validates) ---
    ctx.stop_node(5)
    await ctx.wait_for_nodes_down(nodes=[5], timeout=30)
    await ctx.wait_for_ledgers(1, node_id=0, timeout=30)

    val_5of6 = ctx.validated_ledger_index(0)
    ce5, _ = get_entropy_tx(ctx, val_5of6)
    if ce5.get("EntropyTier") != 3:
        raise AssertionError(
            f"5/6 ledger {val_5of6}: expected validator_quorum (tier 3), got "
            f"tier {ce5.get('EntropyTier')} (EntropyCount={ce5.get('EntropyCount')})"
        )
    log(f"5/6: validator_quorum at validated seq {val_5of6}")

    # --- 4/6: participant_aligned (Tier 2) degraded window ---
    # Validation stalls here (4 < 5); the 4 survivors build PROVISIONAL tier-2
    # ledgers. Confirm injection from the cohort's logs now.
    ctx.stop_node(4)
    await ctx.wait_for_nodes_down(nodes=[4], timeout=30)

    # ~12s ≈ 4 rounds at 3s cadence — enough for the transition blip to settle
    # and several steady 4/6 (tier-2) rounds to close.
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

    # 4/6 sits AT the entropy-gate threshold, so the round must NOT take the
    # impossible/fallback path the sub-floor degradation smoke relies on — this
    # is what distinguishes the tier-2 band from the tier-1 fallback regime.
    ctx.assert_not_log(
        r"reason=impossible-entropy-gate", within=op.window, nodes=[0, 1, 2, 3]
    )

    # --- Recovery: restart -> the provisional tier-2 ledgers become canonical
    # and validate, advancing the validated tip past them. ---
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

    # The 4/6 window's ledgers are now validated. At least one must carry
    # participant_aligned (tier 2): EntropyTier=2, EntropyCount=4. The range may
    # also hold a transitional fallback (the count-3 blip) and post-recovery
    # validator ledgers; the invariant is that the band was reached AND recorded
    # on-ledger — never silently upgraded to tier 3 with a sub-quorum cohort.
    tier2_seen = 0
    for seq in range(val_5of6 + 1, val_recovered + 1):
        ce, _ = get_entropy_tx(ctx, seq)
        tier = ce.get("EntropyTier")
        count = ce.get("EntropyCount", -1)
        if tier == 2:
            assert_participant_aligned(ce, seq, expected_count=4)
            tier2_seen += 1
        log(f"  ledger {seq}: tier={tier} count={count}")

    if tier2_seen == 0:
        raise AssertionError(
            f"No participant_aligned (tier 2) ledger in the recovered range "
            f"{val_5of6 + 1}..{val_recovered}; tier 2 was injected (logs) but "
            "not recorded on a validated ledger"
        )
    log(f"4/6 entropy: {tier2_seen} participant_aligned ledger(s) validated")

    log("PASS")
