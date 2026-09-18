#pragma once

#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpl/beast/xor_shift_engine.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/XRPAmount.h>
#include <xrpl/protocol/jss.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ripple::test::traffic {

struct TrafficTx
{
    std::shared_ptr<Transaction> tx;
    uint256 id;
    jtx::Account src;
    jtx::Account dst;
    XRPAmount amount;
    std::uint32_t sequence = 0;
    TER submitResult = tesSUCCESS;
};

[[nodiscard]] inline std::vector<jtx::Account>
makeAccounts(std::size_t n, std::string prefix = "gen-")
{
    if (n < 2)
        throw std::logic_error(
            "traffic::makeAccounts: need at least two accounts");

    std::vector<jtx::Account> accounts;
    accounts.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        accounts.emplace_back(prefix + std::to_string(i));
    return accounts;
}

inline void
fundStepping(
    SteppingNetwork& net,
    std::uint32_t node,
    std::vector<jtx::Account> const& accounts,
    jtx::PrettyAmount const& amount,
    SteppingNetwork::RunBudget budget = {})
{
    net.fund(node, amount, accounts, budget);
}

struct ThreadedFundOptions
{
    std::chrono::milliseconds tickDuration;
    std::size_t maxBeats;
    MultiNode::ThreadedTickOptions tickOptions;
    std::function<void(MultiNode::ThreadedTickStats const&)> onTick;

    ThreadedFundOptions(
        std::chrono::milliseconds tickDuration_ = std::chrono::seconds{1},
        std::size_t maxBeats_ = 120,
        MultiNode::ThreadedTickOptions tickOptions_ = {},
        std::function<void(MultiNode::ThreadedTickStats const&)> onTick_ = {})
        : tickDuration(tickDuration_)
        , maxBeats(maxBeats_)
        , tickOptions(std::move(tickOptions_))
        , onTick(std::move(onTick_))
    {
    }
};

[[nodiscard]] inline bool
allAccountsValidated(
    MultiNode& net,
    std::vector<jtx::Account> const& accounts,
    std::uint32_t seq)
{
    if (seq == 0)
        return false;

    for (std::size_t i = 0; i < net.size(); ++i)
    {
        if (!net.isLive(i))
            continue;

        auto const ledger = net.ledger(i, seq);
        if (!ledger)
            return false;

        for (auto const& account : accounts)
            if (!ledger->exists(keylet::account(account.id())))
                return false;
    }
    return true;
}

[[nodiscard]] inline std::size_t
fundThreaded(
    MultiNode& net,
    std::size_t node,
    std::vector<jtx::Account> const& accounts,
    jtx::PrettyAmount const& amount,
    ThreadedFundOptions options = {})
{
    if (!net.isLive(node))
        throw std::logic_error("traffic::fundThreaded: node is not live");

    for (auto const& account : accounts)
    {
        auto const txn = net.submit(
            node,
            jtx::pay(jtx::Account::master, account, amount),
            jtx::Account::master);
        if (txn->getResult() != tesSUCCESS)
            throw std::logic_error(
                "traffic::fundThreaded: pay(" + account.name() +
                ") not applied: " + transToken(txn->getResult()));
    }

    std::size_t beats = 0;
    while (!allAccountsValidated(net, accounts, net.minValidated()))
    {
        if (beats >= options.maxBeats)
            throw std::logic_error(
                "traffic::fundThreaded: funding did not validate within " +
                std::to_string(options.maxBeats) + " beats");

        auto const tick =
            net.threadedTick(options.tickDuration, options.tickOptions);
        ++beats;
        if (options.onTick)
            options.onTick(tick);

        if (!net.validatedForkFree())
            throw std::logic_error(
                "traffic::fundThreaded: validated fork during funding");
    }
    return beats;
}

