"""Shared helpers for ConsensusEntropy scenario tests."""

from __future__ import annotations

from xahaud_scripts.testnet.config import feature_name_to_hash

ZERO_DIGEST = "0" * 64
CONSENSUS_ENTROPY_FEATURE = feature_name_to_hash("ConsensusEntropy")


def feature_hash(name: str) -> str:
    """Return the amendment hash accepted by feature RPC."""
    return feature_name_to_hash(name)


def feature_status(ctx, name: str, node_id=0):
    """Query a feature by amendment hash; feature RPC names are ambiguous."""
    return ctx.feature_check(feature_hash(name), node_id=node_id)


def consensus_entropy_feature(ctx, node_id=0):
    """Query ConsensusEntropy by amendment hash."""
    return feature_status(ctx, "ConsensusEntropy", node_id=node_id)


async def require_entropy(ctx, log):
    """Wait for first ledger and assert ConsensusEntropy is enabled."""
    await ctx.wait_for_ledger_close(timeout=120)
    feature = consensus_entropy_feature(ctx, node_id=0)
    if not feature or not feature.get("enabled", False):
        raise AssertionError(f"ConsensusEntropy not enabled: {feature}")
    log("ConsensusEntropy enabled")


def get_entropy_tx(ctx, seq):
    """Fetch ledger and return (ce_tx, user_txns) or raise."""
    result = ctx.ledger(seq, transactions=True)
    if not result:
        raise AssertionError(f"Ledger {seq}: fetch failed")

    ledger = result.get("ledger")
    if not isinstance(ledger, dict):
        raise AssertionError(f"Ledger {seq}: fetch returned no ledger: {result}")

    txns = ledger.get("transactions", [])
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
