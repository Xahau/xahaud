""":descr: n0 ignores proposal-carried CE claims; sidecar fetch must recover them

This is the positive-control counterpart to the healthy steady-state fetch
measurement. RuntimeConfig `rng_reveal_drop_pct=100` makes node 0 still harvest
proposal-carried commits, but ignore proposal-carried reveals. That creates a
clean "hash visible, reveal payload missing" case without relying on a
particular relay topology.

The test proves two things on n0:
  1. fetch/acquisition ran; and
  2. acquired entropy sidecars added new reveal material (`pendingDelta`).

The second check is the load-bearing one: `entriesMerged` alone only proves that
leaves were replayed through the merge loop, not that local consensus state was
improved.
"""

from __future__ import annotations

from helpers import require_entropy, get_entropy_tx, assert_valid_entropy


async def scenario(ctx, log):
    await require_entropy(ctx, log)

    cfg = ctx.rpc.runtime_config(0)
    log(f"n0 runtime_config: {cfg}")

    start = ctx.mark("ce-fetch-recovers-start")
    await ctx.wait_for_ledgers(6, node_id=0, timeout=120)
    end = ctx.mark("ce-fetch-recovers-end")

    end_seq = ctx.validated_ledger_index(0)
    start_seq = end_seq - 4
    log(f"Inspecting ledgers {start_seq} -> {end_seq}")

    digests = set()
    for seq in range(start_seq, end_seq + 1):
        ce, _ = get_entropy_tx(ctx, seq)
        digest, count = assert_valid_entropy(ce, seq, seen_digests=digests)
        if count != 5:
            raise AssertionError(
                f"Ledger {seq}: expected full-count EntropyCount=5 after "
                f"fetch recovery, got {count}"
            )
        log(f"  Ledger {seq}: EntropyCount={count} Digest={digest[:16]}...")

    drops = ctx.assert_log(
        r"RNG: TESTING dropping reveal claim",
        since=start,
        until=end,
        nodes=[0],
        min_count=1,
    )
    log(f"n0 dropped proposal-carried CE reveal claims: {drops.count}")

    triggers = ctx.assert_log(
        r"SIDECARFETCH: triggering network fetch.*kind=entropySet",
        since=start,
        until=end,
        nodes=[0],
        min_count=1,
    )
    log(f"n0 entropySet network-fetch triggers: {triggers.count}")

    yielded = ctx.assert_log(
        r"SIDECARFETCH: merged acquired set.*setKind=entropySet.*"
        r"pendingDelta=[1-9]",
        since=start,
        until=end,
        nodes=[0],
        min_count=1,
    )
    log(f"n0 acquired entropySet sidecars with positive reveal yield: {yielded.count}")

    log("PASS -- sidecar fetch recovered CE material hidden from proposals")
