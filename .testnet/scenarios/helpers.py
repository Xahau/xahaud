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
    """Return (digest, entropy_count, is_fallback) from a ConsensusEntropy tx.

    consensus_fallback rounds carry a deterministic non-zero consensus-bound
    digest with EntropyCount=0 and EntropyTier=1 (consensus_fallback).
    Validator entropy has EntropyTier=3 (validator_quorum).

    WARNING: is_fallback is ``tier != 3``, so it lumps participant_aligned
    (Tier 2) in with fallback. It is only safe where no Tier 2 band exists
    (e.g. 5-node networks, where tier2 == quorum). For band-aware scenarios use
    the explicit assert_consensus_fallback / assert_participant_aligned /
    assert_validator_quorum helpers, which check EntropyTier directly.
    """
    digest = ce_tx.get("Digest", "")
    entropy_count = ce_tx.get("EntropyCount", -1)
    tier = ce_tx.get("EntropyTier", None)
    if tier is not None:
        is_fallback = tier != 3
    else:
        is_fallback = entropy_count == 0
    return digest, entropy_count, is_fallback


def assert_participant_aligned(ce_tx, seq, expected_count=None):
    """Assert participant_aligned (Tier 2) entropy on a ConsensusEntropy tx.

    Tier 2 is the sub-quorum band: the agreed reveal cohort is >= the
    participant floor but < the 80% validator quorum, so it carries
    EntropyTier=2 with a deterministic non-zero digest. NOTE entropy_fields()'s
    is_fallback lumps tier 2 in with fallback (is_fallback = tier != 3), so the
    tier must be checked EXPLICITLY here.
    """
    digest = ce_tx.get("Digest", "")
    count = ce_tx.get("EntropyCount", -1)
    tier = ce_tx.get("EntropyTier", None)
    if tier != 2:
        raise AssertionError(
            f"Ledger {seq}: expected EntropyTier==2 (participant_aligned), "
            f"got {tier} (EntropyCount={count})"
        )
    if not digest or digest == ZERO_DIGEST:
        raise AssertionError(
            f"Ledger {seq}: participant_aligned digest must be non-zero, got "
            f"{digest[:16]}..."
        )
    if expected_count is not None and count != expected_count:
        raise AssertionError(
            f"Ledger {seq}: participant_aligned EntropyCount must be "
            f"{expected_count} (the surviving cohort), got {count}"
        )
    return digest, count


def assert_validator_quorum(ce_tx, seq, min_count=None):
    """Assert validator_quorum (Tier 3) entropy on a ConsensusEntropy tx:
    EntropyTier=3, a deterministic non-zero digest, and (optionally)
    EntropyCount >= min_count (the active quorum). The count can EXCEED the
    quorum (e.g. a still-full 6/6 ledger caught at a 6->5 transition), so check
    >=, not ==.
    """
    digest = ce_tx.get("Digest", "")
    count = ce_tx.get("EntropyCount", -1)
    tier = ce_tx.get("EntropyTier", None)
    if tier != 3:
        raise AssertionError(
            f"Ledger {seq}: expected EntropyTier==3 (validator_quorum), got "
            f"{tier} (EntropyCount={count})"
        )
    if not digest or digest == ZERO_DIGEST:
        raise AssertionError(
            f"Ledger {seq}: validator_quorum digest must be non-zero, got "
            f"{digest[:16]}..."
        )
    if min_count is not None and count < min_count:
        raise AssertionError(
            f"Ledger {seq}: validator_quorum EntropyCount={count} < quorum "
            f"{min_count}"
        )
    return digest, count


def assert_consensus_fallback(ce_tx, seq):
    """Assert consensus_fallback (Tier 1) entropy on a ConsensusEntropy tx:
    EntropyTier=1, EntropyCount=0, and a deterministic NON-zero digest.
    """
    digest = ce_tx.get("Digest", "")
    count = ce_tx.get("EntropyCount", -1)
    tier = ce_tx.get("EntropyTier", None)
    if tier != 1:
        raise AssertionError(
            f"Ledger {seq}: expected EntropyTier==1 (consensus_fallback), got "
            f"{tier} (EntropyCount={count})"
        )
    if count != 0:
        raise AssertionError(
            f"Ledger {seq}: consensus_fallback EntropyCount must be 0, got "
            f"{count}"
        )
    if not digest or digest == ZERO_DIGEST:
        raise AssertionError(
            f"Ledger {seq}: consensus_fallback digest must be non-zero, got "
            f"{digest[:16]}..."
        )
    return digest, count


def assert_valid_entropy(ce_tx, seq, seen_digests=None):
    """Assert quorum-met validator entropy. Optionally check uniqueness."""
    digest, entropy_count, is_fallback = entropy_fields(ce_tx)

    if is_fallback or not digest or digest == ZERO_DIGEST:
        raise AssertionError(f"Ledger {seq}: fallback/empty Digest")

    if entropy_count < 4:
        raise AssertionError(
            f"Ledger {seq}: EntropyCount={entropy_count} < 4 (sub-quorum)"
        )

    if seen_digests is not None:
        if digest in seen_digests:
            raise AssertionError(f"Ledger {seq}: duplicate Digest {digest[:16]}...")
        seen_digests.add(digest)

    return digest, entropy_count
