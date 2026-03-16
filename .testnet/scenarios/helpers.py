"""Shared helpers for ConsensusEntropy scenario tests."""

from __future__ import annotations

ZERO_DIGEST = "0" * 64


async def require_entropy(ctx, log):
    """Wait for first ledger and assert ConsensusEntropy is enabled."""
    await ctx.wait_for_ledger_close(timeout=120)
    feature = ctx.feature_check("ConsensusEntropy", node_id=0)
    if not feature or not feature.get("enabled", False):
        raise AssertionError(f"ConsensusEntropy not enabled: {feature}")
    log("ConsensusEntropy enabled")


def get_entropy_tx(ctx, seq):
    """Fetch ledger and return (ce_tx, user_txns) or raise."""
    result = ctx.ledger(seq, transactions=True)
    if not result:
        raise AssertionError(f"Ledger {seq}: fetch failed")

    txns = result.get("ledger", {}).get("transactions", [])
    ce = [tx for tx in txns if tx.get("TransactionType") == "ConsensusEntropy"]
    user = [tx for tx in txns if tx.get("TransactionType") != "ConsensusEntropy"]

    if len(ce) != 1:
        raise AssertionError(
            f"Ledger {seq}: expected 1 ConsensusEntropy txn, got {len(ce)}"
        )

    return ce[0], user


def entropy_fields(ce_tx):
    """Return (digest, entropy_count, is_zero) from a ConsensusEntropy tx."""
    digest = ce_tx.get("Digest", "")
    entropy_count = ce_tx.get("EntropyCount", -1)
    is_zero = digest == ZERO_DIGEST and entropy_count == 0
    return digest, entropy_count, is_zero


def assert_valid_entropy(ce_tx, seq, seen_digests=None):
    """Assert non-zero quorum-met entropy. Optionally check uniqueness."""
    digest, entropy_count, is_zero = entropy_fields(ce_tx)

    if is_zero or not digest:
        raise AssertionError(f"Ledger {seq}: zero/empty Digest")

    if entropy_count < 4:
        raise AssertionError(
            f"Ledger {seq}: EntropyCount={entropy_count} < 4 (sub-quorum)"
        )

    if seen_digests is not None:
        if digest in seen_digests:
            raise AssertionError(f"Ledger {seq}: duplicate Digest {digest[:16]}...")
        seen_digests.add(digest)

    return digest, entropy_count
