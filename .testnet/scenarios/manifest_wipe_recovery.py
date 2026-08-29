"""Wipe a mid-chain node's wallet and watch both generations heal it.

The forever cache's informal replicated history is gone from new nodes, so
this pins what replaces it. A wiped upgraded node reconnects with empty
durable state; its old-release upstream re-seeds it through the legacy
connect-time dump lane, its own validation traffic re-pairs through
always-send, and it resumes forwarding paired prerequisites downstream. The
recovery, not a starvation, is the honest live boundary: every reachable
topology heals, because old peers dump at connect and new peers re-send the
prerequisite with every validation.
"""

from xahaud_scripts.testnet.scenario import (
    AssertionError as ScenarioAssertion,
)



async def scenario(ctx, log):
    nodes = [0, 1, 2, 3]
    expected = ctx.topology_edges([(0, 1), (1, 2), (2, 3)])

    await ctx.apply_topology(expected, nodes=nodes, exact=False)

    # Baseline: knowledge reaches the end of the chain. The tail observer
    # receives paired traffic from the upgraded mid-chain node. Admission at
    # the tail implies the whole upstream chain, so wait on that log fact —
    # four hops can trail the validator's fast-bootstrap ledger count.
    await ctx.wait_for_ledgers(2, node_id=0, timeout=120)
    deadline = 24
    for attempt in range(deadline):
        try:
            ctx.assert_log(
                "manifest_validation single_manifest_processed .*sequence=1",
                nodes=[3],
            )
            break
        except ScenarioAssertion:
            if attempt == deadline - 1:
                raise
            if attempt % 6 == 5:
                log(f"await-baseline-chain: attempt {attempt + 1}/{deadline}")
            await ctx.sleep(5, name="await-baseline-chain")
    ctx.assert_log(
        "manifest_validation single_manifest_processed .*sequence=1",
        nodes=[2],
    )

    wiped = ctx.mark("wiped")
    await ctx.restart_node(2, wipe_wallet_db=True)
    log("restarted n2 with a wiped wallet database")

    await ctx.wait_for_ledgers(3, node_id=0, timeout=180)

    # Wait on the terminal log fact: resumed paired forwarding downstream is
    # the last event in the recovery chain, so re-learning and re-admission
    # must precede it.
    deadline = 36
    for attempt in range(deadline):
        try:
            ctx.assert_log(
                "manifest_validation send_prerequisite .*sequence=1",
                since=wiped,
                nodes=[2],
            )
            break
        except ScenarioAssertion:
            if attempt == deadline - 1:
                raise
            if attempt % 6 == 5:
                log(f"await-resumed-forwarding: attempt {attempt + 1}/{deadline}")
            await ctx.sleep(5, name="await-resumed-forwarding")

    # Recovery: the wiped node re-learned the validator identity from live
    # traffic (the old upstream's connect-time dump arrives as a singleton
    # candidate; the next validation proves it) and admitted it durably
    # again.
    ctx.assert_log(
        "manifest_validation candidate_staged .*sequence=1",
        since=wiped,
        nodes=[2],
    )
    ctx.assert_log(
        "manifest_validation single_manifest_processed .*sequence=1"
        " .*disposition=accepted",
        since=wiped,
        nodes=[2],
    )

    ctx.assert_not_log("Validation forwarded by peer is invalid", nodes=nodes)
    ctx.assert_not_log("Validation: Too small", nodes=nodes)

    log(
        "PASS: a wallet-wiped mid-chain node re-learned the validator"
        " identity from live traffic through an old upstream, re-admitted it"
        " durably, and resumed paired forwarding to the tail observer"
    )
