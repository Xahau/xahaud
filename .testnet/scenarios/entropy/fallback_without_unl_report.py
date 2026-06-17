""":descr: healthy non-standalone testnet without UNLReport mints Tier 1 fallback"""

from __future__ import annotations

from helpers import require_entropy, get_entropy_tx, assert_consensus_fallback


async def scenario(ctx, log):
    await require_entropy(ctx, log)

    # Non-standalone nodes require a ledger-anchored UNLReport before assigning
    # validator_quorum / participant_aligned labels. Without it, the RNG pipeline
    # may still collect commits/reveals, but injection must remain Tier 1.
    await ctx.wait_for_ledgers(3, node_id=0, timeout=60)
    log("Pipeline warmed up without UNLReport")

    start_seq = ctx.validated_ledger_index(0)
    await ctx.wait_for_ledgers(5, node_id=0, timeout=90)
    end_seq = ctx.validated_ledger_index(0)
    log(f"Inspecting ledgers {start_seq + 1} -> {end_seq}")

    for seq in range(start_seq + 1, end_seq + 1):
        ce, _ = get_entropy_tx(ctx, seq)
        digest, count = assert_consensus_fallback(ce, seq)
        log(f"  Ledger {seq}: EntropyCount={count} Digest={digest[:16]}...")

    log(f"Verified {end_seq - start_seq} ledgers: all consensus_fallback")
    log("PASS")
