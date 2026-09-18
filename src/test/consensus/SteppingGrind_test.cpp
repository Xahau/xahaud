//------------------------------------------------------------------------------
// SteppingGrind — the determinism SOAK, as a MANUAL suite (excluded from
// --unittest, like `compression`; it silently doesn't run unless named):
//
//     xrpld --unittest=SteppingGrind [--unittest-arg=replays=N]
//
// Runs the key scenarios through expectReplays at K=30 (arg can push
// higher). Why deep grind is not mere repetition: the process-global peer
// id/port ranges SHIFT with every in-process run, so container-layout
// effects (the class behind several §7.11 finds) only manifest at
// particular range positions — each extra run sweeps another position.
// Run 1 vs 2 is cold/warm (the MemoryFactory-oracle signature); 2 vs 3 is
// progressive drift; 4..K is the id-range sweep plus tail-risk for any
// reintroduced wall-clock race.
//
// Expected wall time at K=30 on a quiet machine: roughly 7–10 minutes
// (N=3 ≈ 2s/run, N=8 join ≈ 7s/run, faulted N=5 ≈ 5s/run).
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>

#include <ripple.pb.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingGrind_test : public beast::unit_test::suite
{
    static constexpr int kGrindRuns = 30;
    using Payload = std::optional<std::vector<uint256>>;

    void
    grindConvergence()
    {
        testcase("grind: N=3 mesh convergence");
        expectReplays(
            *this,
            "N=3 convergence x30",
            [this](SteppingNetwork& net) {
                net.validators(3).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};
                net.runTo(4);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= 4))
                    return Payload{};
                BEAST_EXPECT(net.failedJobs() == 0);
                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= 4; ++seq)
                    chain.push_back(net.ledgerHash(0, seq));
                return Payload{std::move(chain)};
            },
            kGrindRuns);
    }

    void
    grindLargeJoin()
    {
        testcase("grind: N=8 subset-sampled late join");
        expectReplays(
            *this,
            "N=8 join x30",
            [this](SteppingNetwork& net) {
                net.validators(8);
                if (!BEAST_EXPECT(net.allUp()))
                    return Payload{};
                for (std::uint32_t a = 0; a < 7; ++a)
                    for (std::uint32_t b = a + 1; b < 7; ++b)
                        if (!BEAST_EXPECT(net.connect(a, b) != nullptr))
                            return Payload{};
                net.runOnly({0, 1, 2, 3, 4, 5, 6}, /*target=*/5);
                auto const joinedAt = net.validSeq(0);
                if (!BEAST_EXPECT(joinedAt >= 5))
                    return Payload{};
                for (std::uint32_t peer = 0; peer < 7; ++peer)
                    if (!BEAST_EXPECT(net.connect(7, peer) != nullptr))
                        return Payload{};
                auto const target = joinedAt + 3;
                net.runTo(target);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
                    return Payload{};
                BEAST_EXPECT(net.mode(7) == OperatingMode::FULL);
                BEAST_EXPECT(net.failedJobs() == 0);
                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= target; ++seq)
                    chain.push_back(net.ledgerHash(7, seq));
                return Payload{std::move(chain)};
            },
            kGrindRuns);
    }

    void
    grindSeededLoss()
    {
        testcase("grind: N=5 under 10% seeded random loss");
        auto engines = std::make_shared<
            std::vector<std::unique_ptr<beast::xor_shift_engine>>>();
        expectReplays(
            *this,
            "10% loss x30",
            [this, engines](SteppingNetwork& net) {
                net.validators(5).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};
                net.runTo(3);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                    return Payload{};
                for (std::uint32_t to = 1; to < 5; ++to)
                {
                    engines->push_back(
                        std::make_unique<beast::xor_shift_engine>(
                            0xF00D5EED0000ull + to));
                    net.faultLink(
                        0,
                        to,
                        simfaults::dropWithProbability(*engines->back(), 0.10));
                }
                auto const target = net.minValidatedSeq() + 3;
                net.runTo(target);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
                    return Payload{};
                BEAST_EXPECT(net.failedJobs() == 0);
                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= target; ++seq)
                    chain.push_back(net.ledgerHash(0, seq));
                return Payload{std::move(chain)};
            },
            kGrindRuns);
    }

public:
    void
    run() override
    {
        grindConvergence();
        grindLargeJoin();
        grindSeededLoss();
    }
};

BEAST_DEFINE_TESTSUITE_MANUAL(SteppingGrind, consensus, ripple);

}  // namespace ripple::test
