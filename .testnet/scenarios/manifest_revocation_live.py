"""Revoke one of five validators live and watch the terminal update propagate.

Five validators are used deliberately: revoking one leaves four of five —
exactly the 80% quorum — so the network keeps closing ledgers and the
scenario can assert liveness with the revoked member excluded, not merely a
halt. (On a three-node UNL the same revocation stops the network: small-net
quorums round up to everyone.)

The revocation is installed by config on the OLD release node deliberately:
under the new transport slice a config-loaded revocation on an upgraded node
sits in its durable cache and is never gossiped (there is no connect-time
dump and no ambient manifest relay), while the old binary still dumps its
manifest cache on every fresh connection and relays what it accepts. The old
node is therefore the only in-topology seeder for a config-injected
revocation — that asymmetry is itself part of the documented durability
boundary.
"""

from xahaud_scripts.testnet.scenario import (
    AssertionError as ScenarioAssertion,
)


async def _await_log(ctx, log, pattern, *, nodes, name, since=None, deadline=36):
    # Catch the runner's ScenarioAssertion, not the builtin. The scenario
    # module shadows AssertionError without subclassing it.
    for attempt in range(deadline):
        try:
            ctx.assert_log(pattern, nodes=nodes, **({"since": since} if since else {}))
            return
        except ScenarioAssertion:
            if attempt == deadline - 1:
                raise
            if attempt % 6 == 5:
                log(f"{name}: attempt {attempt + 1}/{deadline}")
            await ctx.sleep(5, name=name)


async def scenario(ctx, log):
    nodes = [0, 1, 2, 3, 4, 5]
    # Validators 0-4 meet at n0. The old release node n5 is a pendant on n4
    # so it can seed the revocation without joining the UNL mesh.
    expected = ctx.topology_edges(
        [(0, 1), (0, 2), (0, 3), (0, 4), (4, 5)]
    )

    await ctx.apply_topology(expected, nodes=nodes, exact=False)

    # Normal operation first: everyone validates and validator 4's manifest
    # is durably known, so the revocation supersedes real retained state.
    await ctx.wait_for_ledgers(2, node_id=0, timeout=180)

    revoked = ctx.mark("revoked")
    result = await ctx.revoke_validator(4, 5)
    log(
        "revoked validator n4 master via the old relay n5: "
        f"{result['public_key']}"
    )

    # The restarted old node loads the revocation and seeds it through its
    # legacy connect-time dump and accept-relay; its peer n4 spreads it into
    # the mesh through the normal revocation relay lane. The old node's
    # reconnect interval dominates the latency, so wait on the log fact.
    await _await_log(
        ctx,
        log,
        "Revoked",
        nodes=[4],
        name="await-revocation-arrival",
        since=revoked,
        deadline=36,
    )

    # Terminal manifest applied and relayed onward by upgraded nodes: first
    # at the old seeder's direct peer, then across the mesh.
    ctx.assert_log(
        "manifest_revocation accepted_for_relay",
        since=revoked,
        nodes=[4],
    )
    await _await_log(
        ctx,
        log,
        "Revoked",
        nodes=[0],
        name="await-mesh-revocation",
        since=revoked,
        deadline=36,
    )
    ctx.assert_log(
        "manifest_revocation accepted_for_relay",
        since=revoked,
        nodes=[0],
    )

    # Liveness with the revoked member excluded: four of five is exactly
    # quorum, so ledgers keep closing.
    await ctx.wait_for_ledgers(2, node_id=0, timeout=180)

    ctx.assert_not_log("Validation forwarded by peer is invalid", nodes=nodes)
    ctx.assert_not_log("Validation: Too small", nodes=nodes)

    log(
        "PASS: a config-injected revocation seeded by the old relay reached"
        " the mesh, applied terminally on upgraded nodes, relayed onward,"
        " and the remaining four validators kept the network live at exact"
        " quorum"
    )
