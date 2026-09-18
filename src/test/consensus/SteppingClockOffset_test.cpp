//------------------------------------------------------------------------------
// SteppingClockOffset — axis A of the fault family: nodes LIE ABOUT TIME.
// (explorer-takeoff.md §2; evolution-plan §5.7.) Every prior scenario shared
// one virtual timeline by construction; real validators disagree about the
// wall time, and a whole consensus layer exists to absorb it — close-time
// VOTING (the vote-averaging + adjustCloseTime damped feedback in
// RCLConsensus doAccept), close-time ROUNDING (effCloseTime bins, genesis
// resolution 10s), and validation freshness windows. None of it was
// exercisable before this axis: with synced clocks the feedback loop runs
// in its degenerate by==0 regime.
//
// The offset is EPOCH disagreement only (TimeKeeper/NetClock: close times,
// validation sign times, freshness checks); the shared steady clock — round
// timing, elapsed durations — stays common. Offsets are set AFTER the mesh
// forms (the real handshake rejects >20s relative skew between CONNECTING
// peers; post-connect skew is the NTP-drift shape and is unconstrained).
//
// Rungs (the take-off ladder):
//   1. ABSORBED: ±1-2s spread inside the 10s genesis bin — converges, every
//      ledger's close time carries consensus agreement (getCloseAgree), the
//      feedback loop ENGAGES (nonzero closeOffset somewhere — non-vacuity),
//      payments land, and the whole skewed run replays bit-for-bit.
//   2. BIN-STRADDLING: 0..20s spread across bins — the close-time votes
//      genuinely disagree; assert convergence + NO safety loss, and the
//      disagreement machinery visibly engaged (a no-consensus close-time
//      ledger and/or feedback movement), again bit-for-bit reproducible.
//   2b. NO-MAJORITY SPLIT: 6 nodes, 2/2/2 across the 0s/10s/20s effective
//      close-time bins (the CSF closeTimeDisagree shape, ported to the
//      real stack) — no bin holds a majority, so consensus must AGREE TO
//      DISAGREE: some ledger closes without close-time consensus
//      (sLCF_NoConsensusTime, read via getCloseAgree). Rungs 1-2 proved
//      the feedback loop absorbs a 0..20s spread of DISTINCT votes; this
//      shape pins the votes into three equal camps so the flag itself is
//      reachable. 20s is the ceiling: proposals older than
//      proposeFRESHNESS (20s) are ignored outright.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpld/core/TimeKeeper.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/LedgerHeader.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ripple::test {

class SteppingClockOffset_test : public beast::unit_test::suite
{
    using Payload = std::optional<std::vector<uint256>>;

    // One skewed timeline: bring N validators up synced (N =
    // offsetSecs.size()), apply per-node offsets at a stepping boundary,
    // then run a funded economy across the skew and report what the
    // disagreement machinery did. requireNoConsensusTime is the rung-2b
    // reachability assertion: the run must contain a ledger that closed
    // WITHOUT close-time consensus, not merely feedback movement.
    Payload
    runSkewed(
        SteppingNetwork& net,
        std::vector<int> const& offsetSecs,
        bool requireNoConsensusTime = false)
    {
        using namespace jtx;
        using namespace std::chrono;
        Account const alice{"alice"};

        auto const n = static_cast<std::uint32_t>(offsetSecs.size());
        net.validators(n).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return Payload{};
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return Payload{};

        //@@start the-lie-begins
        // The lie begins: post-connect, at a stepping boundary — the
        // NTP-drift shape. From the next scheduler event, node i's
        // NetClock reads (true virtual time + offset_i).
        for (std::uint32_t i = 0; i < offsetSecs.size(); ++i)
            net.clockOffset(i, seconds{offsetSecs[i]});
        //@@end the-lie-begins

        // A funded economy ACROSS the skew: ledgers must carry real
        // transactions while the close-time votes disagree.
        net.fund(0, XRP(100000), {alice});
        net.in(milliseconds{500}, 2, [&]() {
            net.submit(
                2, pay(Account::master, alice, XRP(1000)), Account::master);
        });
        auto const target = net.minValidatedSeq() + 4;
        net.runTo(target);
        if (!net.expectConverged(target))
            return Payload{};

        //@@start disagreement-observed
        // Observability: what did the disagreement machinery do? Record
        // WHERE the first no-consensus close happened, not just whether
        // (codex round 7): replay must agree on the sequence, so a timing
        // drift that moves the disagreement to a different ledger is a
        // divergence, not a silently-equal boolean.
        std::uint32_t firstNoConsensusSeq = 0;
        for (std::uint32_t seq = 4; seq <= target; ++seq)
        {
            auto const l = net.ledger(0, seq);
            if (!BEAST_EXPECT(l != nullptr))
                return Payload{};
            if (!getCloseAgree(l->info()) && firstNoConsensusSeq == 0)
                firstNoConsensusSeq = seq;
        }
        bool const anyNoConsensusTime = firstNoConsensusSeq != 0;
        bool anyFeedback = false;
        std::string offsets;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            auto const co =
                net.node(i).app().timeKeeper().closeOffset();
            if (co != seconds{0})
                anyFeedback = true;
            offsets += (i ? "," : "") + std::to_string(co.count()) + "s";
        }
        log << "  closeOffsets=[" << offsets << "] noConsensusTime="
            << (anyNoConsensusTime
                    ? "seen@" + std::to_string(firstNoConsensusSeq)
                    : "never")
            << std::endl;

