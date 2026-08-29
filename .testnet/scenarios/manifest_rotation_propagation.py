"""Rotate a validator's manifest mid-run and watch the bump propagate.

"Heals on a sequence bump" is the load-bearing healing claim of the
manifest-before-validation transport slice. This exercises it live for the
first time: the validator mints sequence 2 and restarts; its validations then
carry the new manifest; an old release relay forwards the existing envelopes;
the upgraded observer supersedes its retained knowledge and admits sequence 2;
and the observer's repair lane re-arms at the newer sequence when the old
relay's later validations arrive naked.
"""

from xahaud_scripts.testnet.scenario import (
    AssertionError as ScenarioAssertion,
)



async def scenario(ctx, log):
    nodes = [0, 1, 2]
    expected = ctx.topology_edges([(0, 1), (1, 2)])

    await ctx.apply_topology(expected, nodes=nodes, exact=False)

    # Baseline: sequence 1 propagates through the old relay and is admitted
    # by the upgraded observer before we rotate anything. The validator can
    # race several ledgers ahead of propagation under fast bootstrap, so
    # wait on the log fact itself.
    await ctx.wait_for_ledgers(2, node_id=0, timeout=120)
    deadline = 24
    for attempt in range(deadline):
        try:
            ctx.assert_log(
                "manifest_validation single_manifest_processed .*sequence=1",
                nodes=[2],
            )
            break
        except ScenarioAssertion:
            if attempt == deadline - 1:
                raise
            if attempt % 6 == 5:
                log(f"await-baseline-admission: attempt {attempt + 1}/{deadline}")
            await ctx.sleep(5, name="await-baseline-admission")

    rotated = ctx.mark("rotated")
    rotation = await ctx.rotate_validator_manifest(0)
    assert rotation["sequence"] == 2, rotation
    log(f"rotated validator n0 to manifest sequence {rotation['sequence']}")

    # The restarted validator re-joins and validates under the new signing
    # key; always-send carries the sequence-2 prerequisite with each
    # validation, and the old relay's one-shot manifest forward plus later
    # naked relays exercise both the supersede and the repair re-arm. Wait
    # on the terminal log fact rather than ledger counts: in this topology
    # only the validator advances its ledger, and its own restart closed the
    # node-0 WebSocket ledger feed. The repair re-arm is the last event in
    # the causal chain, so everything else must precede it.
    deadline = 36  # polls at 5s => 180s budget at ~16s consensus rounds
    for attempt in range(deadline):
        try:
            ctx.assert_log(
                "manifest_validation repair_sent .*sequence=2",
                since=rotated,
                nodes=[2],
            )
            break
        except ScenarioAssertion:
            if attempt == deadline - 1:
                raise
            if attempt % 6 == 5:
                log(f"await-repair-rearm: attempt {attempt + 1}/{deadline}")
            await ctx.sleep(5, name="await-repair-rearm")

    ctx.assert_log(
        "manifest_validation pair_enqueued .*sequence=2",
        since=rotated,
        nodes=[0],
    )
    ctx.assert_log(
        "manifest_validation candidate_staged .*sequence=2",
        since=rotated,
        nodes=[2],
    )
    ctx.assert_log(
        "manifest_validation candidate_matched",
        since=rotated,
        nodes=[2],
    )
    ctx.assert_log(
        "manifest_validation single_manifest_processed .*sequence=2"
        " .*disposition=accepted",
        since=rotated,
        nodes=[2],
    )
    ctx.assert_log_order(
        [
            "manifest_validation single_manifest_processed .*sequence=2",
            "manifest_validation repair_sent .*sequence=2",
        ],
        since=rotated,
        nodes=[2],
    )

    ctx.assert_not_log("Validation forwarded by peer is invalid", nodes=nodes)
    ctx.assert_not_log("Validation: Too small", nodes=nodes)

    log(
        "PASS: mid-run rotation to sequence 2 propagated through an old"
        " relay; the upgraded observer superseded and admitted the new"
        " manifest and re-armed its repair lane at the new sequence"
    )
