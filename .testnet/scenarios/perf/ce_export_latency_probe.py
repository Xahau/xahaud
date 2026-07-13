""":descr: measure CE/export behavior while RuntimeConfig injects latency/drop.

The suite supplies runtime fault injection through network.rc. This scenario
does not mutate RuntimeConfig itself; it observes what the launched network does
under that condition and logs enough counters to compare variants.
"""

from __future__ import annotations

from collections import Counter
import json

from export.export_helpers import assert_export_result, require_export
from helpers import consensus_entropy_feature, get_entropy_tx


async def _require_runtime_config(ctx, log):
    result = ctx.rpc.runtime_config(0)
    if not result or result.get("error"):
        raise AssertionError(
            "Latency probe requires a binary built with "
            "xahaud_runtime_test_config=ON; runtime_config RPC returned "
            f"{result}"
        )
    log("RuntimeConfig RPC active")


async def _require_consensus_entropy(ctx, log):
    feature = consensus_entropy_feature(ctx, node_id=0)
    if not feature or not feature.get("enabled", False):
        raise AssertionError(f"ConsensusEntropy not enabled: {feature}")
    log("ConsensusEntropy enabled")


def _log_runtime_config(ctx, log):
    for node_id in range(ctx.node_count):
        cfg = ctx.rpc.runtime_config(node_id)
        if cfg is None:
            raise AssertionError(f"runtime_config RPC failed on node {node_id}")
        log(
            f"runtime_config n{node_id}: "
            f"{json.dumps(cfg, sort_keys=True, separators=(',', ':'))}"
        )


async def _submit_direct_export(ctx, log, *, timeout):
    await ctx.fund_accounts({"alice": 10000, "bob": 1000})
    alice = ctx.account("alice")
    bob = ctx.account("bob")
    current_seq = ctx.validated_ledger_index(0)

    if current_seq is None:
        raise AssertionError("validated ledger is not available before Export")

    log(f"Submitting direct Export at validated ledger {current_seq}")
    started = ctx.mark("latency-export-submit-start")
    result = await ctx.submit_and_wait(
        {
            "TransactionType": "Export",
            "LastLedgerSequence": current_seq + 12,
            "Fee": "1000000",
            "ExportedTxn": {
                "TransactionType": "Payment",
                "Account": alice.address,
                "Destination": bob.address,
                "Amount": "1000000",
                "Fee": "10",
                "Sequence": 0,
                "TicketSequence": 1,
                "FirstLedgerSequence": current_seq + 1,
                "LastLedgerSequence": current_seq + 10,
                "Flags": 2147483648,
                "SigningPubKey": "",
            },
        },
        alice.wallet,
        timeout=timeout,
    )
    ended = ctx.mark("latency-export-submit-end")

    elapsed = (ended.monotonic_ns - started.monotonic_ns) / 1_000_000_000
    engine_result = result.get("engine_result", "")
    log(f"Export result={engine_result} elapsed={elapsed:.3f}s")

    if engine_result != "tesSUCCESS":
        raise AssertionError(f"Expected Export tesSUCCESS, got {engine_result}")

    export_result = assert_export_result(result.get("meta", {}), log)
    signers = export_result.get("ExportedTxn", {}).get("Signers", [])
    log(f"Export signer count={len(signers)}")
    return started, ended


def _summarize_logs(ctx, log, *, label, started, ended):
    patterns = {
        "rng_selected": r"RNG: entropy selected",
        "rng_fallback": r"tier=1",
        "rng_participant_aligned": r"tier=2",
        "rng_validator_quorum": r"tier=3",
        "export_retry": r"terRETRY_EXPORT",
        "export_quorum_timeout": r"Export: exportSigSet quorum alignment timeout",
        "export_missing_observation_ignored": (
            r"Export: missing exportSigSetHash observation ignored"
        ),
    }
    for name, pattern in patterns.items():
        result = ctx.search_logs(pattern, since=started, until=ended, limit=500)
        log(f"log_count {label}.{name}={result.count}")


async def scenario(
    ctx,
    log,
    *,
    warmup_ledgers=3,
    ledgers=8,
    submit_export=False,
    export_timeout=90,
):
    await ctx.wait_for_ledger_close(timeout=120)
    await _require_runtime_config(ctx, log)
    _log_runtime_config(ctx, log)
    await _require_consensus_entropy(ctx, log)

    if submit_export:
        # require_export also asserts the UNLReport precondition for successful
        # network-mode Export. Keep that explicit in perf runs so a missing
        # report does not masquerade as a latency failure.
        await require_export(ctx, log, require_runtime_config=False)

    await ctx.wait_for_ledgers(warmup_ledgers, node_id=0, timeout=120)
    warm_seq = ctx.validated_ledger_index(0)
    log(f"Warmup complete at validated ledger {warm_seq}")

    export_window = None
    if submit_export:
        export_window = await _submit_direct_export(
            ctx, log, timeout=export_timeout
        )

    started = ctx.mark("latency-probe-start")
    start_seq = ctx.validated_ledger_index(0)
    await ctx.wait_for_ledgers(ledgers, node_id=0, timeout=max(120, ledgers * 30))
    ended = ctx.mark("latency-probe-end")
    end_seq = ctx.validated_ledger_index(0)

    if start_seq is None or end_seq is None:
        raise AssertionError("validated ledger index unavailable during probe")

    elapsed = (ended.monotonic_ns - started.monotonic_ns) / 1_000_000_000
    closed = max(0, end_seq - start_seq)
    cadence = elapsed / closed if closed else 0.0
    log(
        f"Observed validated ledgers {start_seq + 1}..{end_seq} "
        f"closed={closed} elapsed={elapsed:.3f}s cadence={cadence:.3f}s/ledger"
    )

    tiers: Counter[int] = Counter()
    counts: Counter[int] = Counter()
    missing_entropy = 0
    for seq in range(start_seq + 1, end_seq + 1):
        try:
            ce, user_txns = get_entropy_tx(ctx, seq)
        except AssertionError as exc:
            missing_entropy += 1
            log(f"  Ledger {seq}: no ConsensusEntropy tx ({exc})")
            continue

        tier = ce.get("EntropyTier", -1)
        count = ce.get("EntropyCount", -1)
        tiers[tier] += 1
        counts[count] += 1
        log(
            f"  Ledger {seq}: tier={tier} count={count} "
            f"user_txns={len(user_txns)} digest={ce.get('Digest', '')[:16]}..."
        )

    log(
        "SUMMARY "
        f"closed={closed} elapsed_s={elapsed:.3f} cadence_s={cadence:.3f} "
        f"tiers={dict(sorted(tiers.items()))} "
        f"counts={dict(sorted(counts.items()))} "
        f"missing_entropy={missing_entropy}"
    )

    _summarize_logs(ctx, log, label="probe", started=started, ended=ended)
    if export_window is not None:
        _summarize_logs(
            ctx,
            log,
            label="export",
            started=export_window[0],
            ended=export_window[1],
        )

    log("PASS")
