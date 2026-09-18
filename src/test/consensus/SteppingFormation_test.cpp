//------------------------------------------------------------------------------
// SteppingFormation — §5.6: network FORMATION as a first-class steppable
// scenario, instead of the pre-baked `validators(n).mesh()` gloss.
//
// Nodes come up UNCONNECTED and the topology forms ON THE TIMELINE: links are
// injected at chosen virtual instants (the generic at()/in() combinator over
// connect()), while the test walks time beat by beat (tick()) watching what a
// real network does while forming — peer counts grow, each node's operating
// mode walks DISCONNECTED → CONNECTED → ... → FULL, no ledger validates before
// quorum connectivity exists, and the whole formation timeline replays
// bit-identically.
//
// Known modeling boundary (by design, SimOverlay.h): the peer handshake itself
// is atomic — real makeResponse/verifyHandshake, not byte-streamed — so
// formation is steppable at link granularity, not handshake-byte granularity.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>

#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingFormation_test : public beast::unit_test::suite
{
    struct FormationOutcome
    {
        std::size_t beatsToConverge = 0;
        std::vector<uint256> chain;  // validated hashes [2 .. target]
    };

    // Bring up 3 validators with NO links, prove the disconnected state is
    // inert, form the mesh link-by-link on the timeline, then walk beats until
    // the formed network validates `target`. Assertions run inside so both
    // callers get the full formation story checked.
    std::optional<FormationOutcome>
    runFormation(std::uint32_t target)
    {
        using namespace std::chrono;

        SteppingNetwork net(*this);
        net.validators(3);  // deliberately NOT .mesh()
        if (!BEAST_EXPECT(net.allUp()))
            return std::nullopt;

        // Disconnected phase: heartbeats fire but the peer gate holds — no
        // peers, DISCONNECTED mode everywhere, nothing can validate.
        for (int i = 0; i < 3; ++i)
            net.tick();
        BEAST_EXPECT(net.peerCountIs(0));
        for (std::uint32_t n = 0; n < 3; ++n)
            BEAST_EXPECT(net.mode(n) == OperatingMode::DISCONNECTED);
        BEAST_EXPECT(net.minValidatedSeq() == 0);

        // Form the topology ON the timeline: one link per virtual second.
        net.in(seconds{1}, 0, [&]() { net.connect(0, 1); });
        net.in(seconds{2}, 1, [&]() { net.connect(1, 2); });
        net.in(seconds{3}, 2, [&]() { net.connect(0, 2); });

        // Walk beats until the formed network converges. Peer counts and
        // validated seq may only grow between beats.
        constexpr std::size_t kMaxBeats = 80;
        FormationOutcome out;
        std::uint32_t lastValidated = 0;
        while (net.minValidatedSeq() < target &&
               out.beatsToConverge < kMaxBeats)
        {
            net.tick();
            ++out.beatsToConverge;
            auto const v = net.minValidatedSeq();
            BEAST_EXPECT(v >= lastValidated);
            lastValidated = v;
        }
        if (!net.expectConverged(target))
            return std::nullopt;

        // The formed network is a full mesh of FULL validators.
        BEAST_EXPECT(net.peerCountIs(2));
        for (std::uint32_t n = 0; n < 3; ++n)
            BEAST_EXPECT(net.mode(n) == OperatingMode::FULL);

        for (std::uint32_t seq = 2; seq <= target; ++seq)
        {
            auto const h = net.ledgerHash(0, seq);
            BEAST_EXPECT(h != uint256{});
            out.chain.push_back(h);
        }
        return out;
    }

    void
    testStagedFormation()
    {
        testcase(
            "staged formation: unconnected validators are inert, links form "
            "on the timeline, modes walk to FULL, network converges");
        auto const out = runFormation(/*target=*/3);
        if (out)
            log << "  converged " << out->beatsToConverge
                << " beats after formation began" << std::endl;
        BEAST_EXPECT(out.has_value());
    }

    void
    testSpawnLateJoiner()
    {
        testcase(
            "identities()/spawn() unweld: the 5th validator is CREATED "
            "mid-scenario (not restarted — it never existed) and catches up");
        SteppingNetwork net(*this);
        // Five identities fixed up front — every UNL lists all five — but
        // only four nodes exist. quorum(5) = 4, so the four validate alone.
        net.identities(5).spawn({0, 1, 2, 3});
        if (!BEAST_EXPECT(net.allUp()))
            return;
        for (std::uint32_t a = 0; a < 4; ++a)
            for (std::uint32_t b = a + 1; b < 4; ++b)
                if (!BEAST_EXPECT(net.connect(a, b) != nullptr))
                    return;
        net.runOnly({0, 1, 2, 3}, /*target=*/4);
        for (std::uint32_t n = 0; n < 4; ++n)
            if (!BEAST_EXPECT(net.validSeq(n) >= 4))
                return;
        BEAST_EXPECT(net.raw().size() == 4);  // node 4 does not exist yet

        net.spawn({4});  // brought into EXISTENCE mid-scenario
        if (!BEAST_EXPECT(net.isLive(4)))
            return;
        for (std::uint32_t peer = 0; peer < 4; ++peer)
            if (!BEAST_EXPECT(net.connect(4, peer) != nullptr))
                return;
        auto const target = net.validSeq(0) + 3;
        net.runTo(target);
        if (!net.expectConverged(target))
            return;
        BEAST_EXPECT(net.mode(4) == OperatingMode::FULL);
        log << "  spawned-late validator caught up to " << net.validSeq(4)
            << std::endl;
    }

    void
    testFormationReplays()
    {
        testcase("the formation timeline replays identically");
        auto const run1 = runFormation(3);
        auto const run2 = runFormation(3);
        if (!BEAST_EXPECT(run1 && run2))
            return;
        BEAST_EXPECT(run1->beatsToConverge == run2->beatsToConverge);
        BEAST_EXPECT(run1->chain == run2->chain);
        log << "  formation chains "
            << (run1->chain == run2->chain ? "MATCH" : "DIFFER") << std::endl;
    }

public:
    void
    run() override
    {
        testStagedFormation();
        testSpawnLateJoiner();
        testFormationReplays();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingFormation, consensus, ripple);

}  // namespace ripple::test
