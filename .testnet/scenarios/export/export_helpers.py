"""Shared helpers for Export scenario tests."""

from __future__ import annotations

from xahaud_scripts.testnet.config import _unl_report_index, feature_name_to_hash

EXPORT_RETRY_LEDGER_WINDOW = 5
EXPORT_PUBLICATION_LEDGER_WINDOW = 5


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


def export_authority(ctx, *, require_unl_report=True):
    """Build the explicit authority declaration for a direct Export.

    Direct clients pin the validated parent ledger and select members from its
    canonical UNLReport ordering. Hook-created exports derive the same tuple at
    admission and do not use this helper.
    """
    ledger_result = ctx.ledger("validated") or {}
    ledger = ledger_result.get("ledger", {})
    universe_hash = ledger_result.get("ledger_hash") or ledger.get("hash")
    if not universe_hash:
        raise AssertionError(f"Validated ledger hash unavailable: {ledger_result}")

    report = (
        ctx.rpc.request(
            0,
            "ledger_entry",
            {"index": _unl_report_index(), "ledger_hash": universe_hash},
        )
        or {}
    )
    active = report.get("node", {}).get("ActiveValidators", [])
    if not active:
        if require_unl_report:
            raise AssertionError(f"UNLReport active universe unavailable: {report}")
        active = [{}]

    committee = bytearray((len(active) + 7) // 8)
    for index in range(len(active)):
        committee[index // 8] |= 1 << (index % 8)

    return {
        "ExportUniverseHash": universe_hash,
        "ExportCommittee": committee.hex().upper(),
    }


def find_export_signature_witness(ctx, seq, origin_hash):
    """Find a later-ledger ExportSignatures witness for an Export origin."""
    result = ctx.ledger(seq, transactions=True)
    txns = (result or {}).get("ledger", {}).get("transactions", [])
    for tx in txns:
        if not isinstance(tx, dict):
            continue
        if tx.get("TransactionType") != "ExportSignatures":
            continue
        if tx.get("TransactionHash") == origin_hash:
            return tx
    return None


async def wait_for_export_signature_witness(
    ctx,
    log,
    origin_hash,
    *,
    after_ledger,
    max_ledgers=EXPORT_PUBLICATION_LEDGER_WINDOW,
    expect_witness=True,
):
    """Wait through the publication window for an origin-keyed witness."""
    scanned = after_ledger
    target = after_ledger + max_ledgers
    while scanned < target:
        current = ctx.validated_ledger_index(0)
        if current is None or current <= scanned:
            await ctx.wait_for_ledger(scanned + 1, node_id=0, timeout=30)
            current = ctx.validated_ledger_index(0)
        if current is None or current <= scanned:
            continue

        scan_through = min(current, target)
        for seq in range(scanned + 1, scan_through + 1):
            witness = find_export_signature_witness(ctx, seq, origin_hash)
            if witness:
                if not expect_witness:
                    raise AssertionError(
                        f"Unexpected ExportSignatures witness in ledger {seq}"
                    )
                log(f"  ExportSignatures witness found in ledger {seq}")
                return assert_export_witness(witness, origin_hash, seq, log)
        scanned = scan_through

    if expect_witness:
        raise AssertionError(
            f"No ExportSignatures witness for {origin_hash} within "
            f"{max_ledgers} validated ledgers"
        )
    log(f"  No witness observed through ledger {scanned}")
    return None


async def wait_for_validated_transaction(
    ctx, tx_hash, *, after_ledger, max_ledgers=EXPORT_RETRY_LEDGER_WINDOW
):
    """Resolve a raw non-tes submit result to validated transaction evidence."""
    checked = after_ledger
    target = after_ledger + max_ledgers
    while True:
        result = ctx.rpc.request(0, "tx", {"transaction": tx_hash}) or {}
        if result.get("validated"):
            return result
        if checked >= target:
            break
        await ctx.wait_for_ledger(checked + 1, node_id=0, timeout=30)
        current = ctx.validated_ledger_index(0)
        checked = min(target, max(checked + 1, current or checked + 1))
    raise AssertionError(f"Transaction {tx_hash} did not validate by ledger {target}")


async def submit_direct_export(ctx, log, tx, wallet, *, timeout=60, max_rebases=2):
    """Submit a direct Export, rebasing after a validated parent mismatch."""
    for attempt in range(max_rebases + 1):
        current = ctx.validated_ledger_index(0)
        if current is None:
            raise AssertionError("Validated ledger unavailable before Export")

        candidate = dict(tx)
        candidate.update(export_authority(ctx))
        candidate["LastLedgerSequence"] = current + EXPORT_RETRY_LEDGER_WINDOW
        result = await ctx.submit_and_wait(candidate, wallet, timeout=timeout)
        if result.get("engine_result") != "tecEXPORT_UNIVERSE_MISMATCH":
            return result

        tx_hash = result.get("tx_json", {}).get("hash")
        if not tx_hash:
            raise AssertionError(f"Universe mismatch missing tx hash: {result}")
        validated = await wait_for_validated_transaction(
            ctx, tx_hash, after_ledger=current
        )
        meta = validated.get("meta", {})
        if meta.get("TransactionResult") != "tecEXPORT_UNIVERSE_MISMATCH":
            raise AssertionError(
                f"Unexpected validated rebase result for {tx_hash}: {validated}"
            )
        log(f"  Direct Export parent changed; rebasing attempt {attempt + 1}")

    raise AssertionError(f"Direct Export parent changed more than {max_rebases} times")


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


def assert_hook_accepted(meta, log, *, expected_emits=1, expected_exports=None):
    """Assert hook executed with ACCEPT and expected emission counts.

    Checks sfHookExecutions in transaction metadata.
    Returns the hook execution entry for further inspection.
    """
    hook_execs = meta.get("HookExecutions", [])
    if not hook_execs:
        raise AssertionError("No HookExecutions in metadata")

    exec_entry = hook_execs[0].get("HookExecution", {})
    hook_result = exec_entry.get("HookResult", -1)
    emit_count = exec_entry.get("HookEmitCount", -1)
    export_count = exec_entry.get("HookExportCount")
    return_code = exec_entry.get("HookReturnCode", "")

    log(
        f"  HookResult={hook_result} EmitCount={emit_count} "
        f"ExportCount={export_count} ReturnCode={return_code}"
    )

    # HookResult 3 = ExitType::ACCEPT
    if hook_result != 3:
        raise AssertionError(
            f"Hook did not ACCEPT: HookResult={hook_result} ReturnCode={return_code}"
        )

    if emit_count != expected_emits:
        raise AssertionError(f"Expected {expected_emits} emits, got {emit_count}")

    if expected_exports is not None and export_count != expected_exports:
        raise AssertionError(f"Expected {expected_exports} exports, got {export_count}")

    # ReturnCode 0 = success; non-zero = ASSERT line number in hook
    if return_code and str(return_code) != "0":
        raise AssertionError(
            f"Hook returned error code {return_code} "
            f"(likely ASSERT failure at that line)"
        )

    return exec_entry


def _signer_entries(witness):
    entries = []
    exported = witness.get("ExportedTxn", {})
    for entry in exported.get("Signers", []):
        signer = entry.get("Signer", entry)
        entries.append(signer)
    return entries


def _account_sort_key(address):
    from xrpl.core.addresscodec import decode_classic_address

    return decode_classic_address(address)


def assert_export_witness(witness, origin_hash, ledger_seq, log):
    """Assert a later-ledger witness contains one canonical target assembly."""
    if witness.get("TransactionType") != "ExportSignatures":
        raise AssertionError("Expected ExportSignatures witness")
    if witness.get("TransactionHash") != origin_hash:
        raise AssertionError("ExportSignatures origin binding mismatch")
    if witness.get("LedgerSequence") != ledger_seq:
        raise AssertionError("ExportSignatures ledger binding mismatch")
    if witness.get("Signers"):
        raise AssertionError("Witness Signers must be nested under ExportedTxn")
    if not witness.get("EntropyContributors"):
        raise AssertionError("ExportSignatures missing contributor bitmap")

    signers = _signer_entries(witness)
    if not signers:
        raise AssertionError("ExportSignatures assembled transaction has no Signers")
    accounts = [signer.get("Account") for signer in signers]
    if accounts != sorted(accounts, key=_account_sort_key):
        raise AssertionError("ExportSignatures Signers are not Account-sorted")
    log(f"  Witness signers: {len(signers)} validator(s)")
    witness["_WitnessSigners"] = signers
    return witness


def assert_shadow_ticket(
    ctx,
    account_address,
    log,
    *,
    expect_exists=True,
    origin_hash=None,
    expect_witness=None,
    ledger_hash=None,
):
    """Assert shadow ticket exists (or doesn't) for the account."""
    params = {"account": account_address}
    if ledger_hash is not None:
        params["ledger_hash"] = ledger_hash
    obj_result = ctx.rpc.request(0, "account_objects", params)
    all_objects = (obj_result or {}).get("account_objects", [])
    shadow_tickets = [
        obj for obj in all_objects if obj.get("LedgerEntryType") == "ShadowTicket"
    ]
    log(f"  Shadow tickets: {len(shadow_tickets)}")

    if origin_hash is not None:
        shadow_tickets = [
            ticket
            for ticket in shadow_tickets
            if ticket.get("TransactionHash") == origin_hash
        ]

    if expect_exists and not shadow_tickets:
        raise AssertionError("Expected shadow ticket but none found")
    if not expect_exists and shadow_tickets:
        raise AssertionError(
            f"Expected no shadow tickets but found {len(shadow_tickets)}"
        )

    for ticket in shadow_tickets:
        if "Digest" not in ticket:
            raise AssertionError(
                "ShadowTicket missing signature-independent intent Digest"
            )
        if "TransactionHash" not in ticket:
            raise AssertionError("ShadowTicket missing Export origin TransactionHash")
        if expect_witness is True and "ExportSignatureHash" not in ticket:
            raise AssertionError("ShadowTicket missing ExportSignatureHash")
        if expect_witness is False and "ExportSignatureHash" in ticket:
            raise AssertionError("Pending ShadowTicket unexpectedly witnessed")

    return shadow_tickets
