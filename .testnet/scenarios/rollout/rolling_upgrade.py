"""Realistic rolling binary upgrade, then ConsensusEntropy activation.

Topology (ALL start on @release, a build with NO CE support):
  n0-n4  5 UNL validators   (quorum 4)
  n5     tracker (non-UNL)  -> WILL be upgraded; keeps working after activation
  n6     tracker (non-UNL)  -> NOT upgraded (the straggler); becomes
                               amendment-blocked after activation but does NOT crash

TOPOLOGY-AWARE RESTARTS: the fixed-peer mesh takes time to form, and a node whose
peers haven't fully connected acts as a junction -- restarting it partitions the
rest (observed: restarting n0 dropped n1 to 0 peers -> isolated -> stalled). So we
require a well-connected mesh (>= MESH_MIN peers on every node) BEFORE rolling and
after EACH restart, before touching the next node. If the mesh never forms this
fails loud (a peering problem), rather than silently stalling consensus.

Why --seed-majority is used (NOT a magic vote): the validators still VOTE the
amendment up (ctx.feature below). Seeding only pre-writes the sfMajorities record
with CloseTime=0 so the hold is already satisfied, collapsing activation to ONE
flag ledger. If the validators don't vote yes it is cleared (tfLostMajority). See
prepare_genesis_file() in testnet/config.py.

Arc: all-old healthy + mesh formed -> rolling-upgrade 5 validators + 1 tracker
(mesh re-forms between each) -> vote CE -> activation at the flag ledger ->
n5 (upgraded) keeps tracking, n6 (straggler) amendment-blocked but not crashed.

Run: x-testnet --rippled-path @release suite \\
       .testnet/scenarios/rollout/rollout-suite.yml --stop-on-fail
"""

from helpers import CONSENSUS_ENTROPY_FEATURE

VALIDATORS = [0, 1, 2, 3, 4]
UPGRADED_TRACKER = 5
STRAGGLER_TRACKER = 6
ALL_NODES = VALIDATORS + [UPGRADED_TRACKER, STRAGGLER_TRACKER]
# Full mesh is 6 peers each (7 nodes). Require >= 4 so no node is a junction:
# removing any one still leaves the rest connected.
MESH_MIN = 4


def _info(ctx, nid):
    return (ctx.rpc.server_info(nid) or {}).get("info", {})


def _blocked(ctx, nid):
    return bool(_info(ctx, nid).get("amendment_blocked"))


def _peers(ctx, nid):
    return int(_info(ctx, nid).get("peers") or 0)


async def _wait_mesh(ctx, log, *, timeout=180):
    """Wait until every node is well-connected (>= MESH_MIN peers), so a restart
    can't partition the network. Fails loud (TimeoutError) if the mesh never
    forms -- that means a peering problem, not a transient."""
    await ctx.wait_for_nodes(
        lambda x: _peers(ctx, x) >= MESH_MIN, nodes=ALL_NODES, timeout=timeout
    )
    peers = {nid: _peers(ctx, nid) for nid in ALL_NODES}
    log(f"peer mesh healthy (>= {MESH_MIN} peers each): {peers}")


async def scenario(ctx, log):
    # 1. all-old net healthy AND the full peer mesh has formed before we touch it
    await ctx.wait_for_ledger_close(timeout=90)
    base = (await ctx.wait_for_ledgers(2, timeout=120)).result
    for nid in ALL_NODES:
        await ctx.wait_for_ledger(base, node_id=nid, timeout=120)
    await _wait_mesh(ctx, log, timeout=180)
    log(f"all-old net healthy at ledger {base} (CE inactive)")

    # 2. rolling-upgrade validators (then one tracker), one at a time. After each
    #    restart, give the mesh time to re-form (the restarted node re-dials its
    #    fixed peers) BEFORE rolling the next, so we never take out a junction.
    for nid in [*VALIDATORS, UPGRADED_TRACKER]:
        ref = next(v for v in VALIDATORS if v != nid)
        role = "validator" if nid in VALIDATORS else "tracker"
        log(f"rolling upgrade: n{nid} ({role}) -> @export-rng")
        await ctx.restart_node_with_binary(nid, "@export-rng", delay=3)
        await _wait_mesh(ctx, log, timeout=180)  # mesh re-formed before next roll
        target = (await ctx.wait_for_ledgers(2, node_id=ref, timeout=180)).result
        await ctx.wait_for_ledger(target, node_id=nid, timeout=180)
        log(f"n{nid} rejoined; mesh re-formed; quorum advanced to {target}")
    log(
        f"validators + n{UPGRADED_TRACKER} on @export-rng; "
        f"n{STRAGGLER_TRACKER} left on @release; CE still inactive"
    )

    # 3. vote CE up on the validators (real vote; the seed only pre-satisfied the hold)
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

    # 5. upgraded tracker keeps working; straggler is amendment-blocked (not crashed)
    await ctx.wait_for_nodes(
        lambda x: _blocked(ctx, x), nodes=[STRAGGLER_TRACKER], timeout=180
    )
    ctx.assert_log("server blocked", nodes=[STRAGGLER_TRACKER])
    assert not _blocked(ctx, UPGRADED_TRACKER), (
        f"n{UPGRADED_TRACKER} (upgraded tracker) should NOT be amendment-blocked"
    )
    await ctx.wait_for_ledgers(2, node_id=UPGRADED_TRACKER, timeout=120)
    for nid in (UPGRADED_TRACKER, STRAGGLER_TRACKER):
        assert ctx.rpc.server_info(nid), f"n{nid} RPC down (crashed?)"
    log(
        f"PASS: rolling upgrade preserved quorum 4 (mesh-gated); CE activated; "
        f"n{UPGRADED_TRACKER} (upgraded tracker) still tracking; "
        f"n{STRAGGLER_TRACKER} (@release straggler) amendment-blocked and still running"
    )
