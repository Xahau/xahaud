"""Exercise manifest/validation traffic across old and new binaries."""


async def scenario(ctx, log):
    nodes = [0, 1, 2]
    expected = ctx.topology_edges([(0, 1), (1, 2)])

    await ctx.apply_topology(expected, nodes=nodes, exact=False)
    # The first close produces the validator traffic under test; later closes
    # produce naked relays from the old middle node (it forwards a manifest
    # singleton at most once), which must draw the bounded repair. These
    # binaries are different product revisions, so multi-ledger convergence is
    # deliberately not used as the protocol-compatibility oracle.
    await ctx.wait_for_ledgers(3, node_id=0, timeout=180)

    # n0 is the sole validator and uses the new ordered prerequisite path
    # toward the old middle node. The old node processes and relays the two
    # existing envelope types normally. n2 is a new observer and must retain
    # the relayed candidate across unrelated traffic until its validation.
    ctx.assert_log("manifest_validation pair_enqueued", nodes=[0])
    ctx.assert_log("manifest_validation candidate_staged", nodes=[2])
    ctx.assert_log("manifest_validation candidate_matched", nodes=[2])
    ctx.assert_log("manifest_validation validation_parsed", nodes=[2])
    ctx.assert_log("manifest_validation single_manifest_processed", nodes=[2])
    ctx.assert_log_order(
        [
            "manifest_validation candidate_staged",
            "manifest_validation candidate_matched",
            "manifest_validation single_manifest_processed",
        ],
        nodes=[2],
    )

    # After durable admission, the old relay's later validations arrive
    # naked; an authenticated naked validation is an implicit request, so
    # the upgraded observer repairs its sender once per master/sequence.
    ctx.assert_log("manifest_validation repair_sent", nodes=[2])
    ctx.assert_log_order(
        [
            "manifest_validation single_manifest_processed",
            "manifest_validation repair_sent",
        ],
        nodes=[2],
    )

    ctx.assert_not_log("Validation forwarded by peer is invalid", nodes=nodes)
    ctx.assert_not_log("Validation: Too small", nodes=nodes)

    log(
        "PASS: a release relay accepted the upgraded sender's existing "
        "manifest/validation envelopes; the upgraded observer retained, "
        "matched, verified, and admitted the relayed prerequisite"
    )
