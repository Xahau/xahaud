""":descr: all 5 nodes healthy, every ledger has valid unique quorum-met entropy"""

from __future__ import annotations

from helpers import require_entropy, get_entropy_tx, assert_valid_entropy


async def scenario(ctx, log):
    await require_entropy(ctx, log)

    # Wait for the RNG pipeline to warm up past initial proposal/sidecar gossip.
    await ctx.wait_for_ledgers(3, node_id=0, timeout=60)
    log("Pipeline warmed up")

    start_seq = ctx.validated_ledger_index(0)
    await ctx.wait_for_ledgers(10, node_id=0, timeout=120)
    end_seq = ctx.validated_ledger_index(0)
    log(f"Inspecting ledgers {start_seq + 1} → {end_seq}")

    digests = set()
    for seq in range(start_seq + 1, end_seq + 1):
        ce, _ = get_entropy_tx(ctx, seq)
        digest, count = assert_valid_entropy(ce, seq, seen_digests=digests)
        log(f"  Ledger {seq}: EntropyCount={count} Digest={digest[:16]}...")

    log(f"Verified {end_seq - start_seq} ledgers: all quorum entropy, all unique")
    log("PASS")
