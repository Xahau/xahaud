"""Realistic rolling binary upgrade, then ConsensusEntropy activation.

Topology (ALL start on @release, a build with NO CE support):
  n0-n4  5 UNL validators   (quorum 4)
  n5     tracker (non-UNL)  -> WILL be upgraded; keeps working after activation
  n6     tracker (non-UNL)  -> NOT upgraded (the straggler); becomes
                               amendment-blocked, then protocol-isolated

TOPOLOGY-AWARE RESTARTS: the suite starts without fixed peers and this scenario
forms a directed ring over 127.0.0.1. A seven-node ring remains connected while
one node restarts and avoids macOS loopback-alias setup. After each restart we
restore the affected links and require every node to see both ring neighbours
before touching the next node.

Why --seed-majority is used (NOT a magic vote): the validators still VOTE the
amendment up (ctx.feature below). Seeding only pre-writes the sfMajorities record
with CloseTime=0 so the hold is already satisfied, collapsing activation to ONE
flag ledger. If the validators don't vote yes it is cleared (tfLostMajority). See
prepare_genesis_file() in testnet/config.py.

Arc: all-old healthy + mesh formed -> rolling-upgrade 5 validators + 1 tracker
(mesh re-forms between each) -> vote CE -> activation at the flag ledger ->
n5 (upgraded) keeps tracking, while every upgraded peer drops/rejects n6.

Run: x-testnet --rippled-path @release suite \\
       .testnet/scenarios/rollout/rollout-suite.yml --stop-on-fail

The upgrade target is the immutable saved-binary alias `@export-rng-gate`.
"""

import asyncio

from helpers import CONSENSUS_ENTROPY_FEATURE

VALIDATORS = [0, 1, 2, 3, 4]
UPGRADED_TRACKER = 5
STRAGGLER_TRACKER = 6
ALL_NODES = VALIDATORS + [UPGRADED_TRACKER, STRAGGLER_TRACKER]
# Every node has its predecessor and successor in the ring.
MESH_MIN = 2
PROTOCOL = "XRPL/2.2"
CONSENSUS_ENTROPY_CAPABILITY = "xahau-consensus-entropy"
UPGRADE_BINARY = "@export-rng-gate"
RING_EDGES = {(nid, (nid + 1) % len(ALL_NODES)) for nid in ALL_NODES}


def _info(ctx, nid):
    return (ctx.rpc.server_info(nid) or {}).get("info", {})


def _blocked(ctx, nid):
    return bool(_info(ctx, nid).get("amendment_blocked"))


def _peers(ctx, nid):
    return int(_info(ctx, nid).get("peers") or 0)


def _peer_public_keys(ctx, nid):
    result = ctx.rpc.peers(nid) or []
    return {peer.get("public_key") for peer in result if peer.get("public_key")}


def _assert_capability_matrix(ctx, log, *, upgraded, phase):
    """Assert per-edge handshake results from upgraded nodes.

    Capable peers negotiate the CE token without changing the overlay protocol.
    Old peers remain connected on XRPL/2.2 but cannot echo the token.
    """
    upgraded = set(upgraded)
    old = set(ALL_NODES) - upgraded
    key_to_node = {}
    for nid in ALL_NODES:
        public_key = _info(ctx, nid).get("pubkey_node")
        assert public_key, f"n{nid} server_info missing pubkey_node"
        key_to_node[public_key] = nid

    capable_edges = []
    legacy_edges = []
    for source in sorted(upgraded):
        peers = ctx.rpc.peers(source)
        assert peers is not None, f"n{source} peers RPC failed"
        for peer in peers:
            target = key_to_node.get(peer.get("public_key"))
            if target is None:
                continue
            protocol = peer.get("protocol")
            assert protocol == PROTOCOL, (
                f"{phase}: n{source}->n{target} negotiated {protocol!r}, "
                f"expected {PROTOCOL}"
            )
            capabilities = peer.get("capabilities") or {}
            negotiated = bool(capabilities.get(CONSENSUS_ENTROPY_CAPABILITY))
            expected = target in upgraded
            assert negotiated == expected, (
                f"{phase}: n{source}->n{target} capability "
                f"{CONSENSUS_ENTROPY_CAPABILITY}={negotiated}, expected {expected}; "
                f"peer={peer}"
            )
            (capable_edges if negotiated else legacy_edges).append(
                f"n{source}->n{target}"
            )

    if len(upgraded) > 1:
        assert capable_edges, f"{phase}: no upgraded-upgraded edge observed"
    if old:
        assert legacy_edges, f"{phase}: no upgraded-old edge observed"
    log(
        f"{phase}: {PROTOCOL} throughout; CE capability on "
        f"{len(capable_edges)} upgraded-upgraded directed edges and off on "
        f"{len(legacy_edges)} upgraded-old directed edges"
    )


