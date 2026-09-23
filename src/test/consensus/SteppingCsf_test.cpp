//------------------------------------------------------------------------------
// SteppingCsf -- ports of selected CSF scenario semantics onto the real
// stepping harness. CSF proves the consensus algorithm over toy peers; these
// cases keep the same scenario intent but run real Application/LedgerMaster/
// PeerImp/JobQueue code through SteppingNetwork.
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

class SteppingCsf_test : public beast::unit_test::suite
{
    // Xahaud's acquisition timeline; semantic lag/fork and replay checks below
    // are retained alongside this target-specific snapshot.
    static constexpr std::uint64_t kSlowMinorityFingerprint =
        0x25ec64d383356379ull;
    static constexpr std::uint64_t kHubNetworkFingerprint =
        0x182601635f5b0308ull;
    static constexpr std::uint64_t kDisputeFingerprint = 0x297cf7d89bfd08f7ull;

    struct KProfiledDisputeSample
    {
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
        std::uint32_t acceptedSeq = 0;
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
        bool converged = false;
        bool exactlyOneAccepted = false;
        bool acceptedSetVerified = false;
        // Availability before any verification-only backfill. False when
        // no transaction was accepted, or its historical ledger is missing.
        bool historyReadyAtSnapshot = false;

        [[nodiscard]] bool
        operator==(KProfiledDisputeSample const& o) const
        {
            return k == o.k && fingerprint == o.fingerprint &&
                events == o.events && weightedEvents == o.weightedEvents &&
                steps == o.steps && beats == o.beats &&
                minValidated == o.minValidated &&
                maxValidated == o.maxValidated &&
                forkCheckedSeqs == o.forkCheckedSeqs && target == o.target &&
                acceptedSeq == o.acceptedSeq && txASeq == o.txASeq &&
                txBSeq == o.txBSeq && clampHits == o.clampHits &&
                requestedMs == o.requestedMs && consumedMs == o.consumedMs &&
                maxConsumedBeatMs == o.maxConsumedBeatMs &&
                schedulerMs == o.schedulerMs &&
                heartbeatEvents == o.heartbeatEvents &&
                deliverEvents == o.deliverEvents && jobEvents == o.jobEvents &&
                timerEvents == o.timerEvents &&
                firstClampWeight == o.firstClampWeight &&
                submittedA == o.submittedA && submittedB == o.submittedB &&
                forkFree == o.forkFree && converged == o.converged &&
                exactlyOneAccepted == o.exactlyOneAccepted &&
                acceptedSetVerified == o.acceptedSetVerified &&
                historyReadyAtSnapshot == o.historyReadyAtSnapshot;
        }
    };

    struct SubmittedTx
    {
        uint256 id;
    };

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

