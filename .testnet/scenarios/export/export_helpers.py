"""Shared helpers for Export scenario tests."""

from __future__ import annotations


async def require_export(ctx, log):
    """Wait for first ledger and assert Export amendment is enabled."""
    await ctx.wait_for_ledger_close(timeout=120)
    feature = ctx.feature_check("Export", node_id=0)
    if not feature or not feature.get("enabled", False):
        raise AssertionError(f"Export not enabled: {feature}")
    log("Export amendment enabled")


def find_export_txns(ctx, seq):
    """Find Export transactions in a ledger.

    Returns list of Export transaction dicts.
    """
    result = ctx.ledger(seq, transactions=True)
    if not result:
        return []

    txns = result.get("ledger", {}).get("transactions", [])
    return [tx for tx in txns if tx.get("TransactionType") == "Export"]


def dst_param(address):
    """Encode an address as a HookParameter entry for the DST param."""
    from xrpl.core.addresscodec import decode_classic_address

    dst_hex = decode_classic_address(address).hex().upper()
    return {
        "HookParameter": {
            "HookParameterName": "445354",  # "DST"
            "HookParameterValue": dst_hex,
        }
    }