def _straggler_is_isolated(ctx):
    """The old process is alive, but no upgraded node has an active session to it."""
    old_key = _info(ctx, STRAGGLER_TRACKER).get("pubkey_node")
    if not old_key or _peers(ctx, STRAGGLER_TRACKER) != 0:
        return False
    return all(old_key not in _peer_public_keys(ctx, nid) for nid in ALL_NODES[:-1])


async def _wait_for_stable_straggler_isolation(ctx, log, *, timeout=180):
    await ctx.wait_for(
        lambda: _straggler_is_isolated(ctx),
        timeout=timeout,
        poll_interval=1,
        name="legacy-straggler-isolated",
    )
    # Make the assertion survive PeerFinder's immediate reconnect cycle rather
    # than observing only the instant between two attempts.
    for _ in range(5):
        await asyncio.sleep(1)
        assert _straggler_is_isolated(ctx), (
            f"n{STRAGGLER_TRACKER} regained an incompatible active session"
        )
    log(
        f"n{STRAGGLER_TRACKER} remains RPC-alive but protocol-isolated: "
        f"peers={_peers(ctx, STRAGGLER_TRACKER)}"
    )


async def _wait_mesh(ctx, log, *, timeout=180):
    """Wait until every node sees both ring neighbours."""
    await ctx.wait_for_nodes(
        lambda x: _peers(ctx, x) >= MESH_MIN, nodes=ALL_NODES, timeout=timeout
    )
    peers = {nid: _peers(ctx, nid) for nid in ALL_NODES}
    log(f"peer mesh healthy (>= {MESH_MIN} peers each): {peers}")


async def _restore_ring(ctx, log, *, timeout=90):
    """Restore missing ring links using localhost plus each node's peer port."""
    await ctx.wait_for_nodes(
        lambda nid: ctx.rpc.server_info(nid) is not None,
        nodes=ALL_NODES,
        timeout=min(timeout, 30),
    )
    current = ctx.topology_snapshot(nodes=ALL_NODES).outbound_edges
    missing = RING_EDGES - current
    for source, target in sorted(missing):
        target_node = ctx._node_info(target)
        result = ctx.rpc.connect(source, "127.0.0.1", target_node.port_peer)
        assert result and result.get("status") == "success", (
            f"failed to connect ring edge n{source}->n{target}: {result}"
        )
    await ctx.wait_for_topology(
        RING_EDGES,
        nodes=ALL_NODES,
        exact=False,
        timeout=timeout,
        stable_for=1,
    )
    log(f"ring topology restored ({len(RING_EDGES)} directed edges)")


def _ledger_transactions(result):
    """Return the transactions array from a ledger RPC result."""
    if not result:
        return []
    ledger = result.get("ledger") or {}
    return ledger.get("transactions") or result.get("transactions") or []


def _tx_types(result):
    """Return TransactionType (or 'hash') for each tx in a ledger RPC result."""
    types = []
    for tx in _ledger_transactions(result):
        if isinstance(tx, dict):
            types.append(str(tx.get("TransactionType", "<unnamed>")))
        else:
            types.append("hash")
    return types