    [[nodiscard]] std::optional<std::uint32_t>
    findTxSeq(
        SteppingNetwork& net,
        std::uint32_t node,
        std::uint32_t firstSeq,
        std::uint32_t lastSeq,
        uint256 const& txid)
    {
        for (auto seq = firstSeq; seq <= lastSeq; ++seq)
        {
            auto const ledger = net.ledger(node, seq);
            if (ledger && ledger->txExists(txid))
                return seq;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::uint32_t>
    findAppliedTxSeq(
        SteppingNetwork& net,
        std::uint32_t node,
        std::uint32_t firstSeq,
        std::uint32_t lastSeq,
        uint256 const& txid,
        TER expected = TER{tesSUCCESS})
    {
        for (auto seq = firstSeq; seq <= lastSeq; ++seq)
        {
            for (auto const& applied : net.appliedTxs(node, seq))
            {
                if (applied.txid == txid && applied.result == expected)
                    return seq;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    findAllTxSeqs(
        SteppingNetwork& net,
        std::uint32_t node,
        std::uint32_t firstSeq,
        std::vector<SubmittedTx> const& txs)
    {
        auto const lastSeq = net.minValidatedSeq();
        if (lastSeq < firstSeq)
            return std::nullopt;

        std::vector<std::uint32_t> seqs;
        seqs.reserve(txs.size());
        for (auto const& tx : txs)
        {
            auto const seq = findTxSeq(net, node, firstSeq, lastSeq, tx.id);
            if (!seq)
                return std::nullopt;
            seqs.push_back(*seq);
        }
        return seqs;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runSlowMinority(SteppingNetwork& net)
    {
        using namespace jtx;
        using namespace std::chrono;

        constexpr std::uint32_t n = 5;
        net.validators(n).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;

        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        std::array<Account, n> sources = {
            Account{"csf-slow-0"},
            Account{"csf-fast-1"},
            Account{"csf-fast-2"},
            Account{"csf-fast-3"},
            Account{"csf-fast-4"}};
        std::vector<Account> sourceVec{sources.begin(), sources.end()};
        net.fund(1, XRP(100000), sourceVec);

        auto const fundedSeq = net.minValidatedSeq();

        // CSF slow-peers: one validator's links to the rest of the network are
        // slower than the close cadence. The fast 4-of-5 quorum should close a
        // ledger before the slow validator's client transaction becomes part of
        // the agreed set; the slow tx then lands later without forking.
        for (std::uint32_t fast = 1; fast < n; ++fast)
        {
            net.faultLink(0, fast, simfaults::delayAll(milliseconds{3500}));
            net.faultLink(fast, 0, simfaults::delayAll(milliseconds{3500}));
        }

        std::vector<SubmittedTx> txs;
        txs.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            auto const tx = net.submit(
                i, pay(sources[i], Account::master, XRP(1)), sources[i]);
            if (!BEAST_EXPECT(tx->getResult() == tesSUCCESS))
                return std::nullopt;
            txs.push_back(SubmittedTx{tx->getID()});
        }

        std::optional<std::vector<std::uint32_t>> txSeqs;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    txSeqs = findAllTxSeqs(net, 0, fundedSeq + 1, txs);
                    return txSeqs.has_value();
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/180})))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }

        auto const target = net.minValidatedSeq();
        if (!net.expectConverged(target))
            return std::nullopt;

        txSeqs = findAllTxSeqs(net, 0, fundedSeq + 1, txs);
        if (!BEAST_EXPECT(txSeqs.has_value()))
            return std::nullopt;
        for (auto const seq : *txSeqs)
        {
            BEAST_EXPECT(net.ledgersAgree(seq));
        }

        auto const fastFirst =
            *std::min_element(txSeqs->begin() + 1, txSeqs->end());
        auto const fastLast =
            *std::max_element(txSeqs->begin() + 1, txSeqs->end());
        BEAST_EXPECT(fastFirst == fastLast);
        BEAST_EXPECT((*txSeqs)[0] > fastFirst);

        auto const fingerprint = net.traceFingerprint();
        log << "  slow-minority fingerprint 0x" << std::hex << fingerprint
            << std::dec << ", traceCount=" << net.traceCount() << std::endl;
        BEAST_EXPECT(fingerprint == kSlowMinorityFingerprint);

        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        for (auto const seq : *txSeqs)
            payload.push_back(uint256{static_cast<std::uint64_t>(seq)});
        return payload;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runHubNetwork(SteppingNetwork& net)
    {
        using namespace jtx;

        constexpr std::uint32_t validators = 5;
        constexpr std::uint32_t hub = validators;

        net.validators(validators);
        auto const hubId = net.observer();
        if (!BEAST_EXPECT(hubId == hub))
            return std::nullopt;
        for (std::uint32_t i = 0; i < validators; ++i)
            if (!BEAST_EXPECT(net.connect(i, hub) != nullptr))
                return std::nullopt;

        if (!BEAST_EXPECT(net.allUp()))
            return std::nullopt;
        if (!BEAST_EXPECT(net.runUntil([&]() {
                if (net.node(hub).app().overlay().size() != validators)
                    return false;
                for (std::uint32_t i = 0; i < validators; ++i)
                    if (net.node(i).app().overlay().size() != 1)
                        return false;
                return true;
            })))
            return std::nullopt;

        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        std::array<Account, validators> sources = {
            Account{"csf-hub-0"},
            Account{"csf-hub-1"},
            Account{"csf-hub-2"},
            Account{"csf-hub-3"},
            Account{"csf-hub-4"}};
        std::vector<Account> sourceVec{sources.begin(), sources.end()};
        net.fund(0, XRP(100000), sourceVec);
        auto const fundedSeq = net.minValidatedSeq();

        std::vector<SubmittedTx> txs;
        txs.reserve(validators);
        for (std::uint32_t i = 0; i < validators; ++i)
        {
            auto const tx = net.submit(
                i, pay(sources[i], Account::master, XRP(1)), sources[i]);
            if (!BEAST_EXPECT(tx->getResult() == tesSUCCESS))
                return std::nullopt;
            txs.push_back(SubmittedTx{tx->getID()});
        }

        std::optional<std::vector<std::uint32_t>> txSeqs;
        if (!BEAST_EXPECT(net.runUntil([&]() {
                txSeqs = findAllTxSeqs(net, 0, fundedSeq + 1, txs);
                return txSeqs.has_value();
            })))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }

        auto const target = net.minValidatedSeq();
        if (!net.expectConverged(target))
            return std::nullopt;

        txSeqs = findAllTxSeqs(net, 0, fundedSeq + 1, txs);
        if (!BEAST_EXPECT(txSeqs.has_value()))
            return std::nullopt;
        for (auto const seq : *txSeqs)
        {
            BEAST_EXPECT(net.ledgersAgree(seq));
        }
        BEAST_EXPECT(
            std::all_of(txSeqs->begin(), txSeqs->end(), [&](auto const seq) {
                return seq == txSeqs->front();
            }));

        auto const fingerprint = net.traceFingerprint();
        log << "  hub-network fingerprint 0x" << std::hex << fingerprint
            << std::dec << ", traceCount=" << net.traceCount() << std::endl;
        BEAST_EXPECT(fingerprint == kHubNetworkFingerprint);

        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        for (auto const seq : *txSeqs)
            payload.push_back(uint256{static_cast<std::uint64_t>(seq)});
        return payload;
    }