// Single-caller contract: the scenario thread serializes payment() and burst().
// Do not call them concurrently. The accounts managed by a Generator are
// exclusive to that generator; any out-of-band submit from those accounts will
// desync the owned sequence book. Rebuilding that book is future growth, not a
// v1 feature.
class Generator
{
public:
    static constexpr std::uint64_t defaultSeed = 0x7A4771C000000000ull;

    enum class SubmitResultPolicy {
        CheckImmediate,
        // Threaded traffic can keep touching Transaction::result_ after
        // submit() returns. Use the immutable id/sequence as the record and
        // let the validated-ledger scan prove tesSUCCESS without racing it.
        DeferToValidatedLedger
    };

    Generator(Generator const&) = delete;
    Generator&
    operator=(Generator const&) = delete;
    Generator(Generator&&) = delete;
    Generator&
    operator=(Generator&&) = delete;

    Generator(
        MultiNode& net,
        std::size_t snapshotNode,
        std::vector<jtx::Account> accounts,
        std::uint64_t seed = defaultSeed)
        : engine_(seed), accounts_(std::move(accounts))
    {
        if (accounts_.size() < 2)
            throw std::logic_error(
                "traffic::Generator: need at least two accounts");

        snapshotSequences(net, snapshotNode);
    }

    [[nodiscard]] std::vector<jtx::Account> const&
    accounts() const
    {
        return accounts_;
    }

    [[nodiscard]] std::size_t
    size() const
    {
        return accounts_.size();
    }

    TrafficTx
    payment(MultiNode& net, std::size_t node, XRPAmount lo, XRPAmount hi)
    {
        if (accounts_.size() < 2)
            throw std::logic_error(
                "traffic::Generator::payment: need at least two accounts");

        auto const [srcIndex, dstIndex] = distinctPair();
        return paymentFrom(net, node, srcIndex, dstIndex, lo, hi);
    }

    std::vector<TrafficTx>
    burst(
        MultiNode& net,
        std::size_t node,
        std::size_t count,
        XRPAmount lo,
        XRPAmount hi)
    {
        if (accounts_.size() < 2)
            throw std::logic_error(
                "traffic::Generator::burst: need at least two accounts");

        std::vector<TrafficTx> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            out.push_back(payment(net, node, lo, hi));
        return out;
    }

    std::vector<TrafficTx>
    roundRobinBurst(
        MultiNode& net,
        std::size_t node,
        std::size_t count,
        XRPAmount lo,
        XRPAmount hi,
        std::vector<std::size_t>& sentBySource,
        std::size_t maxPerSource,
        SubmitResultPolicy resultPolicy = SubmitResultPolicy::CheckImmediate)
    {
        if (accounts_.size() < 2)
            throw std::logic_error(
                "traffic::Generator::roundRobinBurst: need at least two "
                "accounts");
        if (maxPerSource == 0)
            throw std::logic_error(
                "traffic::Generator::roundRobinBurst: maxPerSource is zero");
        if (sentBySource.size() != accounts_.size())
            throw std::logic_error(
                "traffic::Generator::roundRobinBurst: source counter size "
                "mismatch");

        std::size_t available = 0;
        for (auto const sent : sentBySource)
            if (sent < maxPerSource)
                available += maxPerSource - sent;
        if (count > available)
            throw std::logic_error(
                "traffic::Generator::roundRobinBurst: not enough available "
                "sources");

        std::vector<TrafficTx> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto const srcIndex =
                nextRoundRobinSource(sentBySource, maxPerSource);
            ++sentBySource[srcIndex];
            out.push_back(paymentFrom(
                net,
                node,
                srcIndex,
                randomDestination(srcIndex),
                lo,
                hi,
                resultPolicy));
        }
        return out;
    }