def _rpc_summary(result):
    """Return a compact, log-safe summary of an RPC result."""
    if result is None:
        return "no response"
    if result.get("error"):
        return f"error={result.get('error')} message={result.get('error_message')}"

    ledger = result.get("ledger") or {}
    transactions = _ledger_transactions(result)
    tx_types = _tx_types(result)
    state = result.get("state") or []
    entry_types = sorted(
        {
            str(entry.get("LedgerEntryType", "<unnamed>"))
            for entry in state
            if isinstance(entry, dict)
        }
    )
    details = ["success"]
    if ledger:
        details.append(f"ledger={ledger.get('ledger_index')}")
    details.append(f"txs={len(transactions)} types={tx_types}")
    if state:
        details.append(f"state={len(state)} types={entry_types}")
    if result.get("account_data"):
        details.append(f"account_seq={result['account_data'].get('Sequence')}")
    if result.get("marker"):
        details.append("marker=yes")
    if not ledger and not transactions and not state and not result.get("account_data"):
        details.append(f"keys={sorted(result)}")
    return "; ".join(details)


async def _wait_for_entropy_ledger(ctx, log, *, node_id, timeout=180):
    """Wait until node_id's closed ledger expands with a ConsensusEntropy tx."""
    deadline = asyncio.get_event_loop().time() + timeout
    last = None
    while asyncio.get_event_loop().time() < deadline:
        info = _info(ctx, node_id)
        seq = info.get("validated_ledger", {}).get("seq") or info.get("ledger_index")
        if seq and seq != last:
            last = seq
            result = ctx.rpc.request(
                node_id,
                "ledger",
                {
                    "ledger_index": seq,
                    "transactions": True,
                    "expand": True,
                },
            )
            types = _tx_types(result)
            log(
                f"n{node_id} closed/validated ledger {seq} expanded: "
                f"{_rpc_summary(result)}"
            )
            if "ConsensusEntropy" in types:
                return seq, types
        await asyncio.sleep(1)
    raise TimeoutError(
        f"n{node_id} did not close a ledger containing ConsensusEntropy "
        f"within {timeout}s (last={last})"
    )