    [[nodiscard]] std::optional<std::vector<uint256>>
    runDispute(SteppingNetwork& net)
    {
        using namespace jtx;
        using namespace std::chrono;

        Account const alice{"csf-dispute-alice"};

        net.validators(5).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        net.fund(0, XRP(100000), {alice});
        auto const fundedSeq = net.minValidatedSeq();

        std::optional<uint256> txA;
        std::optional<uint256> txB;
        std::optional<TER> resultA;
        std::optional<TER> resultB;

        // Both submissions fill from the same validated account sequence on
        // different nodes at the same virtual instant. Locally each tx is
        // valid; globally they are mutually exclusive. Consensus must choose
        // one tx-set winner and apply it identically everywhere.
        net.in(milliseconds{500}, 0, [&]() {
            auto const tx =
                net.submit(0, pay(alice, Account::master, XRP(1)), alice);
            resultA = tx->getResult();
            txA = tx->getID();
        });
        net.in(milliseconds{500}, 1, [&]() {
            auto const tx =
                net.submit(1, pay(alice, Account::master, XRP(2)), alice);
            resultB = tx->getResult();
            txB = tx->getID();
        });

        std::optional<std::uint32_t> txASeq;
        std::optional<std::uint32_t> txBSeq;
        if (!BEAST_EXPECT(net.runUntil([&]() {
                if (!txA || !txB)
                    return false;
                txASeq = findTxSeq(
                    net, 0, fundedSeq + 1, net.minValidatedSeq(), *txA);
                txBSeq = findTxSeq(
                    net, 0, fundedSeq + 1, net.minValidatedSeq(), *txB);
                return txASeq || txBSeq;
            })))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }

