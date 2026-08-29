"""Manifest packets precede validations across a real two-hop relay."""


async def scenario(ctx, log):
    nodes = [0, 1, 2]
    expected = ctx.topology_edges([(0, 1), (1, 2)])

    ctx.mark("before-manifest-validation-connect")
    # A live TCP peer session is visible from both endpoints, so require the
    # two links without treating their reverse views as extra connections.
    await ctx.apply_topology(expected, nodes=nodes, exact=False)
    await ctx.wait_for_ledgers(3, timeout=90)
    ctx.mark("after-manifest-validation-ledgers")

    send_pattern = r"manifest_validation send_prerequisite"
    pair_pattern = r"manifest_validation pair_enqueued .*order=manifest,validation"
    staged_pattern = r"manifest_validation candidate_staged"
    matched_pattern = r"manifest_validation candidate_matched"
    processed_pattern = r"manifest_validation single_manifest_processed"
    validation_pattern = r"manifest_validation validation_parsed"

    # This is a fresh per-test network, so the complete node logs are the
    # scenario range. Avoid timestamp filtering: xahaud's custom local-time
    # prefix is not yet understood by x-testnet's Marker parser.
    for node in nodes:
        sent = ctx.assert_log(send_pattern, nodes=[node])
        paired = ctx.assert_log(pair_pattern, nodes=[node])
        assert paired.count == sent.count, (
            f"n{node} logged {sent.count} prerequisite sends but "
            f"{paired.count} completed pairs"
        )
        ctx.assert_log_order([send_pattern, pair_pattern], nodes=[node])

        staged = ctx.assert_log(staged_pattern, nodes=[node])
        matched = ctx.assert_log(matched_pattern, nodes=[node])
        assert matched.count <= staged.count, (
            f"n{node} matched {matched.count} candidates after staging only "
            f"{staged.count}"
        )
        ctx.assert_log(validation_pattern, nodes=[node])
        ctx.assert_log_order(
            [
                staged_pattern,
                matched_pattern,
                validation_pattern,
            ],
            nodes=[node],
        )

    # n0 originates the sole listed validator's manifest and already owns it.
    # The relay and observer must each admit that manifest; other valid pairs
    # may remain ephemeral, so matched and durably processed counts are not
    # expected to be equal.
    for node in [1, 2]:
        processed = ctx.assert_log(processed_pattern, nodes=[node])
        matched = ctx.assert_log(matched_pattern, nodes=[node])
        assert processed.count <= matched.count, (
            f"n{node} processed {processed.count} manifests after matching "
            f"only {matched.count} candidates"
        )
        ctx.assert_log_order(
            [staged_pattern, matched_pattern, processed_pattern], nodes=[node]
        )

    log(
        "PASS: every live peer enqueued manifest before validation and every "
        "receiver staged and matched its one candidate, then applied it only "
        "after the associated validation passed signature verification"
    )
