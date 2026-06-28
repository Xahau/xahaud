"""Shared helpers for Export scenario tests."""

from __future__ import annotations

from xahaud_scripts.testnet.config import _unl_report_index, feature_name_to_hash


async def require_export(
    ctx, log, *, require_unl_report=True, require_runtime_config=True
):
    """Wait for first ledger and assert Export is enabled.

    Network-mode Export success requires a parent-ledger UNLReport-backed
    active validator view. Most export scenarios seed that report in genesis;
    assert it here so a success-path test cannot accidentally pass setup
    without the condition Export::doApply requires. The no-UNLReport retry
    scenario opts out deliberately.

    The tracked export suite also uses XAHAUD_RUNTIME_TEST_CONFIG for polling
    and fault-injection knobs. Default binaries reject the runtime_config RPC,
    so check it up front rather than silently running without those knobs.
    """
    await ctx.wait_for_ledger_close(timeout=120)

    if require_runtime_config:
        result = ctx.rpc.runtime_config(0)
        if not result or result.get("error"):
            raise AssertionError(
                "Export suite requires a binary built with "
                "xahaud_runtime_test_config=ON; runtime_config RPC returned "
                f"{result}"
            )
        log("RuntimeConfig RPC active")

    feature = ctx.feature_check(feature_name_to_hash("Export"), node_id=0)
    if not feature or not feature.get("enabled", False):
        raise AssertionError(f"Export not enabled: {feature}")
    log("Export enabled")

    if require_unl_report:
        result = ctx.rpc.ledger_entry(0, _unl_report_index())
        node = (result or {}).get("node", {})
        active = node.get("ActiveValidators", [])
        if node.get("LedgerEntryType") != "UNLReport" or not active:
            raise AssertionError(
                "Export success scenario requires a ledger UNLReport with "
                f"ActiveValidators, got: {result}"
            )
        log(f"UNLReport active validators: {len(active)}")


def find_export_txns(ctx, seq):
    """Find Export transactions in a ledger.

    Returns list of Export transaction dicts.
    """
    result = ctx.ledger(seq, transactions=True)
    if not result:
        return []

    txns = result.get("ledger", {}).get("transactions", [])
    return [tx for tx in txns if tx.get("TransactionType") == "Export"]


def find_export_signature_witness(ctx, seq, witness_hash):
    """Find the same-ledger ExportSignatures witness by transaction hash."""
    result = ctx.ledger(seq, transactions=True)
    txns = (result or {}).get("ledger", {}).get("transactions", [])
    for tx in txns:
        if not isinstance(tx, dict):
            continue
        if tx.get("hash") != witness_hash:
            continue
        if tx.get("TransactionType") != "ExportSignatures":
            raise AssertionError(
                f"ExportSignatureHash {witness_hash} resolved to "
                f"{tx.get('TransactionType')}, not ExportSignatures"
            )
        return tx
    raise AssertionError(
        f"ExportSignatures witness {witness_hash} not found in ledger {seq}"
    )


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


def _signer_entries(witness):
    entries = []
    for entry in witness.get("Signers", []):
        signer = entry.get("Signer", entry)
        entries.append(signer)
    return entries


def assert_export_result(meta, log, *, ctx=None, require_signers=True):
    """Assert ExportResult is present and well-formed in metadata.

    Returns the ExportResult dict. When signers are required, the result is
    annotated with _Witness and _WitnessSigners from the same-ledger
    ttEXPORT_SIGNATURES pseudo transaction.
    """
    export_result = meta.get("ExportResult", {})
    if not export_result:
        raise AssertionError("ExportResult not found in metadata")

    # Must have LedgerSequence and TransactionHash
    if "LedgerSequence" not in export_result:
        raise AssertionError("ExportResult missing LedgerSequence")
    if "TransactionHash" not in export_result:
        raise AssertionError("ExportResult missing TransactionHash")
    if "ExportSignatureHash" not in export_result:
        raise AssertionError("ExportResult missing ExportSignatureHash")

    log(f"  ExportResult: seq={export_result['LedgerSequence']} "
        f"hash={export_result['TransactionHash'][:16]}... "
        f"witness={export_result['ExportSignatureHash'][:16]}...")

    if "ExportedTxn" in export_result:
        raise AssertionError(
            "ExportResult should reference ExportSignatureHash, not embed "
            "ExportedTxn"
        )

    if require_signers:
        if ctx is None:
            raise AssertionError(
                "assert_export_result(require_signers=True) needs ctx to "
                "dereference ExportSignatureHash"
            )
        witness = find_export_signature_witness(
            ctx,
            export_result["LedgerSequence"],
            export_result["ExportSignatureHash"],
        )
        if witness.get("TransactionHash") != export_result["TransactionHash"]:
            raise AssertionError(
                "ExportSignatures witness TransactionHash does not match "
                "ExportResult.TransactionHash"
            )
        if witness.get("LedgerSequence") != export_result["LedgerSequence"]:
            raise AssertionError(
                "ExportSignatures witness LedgerSequence does not match "
                "ExportResult.LedgerSequence"
            )
        signers = _signer_entries(witness)
        if not signers:
            raise AssertionError("ExportSignatures witness has no Signers")
        accounts = [s.get("Account") for s in signers]
        if accounts != sorted(accounts):
            raise AssertionError("ExportSignatures Signers are not Account-sorted")
        log(f"  Witness signers: {len(signers)} validator(s)")
        export_result["_Witness"] = witness
        export_result["_WitnessSigners"] = signers

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
