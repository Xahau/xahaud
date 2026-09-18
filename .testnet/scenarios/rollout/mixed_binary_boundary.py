"""Mixed-binary rollout boundary for ConsensusEntropy.

Topology (launch with per-node binaries):
  n0-n2  @export-rng   feature-export-rng build, supports Export + ConsensusEntropy, UNL validators
  n3-n5  @release       2026.6.21 mainnet release build, NO CE support, non-UNL trackers

What this proves:
  Phase 1  CE inactive -> heterogeneous net is healthy. New validators emit
           legacy 32-byte proposal positions, so old nodes parse them and track
           validated ledgers.
  Phase 2  CE activates -> the old @release nodes hit the upgrade boundary. Per
           this branch's setAmendmentBlocked() (Change.cpp / LedgerMaster.cpp ->
           NetworkOPs) they become amendment-blocked and DROP to CONNECTED, but
           KEEP RUNNING (RPC stays up) -- they do NOT crash. This is the
           "upgrade validators before activating" rollout invariant, observed.

Launch (fast dev check, CE enabled at genesis):
  x-testnet run -n 6 \
    --node-binary n0:@export-rng --node-binary n1:@export-rng --node-binary n2:@export-rng \
    --node-binary n3:@release    --node-binary n4:@release    --node-binary n5:@release \
    --feature @ConsensusEntropy \
    --scenario-script .testnet/scenarios/rollout/mixed_binary_boundary.py --teardown

Launch (real transition, activates at the next flag ledger ~256; seed pre-satisfies the 1-min hold):
  x-testnet run -n 6 <same --node-binary set> \
    --seed-majority @ConsensusEntropy \
    --scenario-script .testnet/scenarios/rollout/mixed_binary_boundary.py --teardown
"""

from helpers import CONSENSUS_ENTROPY_FEATURE

VALIDATORS = [0, 1, 2]
OLD_NODES = [3, 4, 5]


def _ce_enabled(ctx, node_id=0):
    s = ctx.feature_check(CONSENSUS_ENTROPY_FEATURE, node_id=node_id)
    return bool(s and s.get("enabled"))


def _blocked(ctx, nid):
    info = ctx.rpc.server_info(nid) or {}
    return bool(info.get("info", {}).get("amendment_blocked"))


async def scenario(ctx, log):
    await ctx.wait_for_ledger_close(timeout=90)

    if not _ce_enabled(ctx):
        # Phase 1: with CE inactive the old @release trackers must stay in sync.
        target = (await ctx.wait_for_ledgers(3, timeout=180)).result
        for nid in OLD_NODES:
            await ctx.wait_for_ledger(target, node_id=nid, timeout=120)
        log(f"phase1 OK: old @release nodes tracked to ledger {target} (CE inactive)")

        # Trigger phase 2: vote CE up on the new validators only.
        ctx.feature(CONSENSUS_ENTROPY_FEATURE, vetoed=False, nodes=VALIDATORS)
        log("voted ConsensusEntropy accept on n0-n2; awaiting activation...")
    else:
        log("CE enabled at genesis; skipping phase 1, checking the boundary directly")

    await ctx.wait_for_feature(
        CONSENSUS_ENTROPY_FEATURE,
        check=lambda s: s.get("enabled"),
        nodes=VALIDATORS,
        timeout=1200,
    )
    log("ConsensusEntropy ENABLED on validators n0-n2")

    # The upgrade boundary: old non-UNL nodes must become amendment-blocked...
    await ctx.wait_for_nodes(
        lambda nid: _blocked(ctx, nid), nodes=OLD_NODES, timeout=180
    )
    # ...report it in the log...
    ctx.assert_log("server blocked", nodes=OLD_NODES)
    # ...and still be alive (RPC responsive) -> blocked, not crashed.
    for nid in OLD_NODES:
        assert ctx.rpc.server_info(nid), f"n{nid} RPC unreachable (crashed?)"

    log(
        "PASS: n3-n5 (@release) amendment-blocked and STILL RUNNING -- upgrade boundary confirmed"
    )