        if (!BEAST_EXPECT(resultA && *resultA == tesSUCCESS))
            return std::nullopt;
        if (!BEAST_EXPECT(resultB && *resultB == tesSUCCESS))
            return std::nullopt;
        if (!BEAST_EXPECT(txA && txB && *txA != *txB))
            return std::nullopt;

        std::optional<std::uint32_t> target;
        if (!BEAST_EXPECT(net.runUntil(
                [&]() {
                    auto const hi = net.minValidatedSeq();
                    txASeq = findTxSeq(net, 0, fundedSeq + 1, hi, *txA);
                    txBSeq = findTxSeq(net, 0, fundedSeq + 1, hi, *txB);
                    if (txASeq.has_value() == txBSeq.has_value())
                        return false;
                    auto const acceptedSeq = txASeq ? *txASeq : *txBSeq;
                    if (!net.ledgersAgree(acceptedSeq) || !net.ledgersAgree(hi))
                        return false;
                    target = hi;
                    return true;
                },
                SteppingNetwork::RunBudget{/*heartbeats=*/60})))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        if (!BEAST_EXPECT(target))
            return std::nullopt;
        if (!net.expectConverged(*target))
            return std::nullopt;

        txASeq = findTxSeq(net, 0, fundedSeq + 1, *target, *txA);
        txBSeq = findTxSeq(net, 0, fundedSeq + 1, *target, *txB);
        if (!BEAST_EXPECT(txASeq.has_value() != txBSeq.has_value()))
            return std::nullopt;

        auto const acceptedSeq = txASeq ? *txASeq : *txBSeq;
        auto const acceptedTx = txASeq ? *txA : *txB;
        auto const rejectedTx = txASeq ? *txB : *txA;
        BEAST_EXPECT(net.ledgersAgree(acceptedSeq));

        auto const reference = net.appliedTxs(0, acceptedSeq);
        if (!BEAST_EXPECT(!reference.empty()))
            return std::nullopt;
        auto const acceptedIt = std::find_if(
            reference.begin(), reference.end(), [&](auto const& applied) {
                return applied.txid == acceptedTx;
            });
        if (!BEAST_EXPECT(acceptedIt != reference.end()))
            return std::nullopt;
        BEAST_EXPECT(acceptedIt->result == tesSUCCESS);
        BEAST_EXPECT(std::none_of(
            reference.begin(), reference.end(), [&](auto const& applied) {
                return applied.txid == rejectedTx;
            }));

        for (std::uint32_t node = 1; node < 5; ++node)
        {
            auto const applied = net.appliedTxs(node, acceptedSeq);
            if (!BEAST_EXPECT(applied.size() == reference.size()))
                return std::nullopt;
            for (std::size_t i = 0; i < applied.size(); ++i)
            {
                BEAST_EXPECT(applied[i].index == reference[i].index);
                BEAST_EXPECT(applied[i].txid == reference[i].txid);
                BEAST_EXPECT(applied[i].result == reference[i].result);
            }
            BEAST_EXPECT(
                !findTxSeq(net, node, fundedSeq + 1, *target, rejectedTx));
        }

        auto const fingerprint = net.traceFingerprint();
        log << "  dispute fingerprint 0x" << std::hex << fingerprint << std::dec
            << ", traceCount=" << net.traceCount()
            << ", acceptedSeq=" << acceptedSeq << std::endl;
        BEAST_EXPECT(fingerprint == kDisputeFingerprint);

