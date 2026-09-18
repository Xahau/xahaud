//------------------------------------------------------------------------------
// SteppingLargeNet — the larger-N late joiner (§5.6 "larger N"), and the
// regression gate for the injected-PRNG seam (plan §7.11).
//
// WHY N=8: the acquire machinery only starts making RANDOM CHOICES once the
// joiner has more peers than the selection limits —
//   - InboundLedger::addPeers asks kPeerCountStart(5) peers, ranked by
//     PeerImp::getScore, whose tie-breaker is a PRNG draw: with 7 peers this
//     is a genuine 5-of-7 subset chosen by the engine;
//   - SHAMapSync's missing-nodes walk randomizes its branch order per level
//     (Family::prng), shaping the CONTENT of TMGetLedger requests.
// Both draw from the node's injected engine, so the whole join must replay
// bit-for-bit — asserted through expectReplays (SteppingReplay.h), whose
// in-process grind is ALSO the cross-run-leak detector: this suite's first
// cold/warm comparison found the MemoryFactory static-nodestore oracle
// (design-notes §7).
//
// Quorum arithmetic: quorum(8) = ⌈0.8·8⌉ = 7, so the 7 connected validators
// keep validating (exactly at quorum) while node 7 sits dark.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpld/app/misc/NetworkOPs.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingLargeNet_test : public beast::unit_test::suite
{
    static constexpr std::uint32_t kValidators = 8;
    static constexpr std::uint32_t kJoiner = kValidators - 1;

    // Build the 7-of-8 quorum, leave node 7 dark, join it through the
    // subset-sampled acquire, and return the joiner's validated hash chain.
    // Internal expectations guard the scenario's own semantics; cross-run
    // identity is expectReplays's job.
    std::optional<std::vector<uint256>>
    runJoin(
        SteppingNetwork& net,
        std::optional<std::uint64_t> prngSeedBase = std::nullopt)
    {
        if (prngSeedBase)
            net.seedPrng(*prngSeedBase);
        net.validators(kValidators);
        if (!BEAST_EXPECT(net.allUp()))
            return std::nullopt;

        // Mesh only 0..6 (exact quorum without the joiner).
        for (std::uint32_t a = 0; a < kJoiner; ++a)
            for (std::uint32_t b = a + 1; b < kJoiner; ++b)
                if (!BEAST_EXPECT(net.connect(a, b) != nullptr))
                    return std::nullopt;

        // Phase 1: the quorum advances; the joiner sits dark at genesis.
        net.runOnly({0, 1, 2, 3, 4, 5, 6}, /*target=*/5);
        for (std::uint32_t n = 0; n < kJoiner; ++n)
        {
            if (!BEAST_EXPECT(net.validSeq(n) >= 5))
            {
                log << "  quorum stalled; diagnostics: "
                    << net.jobDiagnostics() << std::endl;
                return std::nullopt;
            }
        }
        BEAST_EXPECT(net.validSeq(kJoiner) == 0);
        BEAST_EXPECT(net.mode(kJoiner) == OperatingMode::DISCONNECTED);

        auto const joinedAt = net.validSeq(0);

        // Phase 2: the joiner connects to ALL 7 peers — more than
        // kPeerCountStart, so the acquire fan-out must CHOOSE peers.
        for (std::uint32_t peer = 0; peer < kJoiner; ++peer)
            if (!BEAST_EXPECT(net.connect(kJoiner, peer) != nullptr))
                return std::nullopt;

        auto const target = joinedAt + 3;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        BEAST_EXPECT(net.validSeq(kJoiner) >= target);
        BEAST_EXPECT(net.mode(kJoiner) == OperatingMode::FULL);
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);

        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            chain.push_back(net.ledgerHash(kJoiner, seq));
        return chain;
    }

    void
    testLargeJoinReplays()
    {
        testcase(
            "an 8th validator joins a 7-of-8 quorum network through a "
            "subset-sampled acquire; the whole timeline replays (grind "
            "with --unittest-arg=replays=N)");
        // Must hold from a COLD process — run 1 is deliberately part of the
        // grind (the cold/warm comparison is the cross-run-leak detector).
        expectReplays(*this, "N=8 join", [this](SteppingNetwork& net) {
            return runJoin(net);
        });
    }

    void
    testSeedSweep()
    {
        testcase(
            "the scenario converges under different prng seeds (seedPrng "
            "sweep), invariants intact at each — and the subsets genuinely "
            "differ by seed");
        std::vector<std::uint64_t> prints;
        for (std::uint64_t seed : {std::uint64_t{11}, std::uint64_t{22}})
        {
            SteppingNetwork net(*this);
            net.recordForensics();
            auto const chain = runJoin(net, seed);
            if (!BEAST_EXPECT(chain.has_value()))
                return;
            prints.push_back(net.traceFingerprint());
            log << "  seed " << seed << ": trace fingerprint 0x" << std::hex
                << prints.back() << std::dec << std::endl;
        }
        // The seeds must actually STEER the run: equal fingerprints would
        // mean the injected engines no longer reach any decision — the exact
        // signature of the pre-fix warm-nodestore oracle, where "acquires"
        // were cache hits and seeds were inert (design-notes §7). If a
        // legitimate change makes this pair collide, pick different seeds
        // in the same commit. (Codex review 2026-07-03: the sweep logged
        // fingerprints but never asserted divergence.)
        BEAST_EXPECT(prints.size() == 2 && prints[0] != prints[1]);
    }

public:
    void
    run() override
    {
        testLargeJoinReplays();
        testSeedSweep();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingLargeNet, consensus, ripple);

}  // namespace ripple::test
