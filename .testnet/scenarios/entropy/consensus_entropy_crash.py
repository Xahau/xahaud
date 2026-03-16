"""Scenario: ConsensusEntropy amendment crashes non-supporting node.

Votes ConsensusEntropy accept on all nodes except n4, then waits for n4
to crash as the amendment activates without its support.

    x-testnet run --scenario-script consensus_entropy_crash.py
"""


async def scenario(ctx, log):
    await ctx.wait_for_ledger_close()
    ctx.feature("ConsensusEntropy", vetoed=False, exclude_nodes=[4])

    log("Waiting for ConsensusEntropy to be voted for...")
    await ctx.wait_for_feature(
        "ConsensusEntropy",
        check=lambda s: not s.get("vetoed"),
        exclude_nodes=[4],
        timeout=60,
    )

    log("Waiting for n4 to crash...")
    op = await ctx.wait_for_nodes_down(nodes=[4], timeout=600)

    ctx.assert_log("unsupported amendments activated", since=op.started, nodes=[4])
    ctx.assert_exit_status(0, nodes=[4])
    log("PASS: n4 shut down due to unsupported amendment")