        std::vector<uint256> payload;
        for (std::uint32_t seq = 2; seq <= *target; ++seq)
            payload.push_back(net.ledgerHash(0, seq));
        payload.push_back(uint256{static_cast<std::uint64_t>(acceptedSeq)});
        payload.push_back(acceptedTx);
        payload.push_back(rejectedTx);
        return payload;
    }

    KProfiledDisputeSample
    runProfiledDispute(std::uint32_t k)
    {
        using namespace jtx;
        using namespace std::chrono;

        Account const alice{"csf-profiled-dispute-alice"};
        KProfiledDisputeSample out;
        out.k = k;

        SteppingNetwork net(*this);
        net.validators(5).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return out;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return out;

        net.fund(0, XRP(100000), {alice});
        auto const fundedSeq = net.minValidatedSeq();
        out.target = fundedSeq + 4;

        std::optional<uint256> txA;
        std::optional<uint256> txB;
        std::optional<TER> resultA;
        std::optional<TER> resultB;
        net.in(milliseconds{500}, 0, [&]() {
            auto const tx =
                net.submit(0, pay(alice, Account::master, XRP(1)), alice);
            resultA = tx->getResult();
            txA = tx->getID();
        });
        net.in(milliseconds{500}, 1, [&]() {
            auto const tx =
                net.submit(1, pay(alice, Account::master, XRP(2)), alice);
            resultB = tx->getResult();
            txB = tx->getID();
        });

        auto const stats = net.runProfiledTo(
            out.target,
            SteppingNetwork::KProfiledOptions{
                /*k=*/k,
                /*unitCost=*/milliseconds{5},
                HarnessScheduler::ProfiledPacer::NodeMultipliers{}},
            SteppingNetwork::RunBudget{
                /*heartbeats=*/160, /*steps=*/1'000'000});

        out.fingerprint = net.traceFingerprint();
        out.events = net.traceCount();
        out.weightedEvents = stats.weightedEvents;
        out.steps = stats.steps;
        out.beats = stats.beats;
        out.minValidated = net.minValidatedSeq();
        for (std::uint32_t i = 0; i < 5; ++i)
            out.maxValidated = std::max(out.maxValidated, net.validSeq(i));
        out.forkCheckedSeqs = out.maxValidated >= 2 ? out.maxValidated - 1 : 0;
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

        auto const firstDisputeSeq = fundedSeq + 1;
        auto const findAcceptedSeq = [&](uint256 const& txid) {
            std::optional<std::uint32_t> acceptedSeq;
            for (std::uint32_t node = 0; node < 5; ++node)
            {
                if (net.validSeq(node) < firstDisputeSeq)
                    continue;
                auto const seq = findAppliedTxSeq(
                    net, node, firstDisputeSeq, net.validSeq(node), txid);
                if (!seq)
                    continue;
                if (acceptedSeq)
                    BEAST_EXPECT(*acceptedSeq == *seq);
                else
                    acceptedSeq = seq;
            }
            return acceptedSeq.value_or(0);
        };

        if (txA)
            out.txASeq = findAcceptedSeq(*txA);
        if (txB)
            out.txBSeq = findAcceptedSeq(*txB);
        out.exactlyOneAccepted = (out.txASeq != 0) != (out.txBSeq != 0);
        out.acceptedSeq = out.txASeq != 0 ? out.txASeq : out.txBSeq;
        out.forkFree = net.validatedForkFree();
        out.converged = out.minValidated >= out.target;

        if (out.exactlyOneAccepted && txA && txB)
        {
            // The pressure snapshot above is complete. A node can validate a
            // newer ledger before backfilling the accepted transaction's
            // historical ledger. Materialize that witness through the real
            // scheduler/acquire path before comparing the applied sets.
            auto const historyReady = [&]() {
                for (std::uint32_t node = 0; node < 5; ++node)
                    if (!net.ledger(node, out.acceptedSeq))
                        return false;
                return true;
            };
            out.historyReadyAtSnapshot = historyReady();
            if (!out.historyReadyAtSnapshot)
            {
                BEAST_EXPECT(
                    net.runUntil(historyReady, SteppingNetwork::RunBudget{30}));
                BEAST_EXPECT(net.validatedForkFree());
            }
            auto const& acceptedTx = out.txASeq != 0 ? *txA : *txB;
            auto const& rejectedTx = out.txASeq != 0 ? *txB : *txA;
            auto verified = net.ledgersAgree(out.acceptedSeq);
            auto const reference = net.appliedTxs(0, out.acceptedSeq);
            auto const acceptedIt = std::find_if(
                reference.begin(), reference.end(), [&](auto const& applied) {
                    return applied.txid == acceptedTx;
                });
            verified = verified && acceptedIt != reference.end() &&
                acceptedIt->result == tesSUCCESS &&
                std::none_of(
                           reference.begin(),
                           reference.end(),
                           [&](auto const& applied) {
                               return applied.txid == rejectedTx;
                           });
            for (std::uint32_t node = 1; node < 5 && verified; ++node)
            {
                auto const applied = net.appliedTxs(node, out.acceptedSeq);
                verified = applied.size() == reference.size();
                for (std::size_t i = 0; i < reference.size() && verified; ++i)
                {
                    verified = applied[i].txid == reference[i].txid &&
                        applied[i].result == reference[i].result;
                }
                verified =
                    verified &&
                    std::none_of(
                        applied.begin(), applied.end(), [&](auto const& tx) {
                            return tx.txid == rejectedTx;
                        });
            }
            out.acceptedSetVerified = verified;
        }

        log << "  profiled-dispute K=" << k << ": fp=0x" << std::hex
            << out.fingerprint << std::dec << ", events=" << out.events
            << ", weightedEvents=" << out.weightedEvents
            << ", steps=" << out.steps << ", beats=" << out.beats
            << ", minValidated=" << out.minValidated
            << ", maxValidated=" << out.maxValidated
            << ", forkCheckedSeqs=" << out.forkCheckedSeqs
            << ", target=" << out.target << ", acceptedSeq=" << out.acceptedSeq
            << ", txASeq=" << out.txASeq << ", txBSeq=" << out.txBSeq
            << ", clampHits=" << out.clampHits
            << ", requestedMs=" << out.requestedMs
            << ", consumedMs=" << out.consumedMs
            << ", maxBeatMs=" << out.maxConsumedBeatMs
            << ", schedulerMs=" << out.schedulerMs
            << ", kindEvents={heartbeat:" << out.heartbeatEvents
            << ", deliver:" << out.deliverEvents << ", job:" << out.jobEvents
            << ", timer:" << out.timerEvents
            << "}, firstClampWeight=" << out.firstClampWeight
            << ", submitted=" << out.submittedA << "/" << out.submittedB
            << ", exactlyOneAccepted=" << out.exactlyOneAccepted
            << ", forkFree=" << out.forkFree << ", converged=" << out.converged
            << ", acceptedSetVerified=" << out.acceptedSetVerified
            << ", historyReadyAtSnapshot=" << out.historyReadyAtSnapshot
            << std::endl;
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
        BEAST_EXPECT(out.forkFree);
        if (out.exactlyOneAccepted)
        {
            BEAST_EXPECT(out.acceptedSeq != 0);
            BEAST_EXPECT(out.forkCheckedSeqs >= out.acceptedSeq - 1);
            BEAST_EXPECT(out.acceptedSetVerified);
        }
        return out;
    }

    void
    testSlowMinority()
    {
        testcase(
            "CSF slow peers: a slow minority's tx lags the fast quorum, then "
            "the real stack converges without a fork");
        expectReplays(*this, "csf slow minority", [this](SteppingNetwork& net) {
            return runSlowMinority(net);
        });
    }

    void
    testHubNetwork()
    {
        testcase(
            "CSF hub network: validators communicate only through a "
            "non-validator hub and still converge on the same tx set");
        expectReplays(*this, "csf hub network", [this](SteppingNetwork& net) {
            return runHubNetwork(net);
        });
    }

    void
    testDispute()
    {
        testcase(
            "CSF disputes: same-account same-sequence txs submitted on "
            "different validators resolve to one identical applied set");
        expectReplays(*this, "csf dispute", [this](SteppingNetwork& net) {
            return runDispute(net);
        });
    }

    void
    testProfiledDisputeForkHunt()
    {
        testcase(
            "K-profiled dispute fork hunt: conflicting same-sequence txs stay "
            "validated-fork-free under weighted pressure");

        // Resolved rows prove the conflict stayed safe through an accepted
        // sequence. Saturated unresolved rows are retained only as pressure
        // coverage; they must not be mistaken for dispute-resolution evidence.
        // These snapshots calibrate this lineage. K=4 is the saturated
        // unresolved control. K=3 accepts one transaction and stops short of
        // the target; the converged check below is unchanged. No consensus
        // policy is changed to fit a snapshot.
        std::array<KProfiledDisputeSample, 5> const kExpected = {{
            {0,    0xa61437b6610a4709ull,
             2645, 0,
             1152, 0,
             11,   11,
             10,   11,
             8,    8,
             0,    0,
             0,    0,
             0,    63030,
             0,    0,
             0,    0,
             0,    true,
             true, true,
             true, true,
             true, true},
            {1,     0xf550e0ee4830bfbbull,
             2656,  2597,
             1163,  15,
             11,    11,
             10,    11,
             8,     8,
             0,     10,
             12985, 12900,
             1880,  66820,
             75,    708,
             348,   30,
             2,     true,
             true,  true,
             true,  true,
             true,  true},
            {2,     0x7ebab2bea882f6c9ull,
             2792,  2995,
             1299,  29,
             11,    11,
             10,    11,
             9,     9,
             0,     28,
             29950, 29410,
             2000,  80430,
             140,   576,
             539,   42,
             2,     true,
             true,  true,
             true,  true,
             true,  true},
            {3,      0xcd1f86fa48e0a973ull,
             6001,   10918,
             4508,   160,
             10,     10,
             9,      11,
             10,     0,
             10,     160,
             163770, 161000,
             2000,   212020,
             770,    910,
             2674,   152,
             3,      true,
             true,   true,
             false,  true,
             true,   true},
            {4,      0x4e11c349617aca71ull,
             5012,   8345,
             3519,   160,
             8,      8,
             7,      11,
             0,      0,
             0,      160,
             166900, 161000,
             2000,   212020,
             735,    605,
             2044,   133,
             2,      true,
             true,   true,
             false,  false,
             false,  false},
        }};

        bool sawResolvedUnderPressure = false;
        bool sawSaturatedUnresolved = false;
        for (auto const& expected : kExpected)
        {
            auto const first = runProfiledDispute(expected.k);
            BEAST_EXPECT(first == expected);
            BEAST_EXPECT(first.submittedA);
            BEAST_EXPECT(first.submittedB);
            BEAST_EXPECT(first.forkFree);
            if (expected.k == 0)
            {
                BEAST_EXPECT(first.beats == 0);
                BEAST_EXPECT(first.weightedEvents == 0);
                BEAST_EXPECT(first.clampHits == 0);
            }
            if (first.exactlyOneAccepted)
            {
                BEAST_EXPECT(first.converged);
                BEAST_EXPECT(first.acceptedSetVerified);
                BEAST_EXPECT(first.forkCheckedSeqs >= first.acceptedSeq - 1);
                sawResolvedUnderPressure =
                    sawResolvedUnderPressure || first.clampHits != 0;
            }
            else
            {
                BEAST_EXPECT(!first.converged);
                BEAST_EXPECT(first.acceptedSeq == 0);
                BEAST_EXPECT(first.clampHits != 0);
                BEAST_EXPECT(first.forkCheckedSeqs != 0);
                sawSaturatedUnresolved = true;
            }
            for (int replay = 2; replay <= 3; ++replay)
            {
                auto const next = runProfiledDispute(expected.k);
                BEAST_EXPECT(next == expected);
                BEAST_EXPECT(next == first);
                BEAST_EXPECT(next.forkFree);
            }
        }
        BEAST_EXPECT(sawResolvedUnderPressure);
        BEAST_EXPECT(sawSaturatedUnresolved);
    }

public:
    void
    run() override
    {
        testSlowMinority();
        testHubNetwork();
        testDispute();
        testProfiledDisputeForkHunt();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingCsf, consensus, ripple);

}  // namespace ripple::test