private:
    beast::xor_shift_engine engine_;
    std::vector<jtx::Account> accounts_;
    std::vector<std::uint32_t> nextSeq_;
    std::size_t rrIndex_ = 0;

    [[nodiscard]] static bool
    ownsSequence(TER result)
    {
        // TxQ admission returns terQUEUED only after it holds the transaction.
        // Other saturation results such as telCAN_NOT_QUEUE_* or retry-class
        // ters seen during later queue-drain attempts do not prove that this
        // submit owns the next sequence, so keep them hard failures.
        return result == tesSUCCESS || result == terQUEUED;
    }

    void
    snapshotSequences(MultiNode& net, std::size_t snapshotNode)
    {
        auto const seq = net.minValidated();
        auto const ledger = net.ledger(snapshotNode, seq);
        if (!ledger)
            throw std::logic_error(
                "traffic::Generator: funding ledger unavailable");

        nextSeq_.clear();
        nextSeq_.reserve(accounts_.size());
        for (auto const& account : accounts_)
        {
            auto const sle = ledger->read(keylet::account(account.id()));
            if (!sle)
                throw std::logic_error(
                    "traffic::Generator: missing funded account " +
                    account.name());
            nextSeq_.push_back(sle->getFieldU32(sfSequence));
        }
    }

    [[nodiscard]] std::pair<std::size_t, std::size_t>
    distinctPair()
    {
        auto const n = accounts_.size();
        auto const src = static_cast<std::size_t>(engine_() % n);
        auto dst = randomDestination(src);
        return {src, dst};
    }

    [[nodiscard]] std::size_t
    randomDestination(std::size_t src)
    {
        auto const n = accounts_.size();
        auto dst = static_cast<std::size_t>(engine_() % (n - 1));
        if (dst >= src)
            ++dst;
        return dst;
    }

    [[nodiscard]] std::size_t
    nextRoundRobinSource(
        std::vector<std::size_t> const& sentBySource,
        std::size_t maxPerSource)
    {
        auto const n = accounts_.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const src = (rrIndex_ + i) % n;
            if (sentBySource[src] < maxPerSource)
            {
                rrIndex_ = (src + 1) % n;
                return src;
            }
        }
        throw std::logic_error(
            "traffic::Generator::roundRobinBurst: no available source");
    }

    TrafficTx
    paymentFrom(
        MultiNode& net,
        std::size_t node,
        std::size_t srcIndex,
        std::size_t dstIndex,
        XRPAmount lo,
        XRPAmount hi,
        SubmitResultPolicy resultPolicy = SubmitResultPolicy::CheckImmediate)
    {
        auto const amount = randomAmount(lo, hi);
        auto const sequence = nextSeq_[srcIndex];
        auto const& src = accounts_[srcIndex];
        auto const& dst = accounts_[dstIndex];

        auto tx = jtx::pay(src, dst, jtx::PrettyAmount{amount});
        tx[jss::Sequence] = sequence;

        auto submitted = net.submit(node, std::move(tx), src);
        auto const id = submitted->getID();
        auto const result = submitted->getResult();
        if (resultPolicy == SubmitResultPolicy::CheckImmediate)
        {
            if (!ownsSequence(result))
                throw std::logic_error(
                    "traffic::Generator::payment: hard submit result " +
                    transToken(result) + " for " + src.name() + " sequence " +
                    std::to_string(sequence));
        }
        ++nextSeq_[srcIndex];
        return TrafficTx{
            std::move(submitted), id, src, dst, amount, sequence, result};
    }

    [[nodiscard]] XRPAmount
    randomAmount(XRPAmount lo, XRPAmount hi)
    {
        if (lo.drops() <= 0 || hi.drops() <= 0)
            throw std::logic_error(
                "traffic::Generator: payment amounts must be positive");
        if (hi < lo)
            throw std::logic_error(
                "traffic::Generator: amount high bound is below low bound");

        auto const loDrops = lo.drops();
        auto const hiDrops = hi.drops();
        auto const span = static_cast<std::uint64_t>(hiDrops - loDrops) + 1;
        if (span == 0)
            throw std::logic_error("traffic::Generator: amount span overflow");
        return XRPAmount{
            loDrops + static_cast<XRPAmount::value_type>(engine_() % span)};
    }
};

}  // namespace ripple::test::traffic
