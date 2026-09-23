//------------------------------------------------------------------------------
// SteppingTrust -- per-node UNL shapes in the real stepping harness.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/TER.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingTrust_test : public beast::unit_test::suite
{
    static constexpr std::uint32_t kForkPeers = 10;

    struct KProfiledForkCell
    {
        std::uint32_t overlap = 0;
        std::uint32_t k = 0;
        std::uint64_t fingerprint = 0;
        std::uint64_t events = 0;
        std::uint64_t weightedEvents = 0;
        std::size_t steps = 0;
        std::size_t beats = 0;
        std::uint32_t minValidated = 0;
        std::uint32_t maxValidated = 0;
        std::uint32_t forkCheckedSeqs = 0;
        std::uint32_t target = 0;
        std::uint32_t divergentSeq = 0;
        std::uint32_t agreedSeq = 0;
        std::uint32_t txASeq = 0;
        std::uint32_t txBSeq = 0;
        std::uint64_t clampHits = 0;
        std::int64_t requestedMs = 0;
        std::int64_t consumedMs = 0;
        std::int64_t maxConsumedBeatMs = 0;
        std::int64_t schedulerMs = 0;
        std::uint64_t heartbeatEvents = 0;
        std::uint64_t deliverEvents = 0;
        std::uint64_t jobEvents = 0;
        std::uint64_t timerEvents = 0;
        std::uint32_t firstClampWeight = 0;
        bool submittedA = false;
        bool submittedB = false;
        bool forkFree = false;
        bool forked = false;
        bool safeResolved = false;
        bool unresolved = false;
        bool saturated = false;
        std::int64_t unitCostMs = 5;

        [[nodiscard]] bool
        operator==(KProfiledForkCell const& o) const
        {
            return overlap == o.overlap && k == o.k &&
                fingerprint == o.fingerprint && events == o.events &&
                weightedEvents == o.weightedEvents && steps == o.steps &&
                beats == o.beats && minValidated == o.minValidated &&
                maxValidated == o.maxValidated &&
                forkCheckedSeqs == o.forkCheckedSeqs && target == o.target &&
                divergentSeq == o.divergentSeq && agreedSeq == o.agreedSeq &&
                txASeq == o.txASeq && txBSeq == o.txBSeq &&
                clampHits == o.clampHits && requestedMs == o.requestedMs &&
                consumedMs == o.consumedMs &&
                maxConsumedBeatMs == o.maxConsumedBeatMs &&
                schedulerMs == o.schedulerMs &&
                heartbeatEvents == o.heartbeatEvents &&
                deliverEvents == o.deliverEvents && jobEvents == o.jobEvents &&
                timerEvents == o.timerEvents &&
                firstClampWeight == o.firstClampWeight &&
                submittedA == o.submittedA && submittedB == o.submittedB &&
                forkFree == o.forkFree && forked == o.forked &&
                safeResolved == o.safeResolved && unresolved == o.unresolved &&
                saturated == o.saturated && unitCostMs == o.unitCostMs;
        }
    };

    struct ForkShape
    {
        std::vector<std::uint32_t> aOnly;
        std::vector<std::uint32_t> bOnly;
        std::vector<std::uint32_t> common;
        std::vector<std::uint32_t> a;
        std::vector<std::uint32_t> b;
        std::vector<std::uint32_t> all;
    };

    struct WrongLclShape
    {
        std::vector<std::uint32_t> minority{0, 1};
        std::vector<std::uint32_t> majorityA{2, 3, 4};
        std::vector<std::uint32_t> majorityB{5, 6, 7, 8, 9};
        std::vector<std::uint32_t> small{0, 1, 2, 3, 4};
        std::vector<std::uint32_t> majority{2, 3, 4, 5, 6, 7, 8, 9};
        std::vector<std::uint32_t> all{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    };

    [[nodiscard]] static bool
    contains(std::vector<std::uint32_t> const& xs, std::uint32_t x)
    {
        return std::find(xs.begin(), xs.end(), x) != xs.end();
    }

    [[nodiscard]] static ForkShape
    forkShape(std::uint32_t overlap)
    {
        ForkShape s;
        auto const numA = (kForkPeers - overlap) / 2;
        auto const numB = kForkPeers - numA - overlap;
        std::uint32_t id = 0;
        for (; id < numA; ++id)
            s.aOnly.push_back(id);
        for (std::uint32_t i = 0; i < numB; ++i)
            s.bOnly.push_back(id++);
        for (std::uint32_t i = 0; i < overlap; ++i)
            s.common.push_back(id++);
        s.a = s.aOnly;
        s.a.insert(s.a.end(), s.common.begin(), s.common.end());
        s.b = s.bOnly;
        s.b.insert(s.b.end(), s.common.begin(), s.common.end());
        for (std::uint32_t i = 0; i < kForkPeers; ++i)
            s.all.push_back(i);
        return s;
    }

    [[nodiscard]] bool
    configureForkTopology(SteppingNetwork& net, ForkShape const& shape)
    {
        net.identities(kForkPeers);
        for (std::uint32_t id = 0; id < kForkPeers; ++id)
        {
            if (contains(shape.aOnly, id))
                net.trust(id, shape.a);
            else if (contains(shape.bOnly, id))
                net.trust(id, shape.b);
            else
                net.trust(id, shape.all);
        }
        net.spawnAll();

        std::vector<std::size_t> expected(kForkPeers);
        for (std::uint32_t i = 0; i < kForkPeers; ++i)
        {
            for (std::uint32_t j = i + 1; j < kForkPeers; ++j)
            {
                auto const inA = contains(shape.a, i) && contains(shape.a, j);
                auto const inB = contains(shape.b, i) && contains(shape.b, j);
                if (!inA && !inB)
                    continue;
                if (!BEAST_EXPECT(net.connect(i, j) != nullptr))
                    return false;
                ++expected[i];
                ++expected[j];
            }
        }

        if (!BEAST_EXPECT(net.allUp()))
            return false;
        return BEAST_EXPECT(waitUntil(
            [&]() {
                for (std::uint32_t i = 0; i < kForkPeers; ++i)
                    if (net.node(i).app().overlay().size() != expected[i])
                        return false;
                return true;
            },
            std::chrono::milliseconds{500}));
    }

    [[nodiscard]] bool
    configureWrongLclTopology(SteppingNetwork& net, WrongLclShape const& shape)
    {
        net.identities(kForkPeers);
        for (auto const id : shape.minority)
            net.trust(id, shape.small);
        for (auto const id : shape.majority)
            net.trust(id, shape.majority);
        net.spawnAll();

        std::vector<std::size_t> expected(kForkPeers);
        for (std::uint32_t i = 0; i < kForkPeers; ++i)
        {
            for (std::uint32_t j = i + 1; j < kForkPeers; ++j)
            {
                auto const inSmall =
                    contains(shape.small, i) && contains(shape.small, j);
                auto const inMajority =
                    contains(shape.majority, i) && contains(shape.majority, j);
                if (!inSmall && !inMajority)
                    continue;
                if (!BEAST_EXPECT(net.connect(i, j) != nullptr))
                    return false;
                ++expected[i];
                ++expected[j];
            }
        }

        if (!BEAST_EXPECT(net.allUp()))
            return false;
        return BEAST_EXPECT(waitUntil(
            [&]() {
                for (std::uint32_t i = 0; i < kForkPeers; ++i)
                    if (net.node(i).app().overlay().size() != expected[i])
                        return false;
                return true;
            },
            std::chrono::milliseconds{500}));
    }

    [[nodiscard]] static std::optional<std::uint32_t>
    findAppliedSeq(
        SteppingNetwork& net,
        std::uint32_t node,
        std::uint32_t firstSeq,
        std::uint32_t lastSeq,
        uint256 const& txid,
        TER expected = tesSUCCESS)
    {
        for (auto seq = firstSeq; seq <= lastSeq; ++seq)
            for (auto const& tx : net.appliedTxs(node, seq))
                if (tx.txid == txid && tx.result == expected)
                    return seq;
        return std::nullopt;
    }

    [[nodiscard]] static std::int64_t
    asMs(HarnessScheduler::duration d)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    }

    [[nodiscard]] static std::int64_t
    asMs(HarnessScheduler::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   t.time_since_epoch())
            .count();
    }

    void
    logForkState(
        char const* label,
        SteppingNetwork& net,
        ForkShape const& shape,
        std::uint32_t prior,
        std::optional<uint256> const& txA,
        std::optional<uint256> const& txB)
    {
        log << "  fork-state " << label << ": tips";
        for (std::uint32_t i = 0; i < kForkPeers; ++i)
            log << " n" << i << "(c=" << net.closedSeq(i)
                << ",v=" << net.validSeq(i) << ")";
        log << std::endl;

        for (auto seq = prior + 1; seq <= net.minValidatedSeq(); ++seq)
        {
            auto const hA = net.ledgerHash(shape.a.front(), seq);
            auto const hB = net.ledgerHash(shape.b.front(), seq);
            log << "  fork-state seq=" << seq
                << " allAgree=" << net.validatedAgree(shape.all, seq)
                << " aAgree=" << net.validatedAgree(shape.a, seq)
                << " bAgree=" << net.validatedAgree(shape.b, seq)
                << " aHash=" << to_string(hA).substr(0, 12)
                << " bHash=" << to_string(hB).substr(0, 12);
            if (txA)
                log << " txA@a="
                    << findAppliedSeq(net, shape.a.front(), seq, seq, *txA)
                           .has_value()
                    << " txA@b="
                    << findAppliedSeq(net, shape.b.front(), seq, seq, *txA)
                           .has_value();
            if (txB)
                log << " txB@a="
                    << findAppliedSeq(net, shape.a.front(), seq, seq, *txB)
                           .has_value()
                    << " txB@b="
                    << findAppliedSeq(net, shape.b.front(), seq, seq, *txB)
                           .has_value();
            log << std::endl;
        }
    }

    [[nodiscard]] static std::uint64_t
    countTxnAcquireJobs(SteppingNetwork& net)
    {
        std::uint64_t count = 0;
        for (auto const& ev : net.controller().scheduler().traceLog())
            if (ev.kind == HarnessScheduler::Kind::job &&
                ev.label.find("JtTxnData") != std::string::npos)
                ++count;
        return count;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runForkSafe(SteppingNetwork& net)
    {
        using namespace jtx;
        using namespace std::chrono;

        auto const shape = forkShape(/*overlap=*/6);
        if (!configureForkTopology(net, shape))
            return std::nullopt;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;
        auto const prior = net.minValidatedSeq();

        Account const a{"trust-safe-a"};
        Account const b{"trust-safe-b"};
        std::optional<uint256> txA;
        std::optional<uint256> txB;
        std::optional<TER> resultA;
        std::optional<TER> resultB;
        net.in(milliseconds{500}, shape.aOnly.front(), [&]() {
            auto const tx = net.submit(
                shape.aOnly.front(),
                pay(Account::master, a, XRP(100000)),
                Account::master);
            resultA = tx->getResult();
            txA = tx->getID();
        });
        net.in(milliseconds{500}, shape.bOnly.front(), [&]() {
            auto const tx = net.submit(
                shape.bOnly.front(),
                pay(Account::master, b, XRP(100000)),
                Account::master);
            resultB = tx->getResult();
            txB = tx->getID();
        });

        std::optional<std::uint32_t> target;
        std::optional<std::uint32_t> aSeq;
        std::optional<std::uint32_t> bSeq;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    if (!txA || !txB)
                        return false;
                    for (auto seq = prior + 1; seq <= net.minValidatedSeq();
                         ++seq)
                    {
                        if (!net.validatedAgree(shape.all, seq))
                            continue;
                        aSeq = findAppliedSeq(net, 0, seq, seq, *txA);
                        bSeq = findAppliedSeq(net, 0, seq, seq, *txB);
                        if (aSeq || bSeq)
                        {
                            target = seq;
                            return true;
                        }
                    }
                    return false;
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/80})))
        {
            logForkState("safe", net, shape, prior, txA, txB);
            log << "  local submit A: "
                << (resultA ? transToken(*resultA) : "not-run")
                << ", B: " << (resultB ? transToken(*resultB) : "not-run")
                << std::endl;
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        if (resultA && *resultA != tesSUCCESS)
            log << "  local submit A: " << transToken(*resultA) << std::endl;
        if (resultB && *resultB != tesSUCCESS)
            log << "  local submit B: " << transToken(*resultB) << std::endl;
        if (!BEAST_EXPECT(resultA && *resultA == tesSUCCESS))
            return std::nullopt;
        if (!BEAST_EXPECT(resultB && *resultB == tesSUCCESS))
            return std::nullopt;
        if (!BEAST_EXPECT(target))
            return std::nullopt;
        if (!net.expectConverged(*target))
            return std::nullopt;
        BEAST_EXPECT(net.validatedForkFree());
        BEAST_EXPECT(aSeq.has_value() != bSeq.has_value());

        log << "  fork-safe overlap=6 fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec
            << ", traceCount=" << net.traceCount() << std::endl;
        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= *target; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        payload.push_back(uint256{aSeq ? *aSeq : 0});
        payload.push_back(uint256{bSeq ? *bSeq : 0});
        return payload;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runForkEnvelope(SteppingNetwork& net)
    {
        using namespace jtx;
        using namespace std::chrono;

        auto const shape = forkShape(/*overlap=*/0);
        if (!configureForkTopology(net, shape))
            return std::nullopt;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;
        auto const prior = net.minValidatedSeq();

        Account const a{"trust-envelope-a"};
        Account const b{"trust-envelope-b"};
        std::optional<uint256> txA;
        std::optional<uint256> txB;
        std::optional<TER> resultA;
        std::optional<TER> resultB;
        net.in(milliseconds{500}, shape.aOnly.front(), [&]() {
            auto const tx = net.submit(
                shape.aOnly.front(),
                pay(Account::master, a, XRP(100000)),
                Account::master);
            resultA = tx->getResult();
            txA = tx->getID();
        });
        net.in(milliseconds{500}, shape.bOnly.front(), [&]() {
            auto const tx = net.submit(
                shape.bOnly.front(),
                pay(Account::master, b, XRP(100000)),
                Account::master);
            resultB = tx->getResult();
            txB = tx->getID();
        });

        std::optional<std::uint32_t> target;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    if (!txA || !txB)
                        return false;
                    for (auto seq = prior + 1; seq <= net.minValidatedSeq();
                         ++seq)
                    {
                        if (!net.validatedAgree(shape.a, seq) ||
                            !net.validatedAgree(shape.b, seq))
                            continue;
                        if (net.ledgerHash(shape.a.front(), seq) ==
                            net.ledgerHash(shape.b.front(), seq))
                            continue;
                        if (!findAppliedSeq(
                                net, shape.a.front(), seq, seq, *txA) ||
                            !findAppliedSeq(
                                net, shape.b.front(), seq, seq, *txB))
                            continue;
                        target = seq;
                        return true;
                    }
                    return false;
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/80})))
        {
            logForkState("envelope", net, shape, prior, txA, txB);
            log << "  local submit A: "
                << (resultA ? transToken(*resultA) : "not-run")
                << ", B: " << (resultB ? transToken(*resultB) : "not-run")
                << std::endl;
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        if (resultA && *resultA != tesSUCCESS)
            log << "  local submit A: " << transToken(*resultA) << std::endl;
        if (resultB && *resultB != tesSUCCESS)
            log << "  local submit B: " << transToken(*resultB) << std::endl;
        if (!BEAST_EXPECT(resultA && *resultA == tesSUCCESS))
            return std::nullopt;
        if (!BEAST_EXPECT(resultB && *resultB == tesSUCCESS))
            return std::nullopt;
        if (!BEAST_EXPECT(txA && txB))
            return std::nullopt;
        if (!BEAST_EXPECT(target))
            return std::nullopt;
        BEAST_EXPECT(
            net.ledgerHash(shape.a.front(), *target) !=
            net.ledgerHash(shape.b.front(), *target));
        BEAST_EXPECT(!net.validatedForkFree());
        BEAST_EXPECT(net.expectApplied(shape.a.front(), *target, *txA));
        BEAST_EXPECT(net.expectApplied(shape.b.front(), *target, *txB));

        log << "  fork-envelope overlap=0 fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec
            << ", traceCount=" << net.traceCount() << std::endl;
        std::vector<uint256> payload;
        payload.push_back(net.ledgerHash(shape.a.front(), *target));
        payload.push_back(net.ledgerHash(shape.b.front(), *target));
        payload.push_back(*txA);
        payload.push_back(*txB);
        return payload;
    }

    KProfiledForkCell
    runKProfiledForkCell(
        std::uint32_t overlap,
        std::uint32_t k,
        std::chrono::milliseconds unitCost = std::chrono::milliseconds{5})
    {
        using namespace jtx;
        using namespace std::chrono;

        KProfiledForkCell out;
        out.overlap = overlap;
        out.k = k;
        out.unitCostMs = unitCost.count();
        if (!BEAST_EXPECT(overlap + 2 <= kForkPeers))
            return out;
        auto const shape = forkShape(overlap);

        SteppingNetwork net(*this);
        if (!configureForkTopology(net, shape))
            return out;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return out;
        auto const prior = net.minValidatedSeq();
        out.target = prior + 4;

        Account const a{"trust-profiled-a"};
        Account const b{"trust-profiled-b"};
        std::optional<uint256> txA;
        std::optional<uint256> txB;
        std::optional<TER> resultA;
        std::optional<TER> resultB;
        net.in(milliseconds{500}, shape.aOnly.front(), [&]() {
            auto const tx = net.submit(
                shape.aOnly.front(),
                pay(Account::master, a, XRP(100000)),
                Account::master);
            resultA = tx->getResult();
            txA = tx->getID();
        });
        net.in(milliseconds{500}, shape.bOnly.front(), [&]() {
            auto const tx = net.submit(
                shape.bOnly.front(),
                pay(Account::master, b, XRP(100000)),
                Account::master);
            resultB = tx->getResult();
            txB = tx->getID();
        });

        auto const stats = net.runProfiledTo(
            out.target,
            SteppingNetwork::KProfiledOptions{
                /*k=*/k,
                /*unitCost=*/unitCost,
                HarnessScheduler::ProfiledPacer::NodeMultipliers{}},
            SteppingNetwork::RunBudget{
                /*heartbeats=*/160, /*steps=*/1'000'000});

        out.fingerprint = net.traceFingerprint();
        out.events = net.traceCount();
        out.weightedEvents = stats.weightedEvents;
        out.steps = stats.steps;
        out.beats = stats.beats;
        out.minValidated = net.minValidatedSeq();
        for (std::uint32_t i = 0; i < kForkPeers; ++i)
            out.maxValidated = std::max(out.maxValidated, net.validSeq(i));
        out.forkCheckedSeqs = net.forkCheckedSeqs();
        out.clampHits = stats.clampHits;
        out.requestedMs = asMs(stats.requestedVirtualAdvance);
        out.consumedMs = asMs(stats.consumedVirtualAdvance);
        out.schedulerMs = asMs(stats.schedulerNow);
        out.heartbeatEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::heartbeat)];
        out.deliverEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::deliver)];
        out.jobEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::job)];
        out.timerEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::timer)];
        out.firstClampWeight = stats.firstClampWeight;
        for (auto const d : stats.consumedPerBeat)
        {
            auto const ms = asMs(d);
            if (ms > out.maxConsumedBeatMs)
                out.maxConsumedBeatMs = ms;
        }

        out.submittedA = resultA && *resultA == tesSUCCESS && txA.has_value();
        out.submittedB = resultB && *resultB == tesSUCCESS && txB.has_value();
        if (txA)
            if (auto const seq = findAppliedSeq(
                    net, shape.a.front(), prior + 1, out.maxValidated, *txA))
                out.txASeq = *seq;
        if (txB)
            if (auto const seq = findAppliedSeq(
                    net, shape.b.front(), prior + 1, out.maxValidated, *txB))
                out.txBSeq = *seq;

        for (auto seq = prior + 1; seq <= out.maxValidated; ++seq)
        {
            if (net.validatedAgree(shape.a, seq) &&
                net.validatedAgree(shape.b, seq) &&
                net.ledgerHash(shape.a.front(), seq) !=
                    net.ledgerHash(shape.b.front(), seq) &&
                txA && txB &&
                findAppliedSeq(net, shape.a.front(), seq, seq, *txA) &&
                findAppliedSeq(net, shape.b.front(), seq, seq, *txB))
            {
                out.divergentSeq = seq;
                break;
            }
            if (net.validatedAgree(shape.all, seq) && txA && txB &&
                (findAppliedSeq(net, 0, seq, seq, *txA) ||
                 findAppliedSeq(net, 0, seq, seq, *txB)))
            {
                out.agreedSeq = seq;
            }
        }

        out.forkFree = net.validatedForkFree();
        out.forked = out.divergentSeq != 0 && !out.forkFree;
        out.safeResolved = out.forkFree && out.agreedSeq != 0 &&
            out.minValidated >= out.target;
        out.unresolved = !out.forked && !out.safeResolved;
        out.saturated = stats.saturated();

        log << "  profiled-fork-cell overlap=" << overlap << " K=" << k
            << " unitMs=" << unitCost.count() << ": fp=0x" << std::hex
            << out.fingerprint << std::dec << ", events=" << out.events
            << ", weightedEvents=" << out.weightedEvents
            << ", steps=" << out.steps << ", beats=" << out.beats
            << ", minValidated=" << out.minValidated
            << ", maxValidated=" << out.maxValidated
            << ", forkCheckedSeqs=" << out.forkCheckedSeqs
            << ", target=" << out.target
            << ", divergentSeq=" << out.divergentSeq
            << ", agreedSeq=" << out.agreedSeq << ", txASeq=" << out.txASeq
            << ", txBSeq=" << out.txBSeq << ", clampHits=" << out.clampHits
            << ", requestedMs=" << out.requestedMs
            << ", consumedMs=" << out.consumedMs
            << ", maxBeatMs=" << out.maxConsumedBeatMs
            << ", schedulerMs=" << out.schedulerMs
            << ", kindEvents={heartbeat:" << out.heartbeatEvents
            << ", deliver:" << out.deliverEvents << ", job:" << out.jobEvents
            << ", timer:" << out.timerEvents
            << "}, firstClampWeight=" << out.firstClampWeight
            << ", submitted=" << out.submittedA << "/" << out.submittedB
            << ", forkFree=" << out.forkFree << ", forked=" << out.forked
            << ", safeResolved=" << out.safeResolved
            << ", unresolved=" << out.unresolved
            << ", saturated=" << out.saturated << std::endl;
        if (stats.saturated())
        {
            log << "    first clamp: kind="
                << HarnessScheduler::kindName(stats.firstClampEvent.kind)
                << ", tier="
                << HarnessScheduler::tierName(stats.firstClampEvent.tier)
                << ", node=" << stats.firstClampEvent.nodeId
                << ", requestedMs=" << asMs(stats.firstClampRequested)
                << ", budgetMs=" << asMs(stats.firstClampBudget)
                << ", weight=" << stats.firstClampWeight << std::endl;
        }

        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(out.submittedA);
        BEAST_EXPECT(out.submittedB);
        if (out.forked)
        {
            BEAST_EXPECT(out.divergentSeq != 0);
            BEAST_EXPECT(out.forkCheckedSeqs >= out.divergentSeq - 1);
            BEAST_EXPECT(!out.forkFree);
        }
        if (out.safeResolved)
        {
            BEAST_EXPECT(out.agreedSeq != 0);
            BEAST_EXPECT(out.forkCheckedSeqs >= out.agreedSeq - 1);
            BEAST_EXPECT(out.forkFree);
        }
        return out;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runWrongLcl(SteppingNetwork& net)
    {
        using namespace jtx;
        using namespace std::chrono;

        WrongLclShape const shape;
        net.recordChainHistory();
        if (!configureWrongLclTopology(net, shape))
            return std::nullopt;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        Account const tx0{"wrong-lcl-0"};
        Account const tx1{"wrong-lcl-1"};
        net.fund(
            0,
            XRP(100000),
            std::vector<Account>{tx0, tx1},
            SteppingNetwork::RunBudget{/*heartbeats=*/80});
        auto const fundedSeq = net.minValidatedSeq();

        auto const dropRawTx = [](std::uint16_t type, std::size_t) {
            SimFault f;
            f.drop =
                type == static_cast<std::uint16_t>(protocol::mtTRANSACTION);
            return f;
        };
        for (auto const a : shape.majorityA)
            for (auto const b : shape.majorityB)
            {
                net.faultLink(a, b, dropRawTx);
                net.faultLink(b, a, dropRawTx);
            }

        auto const acquireBefore = countTxnAcquireJobs(net);
        std::optional<uint256> txA;
        std::optional<uint256> txB;
        net.in(milliseconds{500}, shape.minority.front(), [&]() {
            auto const tx = net.submit(
                shape.minority.front(), pay(tx0, Account::master, XRP(1)), tx0);
            if (tx->getResult() != tesSUCCESS)
                log << "  wrong-LCL local submit A: "
                    << transToken(tx->getResult()) << std::endl;
            txA = tx->getID();
        });
        net.in(milliseconds{500}, shape.majorityB.front(), [&]() {
            auto const tx = net.submit(
                shape.majorityB.front(),
                pay(tx1, Account::master, XRP(2)),
                tx1);
            if (tx->getResult() != tesSUCCESS)
                log << "  wrong-LCL local submit B: "
                    << transToken(tx->getResult()) << std::endl;
            txB = tx->getID();
        });

        std::optional<std::uint32_t> wrongSeq;
        std::optional<uint256> wrongHash;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    if (!txA || !txB)
                        return false;
                    auto const seq = net.closedSeq(shape.minority.front());
                    if (seq <= fundedSeq)
                        return false;
                    auto const h = net.closedHash(shape.minority.front());
                    auto const l = net.closedLedger(shape.minority.front());
                    if (!l || !l->txExists(*txA))
                        return false;
                    wrongSeq = seq;
                    wrongHash = h;
                    return true;
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/20})))
        {
            log << "  wrong-LCL split tips";
            for (std::uint32_t i = 0; i < kForkPeers; ++i)
                log << " n" << i << "(c=" << net.closedSeq(i)
                    << ",v=" << net.validSeq(i) << ")";
            log << std::endl;
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        auto const acquireDuringSplit =
            countTxnAcquireJobs(net) - acquireBefore;
        log << "  wrong-LCL tx-set acquire jobs during split: "
            << acquireDuringSplit << std::endl;
        if (!BEAST_EXPECT(acquireDuringSplit > 0))
            return std::nullopt;
        if (!BEAST_EXPECT(net.validatedForkFree()))
            return std::nullopt;

        for (auto const a : shape.majorityA)
            for (auto const b : shape.majorityB)
            {
                net.faultLink(a, b, {});
                net.faultLink(b, a, {});
            }

        auto const target = *wrongSeq + 4;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    return net.minValidatedSeq() >= target &&
                        net.validatedForkFree() && net.ledgersAgree(target);
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/120})))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        if (!net.expectConverged(target))
            return std::nullopt;
        if (!BEAST_EXPECT(!net.closedJumps(shape.minority.front()).empty()))
            return std::nullopt;
        if (!BEAST_EXPECT(
                net.expectAbandonedClosed(shape.minority.front(), *wrongHash)))
            return std::nullopt;

        log << "  wrong-LCL fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec
            << ", traceCount=" << net.traceCount() << ", wrongSeq=" << *wrongSeq
            << std::endl;

        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        payload.push_back(*wrongHash);
        payload.push_back(uint256{acquireDuringSplit});
        return payload;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runLatencyReplacement(SteppingNetwork& net)
    {
        using namespace std::chrono;

        constexpr std::uint32_t n = 3;
        net.validators(n).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;

        auto const dropAll = [](std::uint16_t, std::size_t) {
            SimFault f;
            f.drop = true;
            return f;
        };
        for (std::uint32_t i = 0; i < n; ++i)
            for (std::uint32_t j = i + 1; j < n; ++j)
            {
                net.faultLink(i, j, dropAll);
                net.faultLink(j, i, dropAll);
            }

        net.linkDelays(latency::uniform(milliseconds{20}));
        net.runTo(3, SteppingNetwork::RunBudget{/*heartbeats=*/80});
        if (!net.expectConverged(3))
            return std::nullopt;

        log << "  latency replacement fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec
            << ", traceCount=" << net.traceCount() << std::endl;
        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= 3; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        return payload;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runAcceptLag(SteppingNetwork& net)
    {
        using namespace jtx;
        using namespace std::chrono;

        constexpr std::uint32_t n = 5;
        std::vector<std::uint32_t> all{0, 1, 2, 3, 4};
        Account const source{"lag-accept-source"};

        net.validators(n).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;
        net.fund(1, XRP(100000), std::vector<Account>{source});
        auto const prior = net.minValidatedSeq();

        net.lagAccept(0, seconds{20});
        auto const tx =
            net.submit(1, pay(source, Account::master, XRP(1)), source);
        if (!BEAST_EXPECT(tx->getResult() == tesSUCCESS))
            return std::nullopt;

        std::optional<std::uint32_t> aheadTarget;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    for (std::uint32_t i = 1; i < n; ++i)
                    {
                        if (net.validSeq(i) <= prior)
                            return false;
                    }
                    auto const target = net.validSeq(1);
                    if (net.jobDiagnostics().find("laggedPending") ==
                        std::string::npos)
                        return false;
                    aheadTarget = target;
                    return true;
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/12})))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        auto const diagnostics = net.jobDiagnostics();
        BEAST_EXPECT(diagnostics.find("laggedPending") != std::string::npos);
        log << "  accept lag pending at target " << *aheadTarget
            << ": n0 valid=" << net.validSeq(0)
            << ", quorum valid=" << net.validSeq(1) << std::endl;

        net.clearLag(0);
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    return net.validatedAgree(all, *aheadTarget) &&
                        net.validatedForkFree() &&
                        net.jobDiagnostics().find("laggedPending") ==
                        std::string::npos;
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/80})))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        if (!net.expectConverged(*aheadTarget))
            return std::nullopt;

        log << "  accept lag fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec
            << ", traceCount=" << net.traceCount() << std::endl;
        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= *aheadTarget; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        payload.push_back(tx->getID());
        return payload;
    }

    void
    testTrustGuardRails()
    {
        testcase("per-node UNL guard rails");
        SteppingNetwork net(*this);
        net.identities(3);
        BEAST_EXPECT(except<std::logic_error>([&]() { net.trust(3, {0}); }));
        BEAST_EXPECT(except<std::logic_error>([&]() { net.trust(0, {3}); }));
        BEAST_EXPECT(except<std::logic_error>([&]() { net.trust(0, {1, 1}); }));
        net.trust(0, {2, 0});
        net.spawn({0});
        BEAST_EXPECT(except<std::logic_error>([&]() { net.trust(0, {0}); }));
    }

    void
    testForkSafe()
    {
        testcase(
            "fork envelope: N=10, overlap above bound remains validated "
            "fork-free");
        expectReplays(
            *this, "fork-safe overlap 6", [this](SteppingNetwork& net) {
                return runForkSafe(net);
            });
    }

    void
    testForkEnvelope()
    {
        testcase(
            "fork envelope: N=10, overlap below bound validates divergent "
            "cliques");
        expectReplays(
            *this, "fork-envelope overlap 0", [this](SteppingNetwork& net) {
                return runForkEnvelope(net);
            });
    }

    void
    testWrongLcl()
    {
        testcase(
            "wrong LCL: minority closes a wrong ledger, abandons it, and "
            "recovers without a validated fork");
        expectReplays(*this, "wrong LCL", [this](SteppingNetwork& net) {
            return runWrongLcl(net);
        });
    }

    void
    testProfiledForkEnvelopeMargin()
    {
        testcase(
            "K-profiled fork envelope: overlap x pressure margin stays "
            "classified without vacuous cells");

        // Retain the donor's K axis with its 5ms unit. Extra explicitly
        // labelled 1ms cells also exercise progress under lighter pressure.
        // A sample is identified by overlap, K AND unitCostMs.
        std::array<KProfiledForkCell, 9> const kExpected = {{
            {0,     0,     0x56ce6480b4972d74ull,
             3488,  0,     2060,
             0,     8,     8,
             7,     8,     5,
             0,     5,     5,
             0,     0,     0,
             0,     54020, 0,
             0,     0,     0,
             0,     true,  true,
             false, true,  false,
             false, false, 5},
            {0,     1,     0x5bcaa0a57392b44bull,
             3475,  4585,  2047,
             12,    8,     8,
             7,     8,     5,
             0,     5,     5,
             0,     4585,  4585,
             790,   54345, 120,
             1312,  613,   0,
             0,     true,  true,
             false, true,  false,
             false, false, 1},
            {0,     1,     0x66349c6e0ba11e57ull,
             3776,  5322,  2348,
             26,    8,     8,
             7,     8,     6,
             0,     6,     6,
             25,    26610, 26385,
             2000,  68395, 250,
             1161,  878,   57,
             2,     true,  true,
             false, true,  false,
             false, true,  5},
            {4,     0,     0xdc05a22afd9269d0ull,
             6153,  0,     4218,
             0,     8,     8,
             7,     8,     0,
             5,     5,     0,
             0,     0,     0,
             0,     55020, 0,
             0,     0,     0,
             0,     true,  true,
             true,  false, true,
             false, false, 5},
            {4,     1,     0x7872f20cdb7ec1c9ull,
             6231,  9698,  4296,
             14,    8,     8,
             7,     8,     0,
             5,     5,     0,
             6,     9698,  9687,
             1643,  56548, 140,
             2884,  1248,  22,
             3,     true,  true,
             true,  false, true,
             false, true,  1},
            {4,     1,      0x7797f687638895d9ull,
             14855, 32488,  12920,
             160,   5,      5,
             4,     8,      0,
             0,     0,      0,
             160,   162440, 161000,
             2000,  203010, 1490,
             2839,  8140,   449,
             2,     true,   true,
             true,  false,  false,
             true,  true,   5},
            {6,     0,     0xc54816e93aff2fd2ull,
             6147,  0,     4108,
             0,     8,     8,
             7,     8,     0,
             5,     5,     0,
             0,     0,     0,
             0,     54020, 0,
             0,     0,     0,
             0,     true,  true,
             true,  false, true,
             false, false, 5},
            {6,     1,     0x63e402b833368e31ull,
             6292,  9487,  4253,
             15,    8,     8,
             7,     8,     0,
             5,     5,     0,
             7,     9487,  9472,
             1635,  57612, 150,
             2952,  1133,  16,
             3,     true,  true,
             true,  false, true,
             false, true,  1},
            {6,     1,     0xf49350dbcf9e3c9cull,
             6477,  10070, 4438,
             49,    8,     8,
             7,     8,     0,
             6,     6,     0,
             48,    50350, 49935,
             2000,  91945, 480,
             2156,  1676,  124,
             2,     true,  true,
             true,  false, true,
             false, true,  5},
        }};

        bool sawForkUnderPressureControl = false;
        bool sawSafeBaseline = false;
        bool sawSafeUnderPressure = false;
        bool sawPressureStallNoFork = false;
        for (auto const& expected : kExpected)
        {
            auto const cell = runKProfiledForkCell(
                expected.overlap,
                expected.k,
                std::chrono::milliseconds{expected.unitCostMs});
            BEAST_EXPECT(cell == expected);
            BEAST_EXPECT(cell.submittedA);
            BEAST_EXPECT(cell.submittedB);
            BEAST_EXPECT(cell.forkCheckedSeqs != 0);
            if (cell.k == 0)
            {
                BEAST_EXPECT(cell.weightedEvents == 0);
                BEAST_EXPECT(cell.clampHits == 0);
            }
            else
            {
                BEAST_EXPECT(cell.weightedEvents != 0);
            }

            if (cell.forked)
            {
                BEAST_EXPECT(cell.divergentSeq != 0);
                BEAST_EXPECT(!cell.forkFree);
                sawForkUnderPressureControl = sawForkUnderPressureControl ||
                    (cell.overlap == 0 && cell.k != 0);
            }
            else if (cell.safeResolved)
            {
                BEAST_EXPECT(cell.agreedSeq != 0);
                BEAST_EXPECT(cell.forkFree);
                sawSafeBaseline = sawSafeBaseline || cell.k == 0;
                sawSafeUnderPressure = sawSafeUnderPressure || cell.k != 0;
            }
            else
            {
                BEAST_EXPECT(cell.unresolved);
                BEAST_EXPECT(cell.forkFree);
                BEAST_EXPECT(cell.saturated);
                sawPressureStallNoFork = true;
            }

            if (cell.overlap >= 4 && cell.k != 0)
                BEAST_EXPECT(!cell.forked);
        }

        BEAST_EXPECT(sawForkUnderPressureControl);
        BEAST_EXPECT(sawSafeBaseline);
        BEAST_EXPECT(sawSafeUnderPressure);
        BEAST_EXPECT(sawPressureStallNoFork);
    }

    void
    testLatencyProfiles()
    {
        using namespace std::chrono;

        testcase("latency profile callables and replace semantics");
        auto const ring = latency::ring(4, milliseconds{10}, milliseconds{30});
        BEAST_EXPECT(ring(0, 0) == std::nullopt);
        BEAST_EXPECT(ring(0, 1) == milliseconds{10});
        BEAST_EXPECT(ring(0, 3) == milliseconds{10});
        BEAST_EXPECT(ring(0, 2) == milliseconds{30});

        auto const clustered = latency::clusters(
            {{0, 1}, {2, 3}}, milliseconds{5}, milliseconds{40});
        BEAST_EXPECT(clustered(0, 1) == milliseconds{5});
        BEAST_EXPECT(clustered(0, 2) == milliseconds{40});
        BEAST_EXPECT(clustered(0, 9) == std::nullopt);

        SteppingNetwork guardNet(*this);
        guardNet.validators(2).mesh();
        BEAST_EXPECT(except<std::logic_error>([&]() {
            guardNet.linkDelays([](std::uint32_t, std::uint32_t) {
                return std::optional<std::chrono::steady_clock::duration>{
                    milliseconds{-1}};
            });
        }));

        expectReplays(
            *this, "latency replacement", [this](SteppingNetwork& net) {
                return runLatencyReplacement(net);
            });
    }

    void
    testAcceptLag()
    {
        testcase(
            "virtual accept lag: lagged accept work is pending while a 4-of-5 "
            "quorum advances, then the network converges after lag clears");
        expectReplays(*this, "accept lag", [this](SteppingNetwork& net) {
            return runAcceptLag(net);
        });
    }

public:
    void
    run() override
    {
        testTrustGuardRails();
        testForkSafe();
        testForkEnvelope();
        testWrongLcl();
        testProfiledForkEnvelopeMargin();
        testLatencyProfiles();
        testAcceptLag();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingTrust, consensus, ripple);

}  // namespace ripple::test
