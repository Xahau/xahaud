//------------------------------------------------------------------------------
// SteppingTraffic — seeded tx traffic over the strict stepping harness.
//
// This is the first consumer of traffic::Generator: it proves the helper can
// own per-account sequences across source reuse in a burst, feed real signed
// XRP payments through the local submission path, and replay the whole
// tx-bearing timeline bit-for-bit.
//------------------------------------------------------------------------------
#include <test/consensus/goldens/donor.h>
#include <test/jtx/SteppingReplay.h>
#include <test/jtx/Traffic.h>
#include <test/jtx/amount.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TER.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace ripple::test {

class SteppingTraffic_test : public beast::unit_test::suite
{
    using Payload = std::optional<std::vector<uint256>>;

    static constexpr auto kTrafficSeed = goldens::donor::kTrafficSeed;
    static constexpr auto kTrafficFingerprint =
        goldens::donor::kTrafficFingerprint;
    static constexpr auto kTrafficEvents = goldens::donor::kTrafficEvents;
    static constexpr auto kTrafficPayloadFingerprint =
        goldens::donor::kTrafficPayloadFingerprint;

    static std::uint64_t
    payloadFingerprint(std::vector<uint256> const& payload)
    {
        std::uint64_t h = 1469598103934665603ull;
        for (auto const& v : payload)
            for (auto const byte : v)
            {
                h ^= byte;
                h *= 1099511628211ull;
            }
        return h;
    }

    bool
    expectRecordCoherence(std::vector<traffic::TrafficTx> const& records)
    {
        bool ok = true;
        std::set<uint256> ids;
        std::map<AccountID, std::uint32_t> lastSeq;

        for (auto const& r : records)
        {
            auto const stx = r.tx->getSTransaction();
            if (!BEAST_EXPECT(stx != nullptr))
            {
                ok = false;
                continue;
            }
            ok = BEAST_EXPECT(r.src.id() != r.dst.id()) && ok;
            ok = BEAST_EXPECT(ids.insert(r.id).second) && ok;
            ok = BEAST_EXPECT(r.tx->getID() == r.id) && ok;
            ok = BEAST_EXPECT(stx->getTransactionID() == r.id) && ok;
            ok = BEAST_EXPECT(stx->getAccountID(sfAccount) == r.src.id()) && ok;
            ok = BEAST_EXPECT(stx->getAccountID(sfDestination) == r.dst.id()) &&
                ok;
            ok = BEAST_EXPECT(stx->getFieldU32(sfSequence) == r.sequence) && ok;
            ok = BEAST_EXPECT(
                     stx->getFieldAmount(sfAmount) ==
                     jtx::PrettyAmount{r.amount}.value()) &&
                ok;

            auto const [it, inserted] = lastSeq.emplace(r.src.id(), r.sequence);
            if (!inserted)
            {
                ok = BEAST_EXPECT(r.sequence == it->second + 1) && ok;
                it->second = r.sequence;
            }
        }
        return ok;
    }

    bool
    expectAllValidated(
        SteppingNetwork& net,
        std::vector<traffic::TrafficTx> const& records,
        std::uint32_t target)
    {
        bool ok = true;
        std::set<uint256> remaining;
        for (auto const& r : records)
            remaining.insert(r.id);

        for (std::uint32_t seq = 2; seq <= target; ++seq)
            for (auto const& tx : net.appliedTxs(0, seq))
                if (remaining.erase(tx.txid) != 0)
                    ok = BEAST_EXPECT(tx.result == tesSUCCESS) && ok;

        if (!remaining.empty())
            log << "  traffic: " << remaining.size()
                << " generated transactions not validated by seq " << target
                << std::endl;
        return BEAST_EXPECT(remaining.empty()) && ok;
    }

    Payload
    runTraffic(SteppingNetwork& net)
    {
        using namespace jtx;

        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return Payload{};

        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return Payload{};

        auto accounts = traffic::makeAccounts(8);
        traffic::fundStepping(net, 0, accounts, XRP(1000));
        traffic::Generator gen(
            net.multiNode(), 0, std::move(accounts), kTrafficSeed);
        BEAST_EXPECT(gen.size() == 8);

        std::vector<traffic::TrafficTx> records;
        for (std::uint32_t beat = 0; beat < 10; ++beat)
        {
            auto burst = gen.burst(
                net.multiNode(),
                0,
                /*count=*/5,
                XRPAmount{10},
                XRPAmount{1'000'000});
            records.insert(records.end(), burst.begin(), burst.end());
            net.tick();
        }
        BEAST_EXPECT(records.size() == 50);

        auto const target = net.minValidatedSeq() + 5;
        auto const steps = net.runTo(target);
        log << "  traffic: " << records.size() << " payments, " << steps
            << " scheduler events to minValidated=" << net.minValidatedSeq()
            << " (target " << target
            << "), offThreadJobs=" << net.offThreadJobs()
            << ", failedJobs=" << net.failedJobs() << std::endl;

        if (!net.expectConverged(target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return Payload{};
        }
        if (!BEAST_EXPECT(net.validatedForkFree()))
            return Payload{};
        if (!expectRecordCoherence(records))
            return Payload{};
        if (!expectAllValidated(net, records, target))
            return Payload{};

        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        for (auto const& r : records)
        {
            payload.push_back(r.id);
            payload.push_back(uint256{r.sequence});
        }
        return Payload{std::move(payload)};
    }

    void
    testTrafficConvergesAndPinsTimeline()
    {
        testcase("seeded burst traffic converges and matches pinned timeline");
        SteppingNetwork net(*this);
        net.recordForensics();
        auto const payload = runTraffic(net);
        if (!BEAST_EXPECT(payload.has_value()))
            return;

        log << "  traffic observed: fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec << ", " << net.traceCount()
            << " events" << std::endl;
        auto const content = payloadFingerprint(*payload);
        log << "  traffic content observed: fingerprint 0x" << std::hex
            << content << std::dec << std::endl;
        if (goldens::donor::goldensPrint(*this))
        {
            goldens::donor::printScalar(
                *this, "kTrafficFingerprint", net.traceFingerprint());
            goldens::donor::printScalar(
                *this, "kTrafficEvents", net.traceCount());
            goldens::donor::printScalar(
                *this, "kTrafficPayloadFingerprint", content);
        }
        BEAST_EXPECT(net.traceFingerprint() == kTrafficFingerprint);
        BEAST_EXPECT(net.traceCount() == kTrafficEvents);
        BEAST_EXPECT(content == kTrafficPayloadFingerprint);
    }

    void
    testTrafficReplays()
    {
        testcase(
            "seeded burst traffic replays bit-for-bit (grind with "
            "--unittest-arg=replays=N)");
        expectReplays(*this, "seeded traffic", [this](SteppingNetwork& net) {
            return runTraffic(net);
        });
    }

public:
    void
    run() override
    {
        testTrafficConvergesAndPinsTimeline();
        testTrafficReplays();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingTraffic, consensus, ripple);

}  // namespace ripple::test
