""":descr: entropy stays valid under transaction load"""

from __future__ import annotations

from helpers import require_entropy, get_entropy_tx, assert_valid_entropy

variants = [
    {"label": "light", "min_txns": 5, "max_txns": 10},
    {"label": "heavy", "min_txns": 50, "max_txns": 60},
    {"label": "super_heavy", "min_txns": 90, "max_txns": 120},
]


async def scenario(ctx, log, *, min_txns=5, max_txns=10, **_):
    await require_entropy(ctx, log)

    gen = ctx.txn_generator(min_txns=min_txns, max_txns=max_txns)
    await gen.start()
    await gen.wait_until_ready()
    log(f"Transaction generator ready ({min_txns}-{max_txns} txns/ledger)")

    # Wait for pipeline warmup + a few txn-bearing ledgers.
    await ctx.wait_for_ledgers(3, node_id=0, timeout=60)

    start_seq = ctx.validated_ledger_index(0)
    await ctx.wait_for_ledgers(10, node_id=0, timeout=120)
    end_seq = ctx.validated_ledger_index(0)
    log(f"Inspecting ledgers {start_seq + 1} → {end_seq}")

    digests = set()
    total_user_txns = 0

    for seq in range(start_seq + 1, end_seq + 1):
        ce, user_txns = get_entropy_tx(ctx, seq)
        digest, count = assert_valid_entropy(ce, seq, seen_digests=digests)
        total_user_txns += len(user_txns)
        log(
            f"  Ledger {seq}: EntropyCount={count} "
            f"user_txns={len(user_txns)} Digest={digest[:16]}..."
        )

    await gen.stop()

    log(
        f"Verified {end_seq - start_seq} ledgers: {total_user_txns} user txns, "
        f"all entropy valid and unique"
    )

    if total_user_txns == 0:
        raise AssertionError("No user transactions were included in any ledger")

    log("PASS")