async def scenario(ctx, log):
    # 1. all-old net healthy AND the ring has formed before we touch it
    await _restore_ring(ctx, log)
    await ctx.wait_for_ledger_close(timeout=90)
    base = (await ctx.wait_for_ledgers(2, timeout=120)).result
    for nid in ALL_NODES:
        await ctx.wait_for_ledger(base, node_id=nid, timeout=120)
    await _wait_mesh(ctx, log, timeout=180)
    log(f"all-old net healthy at ledger {base} (CE inactive)")

    # 2. rolling-upgrade validators (then one tracker), one at a time. After each
    #    restart, restore and settle the ring BEFORE rolling the next, so we never
    #    leave a broken path in place while taking out another node.
    upgraded = set()
    for nid in [*VALIDATORS, UPGRADED_TRACKER]:
        ref = next(v for v in VALIDATORS if v != nid)
        role = "validator" if nid in VALIDATORS else "tracker"
        log(f"rolling upgrade: n{nid} ({role}) -> {UPGRADE_BINARY}")
        await ctx.restart_node_with_binary(nid, UPGRADE_BINARY, delay=3)
        upgraded.add(nid)
        await _restore_ring(ctx, log)
        await _wait_mesh(ctx, log, timeout=180)  # mesh re-formed before next roll
        target = (await ctx.wait_for_ledgers(2, node_id=ref, timeout=180)).result
        await ctx.wait_for_ledger(target, node_id=nid, timeout=180)
        _assert_capability_matrix(
            ctx,
            log,
            upgraded=upgraded,
            phase=f"after upgrading n{nid}",
        )
        log(f"n{nid} rejoined; mesh re-formed; quorum advanced to {target}")
    log(
        f"validators + n{UPGRADED_TRACKER} on {UPGRADE_BINARY}; "
        f"n{STRAGGLER_TRACKER} left on @release; CE still inactive"
    )

    # 3. vote CE up on the validators (real vote; the seed only pre-satisfied the hold)
    gate_start = ctx.mark("ce-protocol-gate")
    ctx.feature(CONSENSUS_ENTROPY_FEATURE, vetoed=False, nodes=VALIDATORS)
    log("voted ConsensusEntropy accept on n0-n4; crossing the flag ledger...")

    # 4. activation at the flag ledger
    await ctx.wait_for_feature(
        CONSENSUS_ENTROPY_FEATURE,
        check=lambda s: s.get("enabled"),
        nodes=VALIDATORS,
        timeout=900,
    )
    log("ConsensusEntropy ENABLED on the upgraded quorum")

    # 5. The amendment-blocked behavior remains visible on the old process, but
    #    the upgraded overlay now makes the incompatibility fail fast.
    await ctx.wait_for_nodes(
        lambda x: _blocked(ctx, x), nodes=[STRAGGLER_TRACKER], timeout=180
    )
    ctx.assert_log("server blocked", nodes=[STRAGGLER_TRACKER])
    for nid in sorted(upgraded):
        ctx.assert_log(
            r"Peer protocol feature now required: xahau-consensus-entropy",
            since=gate_start,
            nodes=[nid],
        )
    for nid in (VALIDATORS[0], UPGRADED_TRACKER):
        ctx.assert_log(
            r"Missing required protocol feature xahau-consensus-entropy",
            since=gate_start,
            nodes=[nid],
        )
    await _wait_for_stable_straggler_isolation(ctx, log)

    # Exercise both admission directions after the initial eviction. The RPC
    # only schedules a connection attempt; the invariant is that neither
    # attempt becomes an active peer session.
    old_node = ctx._node_info(STRAGGLER_TRACKER)
    new_node = ctx._node_info(VALIDATORS[0])
    new_to_old = ctx.rpc.connect(UPGRADED_TRACKER, "127.0.0.1", old_node.port_peer)
    old_to_new = ctx.rpc.connect(STRAGGLER_TRACKER, "127.0.0.1", new_node.port_peer)
    log(
        "forced reconnect attempts in both directions: "
        f"new->old={new_to_old}; old->new={old_to_new}"
    )
    await _wait_for_stable_straggler_isolation(ctx, log, timeout=60)

    assert not _blocked(ctx, UPGRADED_TRACKER), (
        f"n{UPGRADED_TRACKER} (upgraded tracker) should NOT be amendment-blocked"
    )
    probe_ledger, ce_types = await _wait_for_entropy_ledger(
        ctx, log, node_id=UPGRADED_TRACKER, timeout=180
    )
    log(
        f"n{UPGRADED_TRACKER} has ConsensusEntropy at ledger {probe_ledger}: "
        f"types={ce_types}"
    )

    target = (await ctx.wait_for_ledgers(2, node_id=VALIDATORS[0], timeout=180)).result
    for nid in sorted(upgraded):
        await ctx.wait_for_ledger(target, node_id=nid, timeout=180)
    log(f"capable six-node component continued through ledger {target}")

    snapshot = ctx._network.snapshot("ce-required-protocol-gate", keep_db=False)
    log(f"captured protocol-gate and reconnect evidence at {snapshot}")
    for nid in (UPGRADED_TRACKER, STRAGGLER_TRACKER):
        assert ctx.rpc.server_info(nid), f"n{nid} RPC down (crashed?)"
    log(
        f"PASS: rolling upgrade preserved quorum 4 (mesh-gated); CE activated; "
        f"n{UPGRADED_TRACKER} (upgraded tracker) still tracking; "
        f"n{STRAGGLER_TRACKER} (@release straggler) amendment-blocked, still "
        "running for RPC observation, and rejected from the peer protocol"
    )
