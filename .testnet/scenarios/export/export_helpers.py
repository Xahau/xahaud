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


def assert_hook_accepted(meta, log, *, expected_emits=1):
    """Assert hook executed with ACCEPT and the expected emit count.

    Checks sfHookExecutions in transaction metadata.
    Returns the hook execution entry for further inspection.
    """
    hook_execs = meta.get("HookExecutions", [])
    if not hook_execs:
        raise AssertionError("No HookExecutions in metadata")

    exec_entry = hook_execs[0].get("HookExecution", {})
    hook_result = exec_entry.get("HookResult", -1)
    emit_count = exec_entry.get("HookEmitCount", -1)
    return_code = exec_entry.get("HookReturnCode", "")

    log(f"  HookResult={hook_result} EmitCount={emit_count} ReturnCode={return_code}")

    # HookResult 3 = ExitType::ACCEPT
    if hook_result != 3:
        raise AssertionError(
            f"Hook did not ACCEPT: HookResult={hook_result} "
            f"ReturnCode={return_code}"
        )

    if emit_count != expected_emits:
        raise AssertionError(
            f"Expected {expected_emits} emits, got {emit_count}"
        )

    # ReturnCode 0 = success; non-zero = ASSERT line number in hook
    if return_code and str(return_code) != "0":
        raise AssertionError(
            f"Hook returned error code {return_code} "
            f"(likely ASSERT failure at that line)"
        )

    return exec_entry


def assert_export_result(meta, log, *, require_signers=True):
    """Assert ExportResult is present and well-formed in metadata.

    Returns the ExportResult dict.
    """
    export_result = meta.get("ExportResult", {})
    if not export_result:
        raise AssertionError("ExportResult not found in metadata")

    # Must have LedgerSequence and TransactionHash
    if "LedgerSequence" not in export_result:
        raise AssertionError("ExportResult missing LedgerSequence")
    if "TransactionHash" not in export_result:
        raise AssertionError("ExportResult missing TransactionHash")

    # Must have the inner ExportedTxn object
    inner = export_result.get("ExportedTxn", {})
    if not inner:
        raise AssertionError("ExportResult missing ExportedTxn (multisigned blob)")

    log(f"  ExportResult: seq={export_result['LedgerSequence']} "
        f"hash={export_result['TransactionHash'][:16]}...")

    # Inner tx should have Account, Destination, TransactionType
    if "Account" not in inner:
        raise AssertionError("ExportedTxn missing Account")
    if "TransactionType" not in inner:
        raise AssertionError("ExportedTxn missing TransactionType")

    # Should have empty SigningPubKey (multisigned)
    if inner.get("SigningPubKey", "NOT_EMPTY") != "":
        raise AssertionError(
            f"ExportedTxn SigningPubKey should be empty, "
            f"got '{inner.get('SigningPubKey')}'"
        )

    if require_signers:
        signers = inner.get("Signers", [])
        if not signers:
            raise AssertionError("ExportedTxn has no Signers (multisig not applied)")
        log(f"  Signers: {len(signers)} validator(s)")

    return export_result


def assert_shadow_ticket(ctx, account_address, log, *, expect_exists=True):
    """Assert shadow ticket exists (or doesn't) for the account."""
    obj_result = ctx.rpc.request(
        0, "account_objects", {"account": account_address}
    )
    all_objects = (obj_result or {}).get("account_objects", [])
    shadow_tickets = [
        obj for obj in all_objects
        if obj.get("LedgerEntryType") == "ShadowTicket"
    ]
    log(f"  Shadow tickets: {len(shadow_tickets)}")

    if expect_exists and not shadow_tickets:
        raise AssertionError("Expected shadow ticket but none found")
    if not expect_exists and shadow_tickets:
        raise AssertionError(
            f"Expected no shadow tickets but found {len(shadow_tickets)}"
        )

    return shadow_tickets