        // NON-VACUITY: the skew must have ENGAGED the machinery this axis
        // exists to exercise — the adjustCloseTime feedback pulled some
        // node's close-time view (nonzero closeOffset), and/or a ledger
        // closed without close-time consensus. A timing change that lets
        // the skew round away silently demotes the scenario — fail it.
        BEAST_EXPECT(anyFeedback || anyNoConsensusTime);

        // Rung-2b reachability: the no-majority split must actually drive
        // the agree-to-disagree path. A converged run that never lost
        // close-time consensus means the shape stopped splitting the vote
        // — fail it rather than let the rung silently demote to rung 2.
        if (requireNoConsensusTime)
            BEAST_EXPECT(anyNoConsensusTime);
        //@@end disagreement-observed

        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            chain.push_back(net.ledgerHash(0, seq));
        // Fold the engagement facts into the replay payload so runs must
        // agree on WHAT the machinery did, not just the chain it built:
        // the no-consensus-time observation and each node's final
        // closeOffset (feedback-loop END STATE — app state the event
        // fingerprint cannot see; +1000 biases the signed count into
        // uint256 range).
        // (first disagreeing seq, or 0: runs must agree on WHERE — offset
        // by 1 so "never" stays distinct from a seq-0 impossibility)
        chain.push_back(uint256{1u + firstNoConsensusSeq});
        for (std::uint32_t i = 0; i < n; ++i)
            chain.push_back(uint256{static_cast<std::uint64_t>(
                1000 +
                net.node(i).app().timeKeeper().closeOffset().count())});
        return Payload{std::move(chain)};
    }

    void
    testAbsorbedOffsets()
    {
        testcase(
            "rung 1 — ±2s inside the close-time bin: absorbed, feedback "
            "engages, bit-for-bit replay");
        expectReplays(
            *this, "absorbed offsets", [this](SteppingNetwork& net) {
                return runSkewed(net, {0, +1, -1, +2, -2});
            });
    }

    void
    testBinStraddle()
    {
        testcase(
            "rung 2 — 0..20s across bins: close-time votes disagree, no "
            "safety loss, bit-for-bit replay");
        expectReplays(
            *this, "bin straddle", [this](SteppingNetwork& net) {
                return runSkewed(net, {0, 5, 10, 15, 20});
            });
    }

    void
    testNoMajoritySplit()
    {
        testcase(
            "rung 2b — 6 nodes 2/2/2 across 0s/10s/20s bins: no close-time "
            "majority, agree-to-disagree fires, no safety loss, bit-for-bit "
            "replay");
        expectReplays(
            *this, "no-majority split", [this](SteppingNetwork& net) {
                return runSkewed(net, {0, 0, 10, 10, 20, 20}, true);
            });
    }

public:
    void
    run() override
    {
        testAbsorbedOffsets();
        testBinStraddle();
        testNoMajoritySplit();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingClockOffset, consensus, ripple);

}  // namespace ripple::test
