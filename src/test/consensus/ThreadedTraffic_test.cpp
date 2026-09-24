//------------------------------------------------------------------------------
// ThreadedTraffic -- seeded tx traffic over the threaded virtual-time harness.
//
// This is intentionally safety-only: inputs are seeded, but inclusion timing
// and TxQ/consensus interleavings are real-thread nondeterministic.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/Traffic.h>
#include <test/jtx/noop.h>

#include <xrpld/app/misc/TxQ.h>
#include <xrpld/overlay/Overlay.h>

#include <xrpld/core/ConfigSections.h>
#include <xrpld/net/HTTPClientSSLContext.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TER.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ripple::test {

class ThreadedTraffic_test : public beast::unit_test::suite
{
    struct RunOutcome
    {
        std::size_t formationBeats = 0;
        std::size_t fundingBeats = 0;
        std::size_t scenarioBeats = 0;
        std::chrono::milliseconds maxDrain{0};
        std::chrono::milliseconds totalDrain{0};
        std::size_t maxPolls = 0;
        std::size_t maxQuietStreak = 0;
        std::size_t maxBufferedBytes = 0;
        std::size_t maxBusyJobQueues = 0;
        int maxSuspended = 0;
        bool sawBufferedBytes = false;
        bool sawTransportPosts = false;
        bool sawJobWork = false;
        bool trafficSawBufferedBytes = false;
        bool trafficSawTransportPosts = false;
        bool trafficSawJobWork = false;
        std::uint64_t readStarted = 0;
        std::uint64_t writeStarted = 0;
        std::uint64_t trafficReadStarted = 0;
        std::uint64_t trafficWriteStarted = 0;
        std::size_t payments = 0;
        std::uint32_t finalSeq = 0;
        std::size_t queuedSubmits = 0;
        std::size_t queueSamples = 0;
        std::size_t queueHitSamples = 0;
        std::size_t maxQueueDepth = 0;
        std::size_t maxOpenLedgerTx = 0;
    };

    struct ValidationCheck
    {
        bool allPresent = false;
        bool allTesSuccess = true;
        std::size_t remaining = 0;
    };

    struct RunConfig
    {
        std::size_t validators = 3;
        std::size_t accounts = 8;
        std::uint32_t formationTarget = 3;
        std::size_t maxFormationBeats = 120;
        std::size_t maxFundingBeats = 120;
        std::size_t maxScenarioBeats = 120;
        std::size_t trafficBeats = 8;
        std::size_t burstsPerBeat = 1;
        std::size_t burstSize = 3;
        std::size_t maxPerSourcePerBeat = 1;
        bool txqPressure = false;
    };

    static constexpr std::uint64_t kTrafficSeed = 0x5452414646494331ull;

    [[nodiscard]] static bool
    defaultGeneratorShape(RunConfig const& config)
    {
        return config.validators == 3 && config.accounts == 8 &&
            config.burstsPerBeat == 1 && config.burstSize == 3 &&
            config.maxPerSourcePerBeat == 1 && !config.txqPressure;
    }

    [[nodiscard]] static std::size_t
    maxBurstsPerTargetPerBeat(RunConfig const& config)
    {
        return (config.burstsPerBeat + config.validators - 1) /
            config.validators;
    }

    [[nodiscard]] static OverlayFactory
    simOverlayFactory()
    {
        return [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
    }

    [[nodiscard]] static ConfigHook
    txqPressureConfigHook()
    {
        return [](Config& cfg) {
            auto& section = cfg.section("transaction_queue");
            section.set("ledgers_in_queue", "8");
            section.set("minimum_queue_size", "200");
            section.set("minimum_txn_in_ledger", "1");
            section.set("minimum_txn_in_ledger_standalone", "1");
            section.set("target_txn_in_ledger", "1");
            section.set("maximum_txn_in_ledger", "2");
            section.set("maximum_txn_per_account", "16");
            section.set("retry_sequence_percent", "25");
            section.set("normal_consensus_increase_percent", "0");
        };
    }

    [[nodiscard]] static ConfigHook
    parentHashCompConfigHook()
    {
        return [](Config& cfg) {
            auto& section = cfg.section("transaction_queue");
            section.set("minimum_txn_in_ledger", "3");
            section.set("minimum_txn_in_ledger_standalone", "3");
            section.set("target_txn_in_ledger", "3");
            section.set("maximum_txn_in_ledger", "3");
            section.set("minimum_queue_size", "50000");
            section.set("maximum_txn_per_account", "50000");
            section.set("retry_sequence_percent", "25");
            section.set("normal_consensus_increase_percent", "0");
        };
    }

    [[nodiscard]] static ConfigHook
    sslVerifyConfigHook(bool sslVerify)
    {
        return [sslVerify](Config& cfg) {
            cfg.SSL_VERIFY = sslVerify;
            cfg.SSL_VERIFY_DIR.clear();
            cfg.SSL_VERIFY_FILE.clear();
        };
    }

    [[nodiscard]] static char const*
    yesno(bool value)
    {
        return value ? "true" : "false";
    }

    [[nodiscard]] std::size_t
    runsArg(std::size_t fallback) const
    {
        auto const& a = arg();
        std::string v;
        if (auto const pos = a.find("runs="); pos != std::string::npos)
            v = a.substr(pos + 5);
        else if (
            !a.empty() &&
            a.find_first_not_of("0123456789") == std::string::npos)
            v = a;

        std::size_t n = 0;
        for (char const c : v)
        {
            if (c < '0' || c > '9')
                break;
            n = n * 10 + static_cast<std::size_t>(c - '0');
        }
        return n == 0 ? fallback : n;
    }

    [[nodiscard]] std::size_t
    sizeArg(std::string const& name, std::size_t fallback) const
    {
        auto const& a = arg();
        for (std::size_t start = 0; start < a.size();)
        {
            auto end = a.find(',', start);
            if (end == std::string::npos)
                end = a.size();
            auto const eq = a.find('=', start);
            if (eq != std::string::npos && eq < end &&
                a.compare(start, eq - start, name) == 0)
            {
                std::size_t n = 0;
                bool sawDigit = false;
                for (auto i = eq + 1; i < end; ++i)
                {
                    char const c = a[i];
                    if (c < '0' || c > '9')
                        return fallback;
                    sawDigit = true;
                    n = n * 10 + static_cast<std::size_t>(c - '0');
                }
                return sawDigit ? n : fallback;
            }
            start = end + 1;
        }
        return fallback;
    }

    [[nodiscard]] std::string
    stringArg(std::string const& name, std::string fallback) const
    {
        auto const& a = arg();
        for (std::size_t start = 0; start < a.size();)
        {
            auto end = a.find(',', start);
            if (end == std::string::npos)
                end = a.size();
            auto const eq = a.find('=', start);
            if (eq != std::string::npos && eq < end &&
                a.compare(start, eq - start, name) == 0)
            {
                return a.substr(eq + 1, end - eq - 1);
            }
            start = end + 1;
        }
        return fallback;
    }

    void
    noteSubmitResults(
        RunOutcome& out,
        std::vector<traffic::TrafficTx> const& burst)
    {
        for (auto const& r : burst)
            if (r.submitResult == terQUEUED)
                ++out.queuedSubmits;
    }

    void
    noteTxQMetrics(MultiNode& net, RunOutcome& out)
    {
        for (std::uint32_t i = 0; i < net.size(); ++i)
        {
            if (!net.isLive(i))
                continue;

            auto& app = net[i].app();
            auto const view = app.openLedger().current();
            auto const metrics = app.getTxQ().getMetrics(*view);
            ++out.queueSamples;
            if (metrics.txCount > 0)
                ++out.queueHitSamples;
            out.maxQueueDepth = std::max(out.maxQueueDepth, metrics.txCount);
            out.maxOpenLedgerTx =
                std::max(out.maxOpenLedgerTx, metrics.txInLedger);
        }
    }

    [[nodiscard]] static std::uint32_t
    accountSeq(Application& app, jtx::Account const& account)
    {
        auto const view = app.openLedger().current();
        auto const sle = view->read(keylet::account(account.id()));
        if (!sle)
            throw std::logic_error(
                "ThreadedTraffic accountSeq: missing account root");
        return sle->getFieldU32(sfSequence);
    }

    [[nodiscard]] static std::uint64_t
    baseFeeDrops(Application& app)
    {
        auto const view = app.openLedger().current();
        return view->fees().base.drops();
    }

    [[nodiscard]] static LedgerHash
    openLedgerParentHash(Application& app)
    {
        return app.openLedger().current()->info().parentHash;
    }

    [[nodiscard]] static std::shared_ptr<Ledger const>
    makeClosedDescendant(Application& app, std::size_t generations)
    {
        auto current = app.getLedgerMaster().getClosedLedger();
        for (std::size_t i = 0; i < generations; ++i)
        {
            auto next = std::make_shared<Ledger>(
                *current, app.timeKeeper().closeTime());
            next->setImmutable(true);
            current = std::move(next);
        }
        return current;
    }

    static void
    installOpenLedgerParent(
        Application& app,
        std::shared_ptr<Ledger const> const& ledger)
    {
        OrderedTxs locals{uint256{}};
        OrderedTxs retries{uint256{}};
        app.openLedger().accept(
            app,
            ledger->rules(),
            ledger,
            locals,
            /*retriesFirst=*/false,
            retries,
            tapNONE,
            "txq-parenthashcomp-oracle");
    }

    static void
    repopulateTxQWithOpenLedgerParent(Application& app)
    {
        app.openLedger().modify([&app](OpenView& view, beast::Journal) {
            app.getTxQ().accept(app, view);
            return false;
        });
    }

    [[nodiscard]] static std::vector<uint256>
    queuedTxIds(Application& app)
    {
        auto const txs = app.getTxQ().getTxs();
        std::vector<uint256> ids;
        ids.reserve(txs.size());
        for (auto const& tx : txs)
            ids.push_back(tx.txn->getTransactionID());
        return ids;
    }

    [[nodiscard]] static std::vector<uint256>
    sortedByParentHash(std::vector<uint256> ids, LedgerHash const& parentHash)
    {
        std::sort(
            ids.begin(),
            ids.end(),
            [&parentHash](uint256 const& a, uint256 const& b) {
                return (a ^ parentHash) < (b ^ parentHash);
            });
        return ids;
    }

    [[nodiscard]] static bool
    sameOrder(std::vector<uint256> const& lhs, std::vector<uint256> const& rhs)
    {
        return lhs.size() == rhs.size() &&
            std::equal(lhs.begin(), lhs.end(), rhs.begin());
    }

    [[nodiscard]] static std::string
    shortIds(std::vector<uint256> const& ids)
    {
        std::ostringstream os;
        char const* sep = "";
        for (auto const& id : ids)
        {
            os << sep << to_string(id).substr(0, 8);
            sep = ",";
        }
        return os.str();
    }

    [[nodiscard]] static Json::Value
    sequencedNoop(
        jtx::Account const& account,
        std::uint32_t sequence,
        std::uint64_t feeDrops)
    {
        auto tx = jtx::noop(account);
        tx[jss::Sequence] = sequence;
        tx[jss::Fee] = std::to_string(feeDrops);
        return tx;
    }

    [[nodiscard]] static TER
    submitSequencedNoop(
        MultiNode& net,
        std::size_t node,
        jtx::Account const& account,
        std::uint32_t sequence,
        std::uint64_t feeDrops)
    {
        auto const txn = net.submit(
            node, sequencedNoop(account, sequence, feeDrops), account);
        return txn->getResult();
    }

    [[nodiscard]] static std::vector<jtx::Account>
    makeTxQAccounts(std::string const& prefix, std::size_t count)
    {
        std::vector<jtx::Account> accounts;
        accounts.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            accounts.emplace_back(prefix + std::to_string(i));
        return accounts;
    }

    static void
    seedAccountRoots(
        MultiNode& net,
        std::size_t node,
        std::vector<jtx::Account> const& accounts,
        XRPAmount balance)
    {
        auto& app = net[node].app();
        app.openLedger().modify([&](OpenView& view, beast::Journal) {
            auto changed = false;
            for (auto const& account : accounts)
            {
                if (view.read(keylet::account(account.id())))
                    continue;

                auto sle = std::make_shared<SLE>(keylet::account(account.id()));
                sle->setFieldU32(sfSequence, 1);
                sle->setAccountID(sfAccount, account.id());
                sle->setFieldAmount(sfBalance, balance);
                view.rawInsert(sle);
                changed = true;
            }
            return changed;
        });
    }

    static void
    fillOpenLedger(MultiNode& net, std::size_t node)
    {
        auto& app = net[node].app();
        auto const metrics =
            app.getTxQ().getMetrics(*app.openLedger().current());
        auto seq = accountSeq(app, jtx::Account::master);
        auto const fee = baseFeeDrops(app);
        for (std::size_t i = metrics.txInLedger; i <= metrics.txPerLedger; ++i)
        {
            auto const result = submitSequencedNoop(
                net, node, jtx::Account::master, seq++, fee);
            if (result != tesSUCCESS && result != terQUEUED)
                throw std::logic_error(
                    "ThreadedTraffic fillOpenLedger: noop returned " +
                    transToken(result));
        }
    }

    void
    noteTick(RunOutcome& out, MultiNode::ThreadedTickStats const& tick)
    {
        out.maxDrain = std::max(out.maxDrain, tick.wallElapsed);
        out.totalDrain += tick.wallElapsed;
        out.maxPolls = std::max(out.maxPolls, tick.polls);
        out.maxQuietStreak = std::max(out.maxQuietStreak, tick.maxQuietStreak);
        out.maxBufferedBytes =
            std::max(out.maxBufferedBytes, tick.maxBufferedBytes);
        out.maxBusyJobQueues =
            std::max(out.maxBusyJobQueues, tick.maxBusyJobQueues);
        out.maxSuspended = std::max(out.maxSuspended, tick.maxSuspended);
        out.sawBufferedBytes = out.sawBufferedBytes || tick.sawBufferedBytes;
        out.sawTransportPosts = out.sawTransportPosts || tick.sawTransportPosts;
        out.sawJobWork = out.sawJobWork || tick.sawJobWork;
        out.readStarted +=
            tick.transportEnd.readStarted - tick.transportStart.readStarted;
        out.writeStarted +=
            tick.transportEnd.writeStarted - tick.transportStart.writeStarted;
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

    [[nodiscard]] ValidationCheck
    validatedBy(
        MultiNode& net,
        std::vector<traffic::TrafficTx> const& records,
        std::uint32_t seq)
    {
        std::set<uint256> remaining;
        for (auto const& r : records)
            remaining.insert(r.id);

        bool allTes = true;
        for (std::uint32_t s = 2; s <= seq; ++s)
        {
            for (auto const& tx : net.appliedTxs(0, s))
            {
                if (remaining.erase(tx.txid) != 0 && tx.result != tesSUCCESS)
                    allTes = false;
            }
        }

        return ValidationCheck{remaining.empty(), allTes, remaining.size()};
    }

    [[nodiscard]] std::size_t
    fundThreadedSerial(
        MultiNode& net,
        std::size_t node,
        std::vector<jtx::Account> const& accounts,
        jtx::PrettyAmount const& amount,
        RunConfig const& config,
        MultiNode::ThreadedTickOptions const& tickOptions,
        RunOutcome& out,
        std::chrono::milliseconds runTotalTimeout)
    {
        if (!net.isLive(node))
            throw std::logic_error("fundThreadedSerial: node is not live");

        std::vector<jtx::Account> funded;
        funded.reserve(accounts.size());
        std::size_t beats = 0;
        for (auto const& account : accounts)
        {
            auto const txn = net.submit(
                node,
                jtx::pay(jtx::Account::master, account, amount),
                jtx::Account::master);
            if (txn->getResult() != tesSUCCESS)
                throw std::logic_error(
                    "fundThreadedSerial: pay(" + account.name() +
                    ") not applied: " + transToken(txn->getResult()));

            funded.push_back(account);
            while (
                !traffic::allAccountsValidated(net, funded, net.minValidated()))
            {
                if (beats >= config.maxFundingBeats)
                    throw std::logic_error(
                        "fundThreadedSerial: funding did not validate within " +
                        std::to_string(config.maxFundingBeats) + " beats");

                auto const tick =
                    net.threadedTick(std::chrono::seconds{1}, tickOptions);
                noteTick(out, tick);
                ++beats;

                if (out.totalDrain > runTotalTimeout)
                    throw std::logic_error(
                        "fundThreadedSerial: funding drain budget exceeded");
                if (!net.validatedForkFree())
                    throw std::logic_error(
                        "fundThreadedSerial: validated fork during funding");
            }
        }
        return beats;
    }

    void
    logThreadedState(MultiNode& net, char const* label)
    {
        auto const activity = net.simActivitySnapshot();
        log << "  " << label << ": pipeBytes=" << net.simBufferedBytes()
            << " posts=" << activity.inFlightPosts
            << " epoch=" << activity.epoch << " seqs=[";
        char const* sep = "";
        for (std::uint32_t i = 0; i < net.size(); ++i)
        {
            if (!net.isLive(i))
                continue;
            auto& jq = net[i].app().getJobQueue();
            auto const view = net[i].app().openLedger().current();
            auto const metrics = net[i].app().getTxQ().getMetrics(*view);
            log << sep << "n" << i << "{closed=" << net.closedSeq(i)
                << ",valid=" << net.validSeq(i)
                << ",idle=" << (jq.isIdle() ? "true" : "false")
                << ",suspended=" << jq.suspendedCount()
                << ",queue=" << metrics.txCount
                << ",openTx=" << metrics.txInLedger << "}";
            sep = ",";
        }
        log << "]" << std::endl;
    }

    [[nodiscard]] std::optional<RunOutcome>
    runThreadedTraffic(
        std::size_t run,
        RunConfig const& config,
        MultiNode::ThreadedTickOptions const& tickOptions,
        std::chrono::milliseconds runTotalTimeout)
    {
        using namespace std::chrono_literals;

        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/false);

        std::vector<ValidatorKey> keys;
        std::vector<std::string> unl;
        keys.reserve(config.validators);
        unl.reserve(config.validators);
        for (std::size_t i = 0; i < config.validators; ++i)
        {
            keys.push_back(ValidatorKey::fromPassphrase(
                "threaded-traffic-" + std::to_string(i)));
            unl.push_back(keys.back().pubKey);
        }

        auto factory = simOverlayFactory();
        auto configHook =
            config.txqPressure ? txqPressureConfigHook() : ConfigHook{};
        for (auto const& key : keys)
            net.add(
                TrustConfig{key.seed, unl},
                factory,
                JobQueue::DispatchHook{},
                /*bindServerListeners=*/false,
                configHook);
        if (!BEAST_EXPECT(net.allUp()))
            return std::nullopt;

        for (std::size_t i = 0; i < config.validators; ++i)
            for (std::size_t j = i + 1; j < config.validators; ++j)
                if (!BEAST_EXPECT(net.simConnect(i, j) != nullptr))
                    return std::nullopt;
        if (!BEAST_EXPECT(net.waitForPeers(config.validators - 1, 20s)))
            return std::nullopt;

        RunOutcome out;
        try
        {
            while (out.formationBeats < config.maxFormationBeats &&
                   net.minValidated() < config.formationTarget)
            {
                noteTick(out, net.threadedTick(1s, tickOptions));
                ++out.formationBeats;
                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return std::nullopt;
                if (!BEAST_EXPECTS(
                        out.totalDrain <= runTotalTimeout,
                        "threaded traffic formation drain budget exceeded"))
                {
                    logThreadedState(net, "formation drain budget failure");
                    return std::nullopt;
                }
            }

            if (!BEAST_EXPECT(net.minValidated() >= config.formationTarget))
            {
                logThreadedState(net, "formation stalled");
                return std::nullopt;
            }

            auto accounts = traffic::makeAccounts(config.accounts);
            if (config.txqPressure)
            {
                out.fundingBeats = fundThreadedSerial(
                    net,
                    0,
                    accounts,
                    jtx::XRP(1000),
                    config,
                    tickOptions,
                    out,
                    runTotalTimeout);
            }
            else
            {
                traffic::ThreadedFundOptions fundOptions;
                fundOptions.maxBeats = config.maxFundingBeats;
                fundOptions.tickOptions = tickOptions;
                fundOptions.onTick = [&](auto const& tick) {
                    noteTick(out, tick);
                    if (out.totalDrain > runTotalTimeout)
                        throw std::logic_error(
                            "threaded traffic funding drain budget exceeded");
                };
                out.fundingBeats = traffic::fundThreaded(
                    net, 0, accounts, jtx::XRP(1000), fundOptions);
            }

            std::vector<traffic::TrafficTx> records;
            records.reserve(
                config.trafficBeats * config.burstsPerBeat * config.burstSize);
            RunOutcome trafficActivity;
            std::set<std::size_t> targetNodes;

            auto const noteScenarioBeat = [&]() {
                auto const tick = net.threadedTick(1s, tickOptions);
                noteTick(out, tick);
                noteTick(trafficActivity, tick);
                ++out.scenarioBeats;

                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return false;
                if (!BEAST_EXPECTS(
                        out.totalDrain <= runTotalTimeout,
                        "threaded traffic cumulative drain budget exceeded"))
                {
                    logThreadedState(net, "drain budget failure");
                    return false;
                }
                return true;
            };

            if (defaultGeneratorShape(config))
            {
                traffic::Generator gen(
                    net, 0, std::move(accounts), kTrafficSeed + run);

                for (std::size_t beat = 0; beat < config.trafficBeats; ++beat)
                {
                    auto const target =
                        (beat * config.burstsPerBeat) % net.size();
                    targetNodes.insert(target);
                    auto burst = gen.burst(
                        net,
                        target,
                        config.burstSize,
                        XRPAmount{10},
                        XRPAmount{1'000'000});
                    noteSubmitResults(out, burst);
                    records.insert(records.end(), burst.begin(), burst.end());

                    if (!noteScenarioBeat())
                        return std::nullopt;
                }
            }
            else
            {
                std::vector<std::unique_ptr<traffic::Generator>> generators;
                generators.reserve(net.size());
                for (std::size_t target = 0; target < net.size(); ++target)
                {
                    std::vector<jtx::Account> group;
                    for (std::size_t i = target; i < accounts.size();
                         i += net.size())
                    {
                        group.push_back(accounts[i]);
                    }
                    generators.push_back(std::make_unique<traffic::Generator>(
                        net,
                        0,
                        std::move(group),
                        kTrafficSeed + run +
                            (target + 1) * 0x9E3779B97F4A7C15ull));
                }

                using SubmitResultPolicy =
                    traffic::Generator::SubmitResultPolicy;
                auto constexpr deferSubmitResult =
                    SubmitResultPolicy::DeferToValidatedLedger;
                auto const sourceLimit = config.maxPerSourcePerBeat;
                for (std::size_t beat = 0; beat < config.trafficBeats; ++beat)
                {
                    std::vector<std::vector<std::size_t>> sentByTarget;
                    sentByTarget.reserve(generators.size());
                    for (auto const& generator : generators)
                        sentByTarget.emplace_back(generator->size(), 0);

                    for (std::size_t burstIndex = 0;
                         burstIndex < config.burstsPerBeat;
                         ++burstIndex)
                    {
                        auto const target =
                            (beat * config.burstsPerBeat + burstIndex) %
                            net.size();
                        targetNodes.insert(target);
                        // The scaled threaded path stresses async TxQ and
                        // held-tx retries. Transaction::result_ is mutable
                        // batch state, so do not sample it here as a sequence
                        // oracle; the final ledger scan below requires every
                        // generated tx to validate as tesSUCCESS.
                        auto burst = generators[target]->roundRobinBurst(
                            net,
                            target,
                            config.burstSize,
                            XRPAmount{10},
                            XRPAmount{1'000'000},
                            sentByTarget[target],
                            sourceLimit,
                            deferSubmitResult);
                        noteSubmitResults(out, burst);
                        records.insert(
                            records.end(), burst.begin(), burst.end());
                        if (config.txqPressure)
                            noteTxQMetrics(net, out);
                    }

                    if (!noteScenarioBeat())
                        return std::nullopt;
                    if (config.txqPressure)
                        noteTxQMetrics(net, out);
                }
            }

            while (out.scenarioBeats < config.maxScenarioBeats)
            {
                auto const check =
                    validatedBy(net, records, net.minValidated());
                if (check.allPresent)
                    break;

                auto const tick = net.threadedTick(1s, tickOptions);
                noteTick(out, tick);
                noteTick(trafficActivity, tick);
                ++out.scenarioBeats;

                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return std::nullopt;
                if (!BEAST_EXPECTS(
                        out.totalDrain <= runTotalTimeout,
                        "threaded traffic cumulative drain budget exceeded"))
                {
                    logThreadedState(net, "drain budget failure");
                    return std::nullopt;
                }
                if (config.txqPressure)
                    noteTxQMetrics(net, out);
            }

            out.finalSeq = net.minValidated();
            out.payments = records.size();

            auto const check = validatedBy(net, records, out.finalSeq);
            if (!BEAST_EXPECT(check.allPresent))
            {
                log << "  run " << run << ": " << check.remaining
                    << " generated transactions not validated by seq "
                    << out.finalSeq << std::endl;
                logThreadedState(net, "missing traffic");
                return std::nullopt;
            }
            if (!BEAST_EXPECT(check.allTesSuccess))
                return std::nullopt;
            if (!BEAST_EXPECT(net.ledgersAgree(out.finalSeq)))
            {
                logThreadedState(net, "ledger disagreement");
                return std::nullopt;
            }
            if (!BEAST_EXPECT(net.validatedForkFree()))
            {
                logThreadedState(net, "final fork failure");
                return std::nullopt;
            }
            if (!expectRecordCoherence(records))
                return std::nullopt;
            if (config.txqPressure)
            {
                if (!BEAST_EXPECTS(
                        out.queuedSubmits > 0,
                        "txq pressure mode must observe terQUEUED submits"))
                {
                    logThreadedState(net, "txq pressure no terQUEUED");
                    return std::nullopt;
                }
                if (!BEAST_EXPECTS(
                        out.maxQueueDepth > 0,
                        "txq pressure mode must sample non-empty TxQ depth"))
                {
                    logThreadedState(net, "txq pressure no queue depth");
                    return std::nullopt;
                }
            }
            if (!BEAST_EXPECT(targetNodes.size() == net.size()))
            {
                log << "  run " << run << ": traffic targeted "
                    << targetNodes.size() << "/" << net.size() << " nodes"
                    << std::endl;
                return std::nullopt;
            }

            out.trafficSawBufferedBytes = trafficActivity.sawBufferedBytes;
            out.trafficSawTransportPosts = trafficActivity.sawTransportPosts;
            out.trafficSawJobWork = trafficActivity.sawJobWork;
            out.trafficReadStarted = trafficActivity.readStarted;
            out.trafficWriteStarted = trafficActivity.writeStarted;
            bool trafficActivityOk = true;
            trafficActivityOk =
                BEAST_EXPECT(out.trafficSawBufferedBytes) && trafficActivityOk;
            trafficActivityOk =
                BEAST_EXPECT(out.trafficSawTransportPosts) && trafficActivityOk;
            trafficActivityOk =
                BEAST_EXPECT(out.trafficSawJobWork) && trafficActivityOk;
            trafficActivityOk =
                BEAST_EXPECT(out.trafficReadStarted > 0) && trafficActivityOk;
            trafficActivityOk =
                BEAST_EXPECT(out.trafficWriteStarted > 0) && trafficActivityOk;
            if (!trafficActivityOk)
            {
                logThreadedState(net, "traffic activity failure");
                return std::nullopt;
            }

            BEAST_EXPECT(net.waitForSimQuiescence(tickOptions.stallTimeout));
            auto const activity = net.simActivitySnapshot();
            out.maxBufferedBytes = std::max<std::size_t>(
                out.maxBufferedBytes,
                static_cast<std::size_t>(activity.maxBufferedBytes));

            BEAST_EXPECT(net.simBufferedBytes() == 0);
            BEAST_EXPECT(activity.inFlightPosts == 0);
            return out;
        }
        catch (std::exception const& e)
        {
            log << "  run " << run
                << " threaded traffic exception: " << e.what() << std::endl;
            logThreadedState(net, "exception");
            BEAST_EXPECT(false);
            return std::nullopt;
        }
    }

    void
    testTxQParentHashCompRung0()
    {
        testcase(
            "TxQ parentHashComp raw MultiNode control without consensus "
            "ticking or RPC fibers");

        using namespace std::chrono;

        auto const iterations = sizeArg("iterations", 5000);

        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/true);

        std::vector<ValidatorKey> keys;
        std::vector<std::string> unl;
        keys.reserve(2);
        unl.reserve(2);
        for (std::size_t i = 0; i < 2; ++i)
        {
            keys.push_back(ValidatorKey::fromPassphrase(
                "txq-parenthashcomp-" + std::to_string(i)));
            unl.push_back(keys.back().pubKey);
        }

        auto factory = simOverlayFactory();
        auto configHook = parentHashCompConfigHook();
        for (auto const& key : keys)
            net.add(
                TrustConfig{key.seed, unl},
                factory,
                JobQueue::DispatchHook{},
                /*bindServerListeners=*/false,
                configHook);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        if (!BEAST_EXPECTS(
                iterations > 1,
                "txqParentHashComp iterations must be greater than 1"))
            return;

        static constexpr std::size_t readerPreseed = 1;
        auto const writerAccounts =
            makeTxQAccounts("txq-rung0-multinode-writer-", 16);
        auto const readerAccounts = makeTxQAccounts(
            "txq-rung0-multinode-reader-", iterations + readerPreseed);
        try
        {
            seedAccountRoots(
                net, 0, writerAccounts, XRPAmount{50'000'000'000'000});
            seedAccountRoots(
                net, 1, readerAccounts, XRPAmount{50'000'000'000'000});
            fillOpenLedger(net, 0);
            fillOpenLedger(net, 1);

            auto const writerFee = baseFeeDrops(net[0].app()) * 10;
            for (auto const& account : writerAccounts)
            {
                auto const result =
                    submitSequencedNoop(net, 0, account, 1, writerFee);
                if (!BEAST_EXPECTS(
                        result == terQUEUED,
                        "writer setup submit returned " + transToken(result)))
                    return;
            }

            // Seed one same-fee reader candidate so later inserts must compare
            // against an existing byFee_ entry instead of growing an empty set.
            auto const readerFee = baseFeeDrops(net[1].app()) * 10;
            auto const result = submitSequencedNoop(
                net, 1, readerAccounts.front(), 1, readerFee);
            if (!BEAST_EXPECTS(
                    result == terQUEUED,
                    "reader preseed submit returned " + transToken(result)))
                return;
        }
        catch (std::exception const& e)
        {
            BEAST_EXPECTS(false, e.what());
            return;
        }

        auto const writerQueueBefore =
            net[0]
                .app()
                .getTxQ()
                .getMetrics(*net[0].app().openLedger().current())
                .txCount;
        auto const readerQueueBefore =
            net[1]
                .app()
                .getTxQ()
                .getMetrics(*net[1].app().openLedger().current())
                .txCount;
        auto const writerParentHash = openLedgerParentHash(net[0].app());
        auto const readerParentHash = openLedgerParentHash(net[1].app());

        // Equal hashes mean this rung is only a TSan/noise control, not a
        // deterministic wrong-salt semantic oracle.
        std::atomic<bool> start{false};
        std::atomic<bool> stop{false};
        std::atomic<bool> insertActive{false};
        std::atomic<std::size_t> acceptCalls{0};
        std::atomic<std::size_t> acceptChanged{0};
        std::atomic<std::size_t> acceptDuringInsert{0};
        std::atomic<std::size_t> acceptDuringQueuedInserts{0};
        std::atomic<std::size_t> queuedSubmits{0};
        std::atomic<std::size_t> unexpectedSubmits{0};
        std::mutex errorMutex;
        std::string error;
        constexpr auto relaxed = std::memory_order_relaxed;

        auto recordError = [&](std::string message) {
            {
                std::scoped_lock const lock(errorMutex);
                if (error.empty())
                    error = std::move(message);
            }
            stop.store(true, relaxed);
        };

        auto const t0 = steady_clock::now();
        std::thread acceptThread([&] {
            try
            {
                auto& app = net[0].app();
                while (!start.load(relaxed))
                    std::this_thread::yield();

                while (!stop.load(relaxed))
                {
                    auto changed = false;
                    app.openLedger().modify(
                        [&](OpenView& view, beast::Journal) {
                            changed = app.getTxQ().accept(app, view);
                            return changed;
                        });
                    acceptCalls.fetch_add(1, relaxed);
                    if (changed)
                        acceptChanged.fetch_add(1, relaxed);
                    if (insertActive.load(relaxed))
                    {
                        acceptDuringInsert.fetch_add(1, relaxed);
                        auto const queued = queuedSubmits.load(relaxed);
                        if (queued > 0 && queued < iterations)
                            acceptDuringQueuedInserts.fetch_add(1, relaxed);
                    }
                }
            }
            catch (std::exception const& e)
            {
                recordError(std::string("accept thread: ") + e.what());
            }
            catch (...)
            {
                recordError("accept thread: unknown exception");
            }
        });

        std::thread insertThread([&] {
            try
            {
                auto const fee = baseFeeDrops(net[1].app()) * 10;
                while (!start.load(relaxed))
                    std::this_thread::yield();

                insertActive.store(true, relaxed);
                for (std::size_t i = 0; i < iterations && !stop.load(relaxed);
                     ++i)
                {
                    auto const result = submitSequencedNoop(
                        net, 1, readerAccounts[i + readerPreseed], 1, fee);
                    if (result == terQUEUED)
                    {
                        queuedSubmits.fetch_add(1, relaxed);
                    }
                    else
                    {
                        unexpectedSubmits.fetch_add(1, relaxed);
                        recordError(
                            "insert thread: submit returned " +
                            transToken(result));
                        break;
                    }
                    if ((i & 0x3f) == 0)
                        std::this_thread::yield();
                }
                insertActive.store(false, relaxed);
                stop.store(true, relaxed);
            }
            catch (std::exception const& e)
            {
                insertActive.store(false, relaxed);
                recordError(std::string("insert thread: ") + e.what());
            }
            catch (...)
            {
                insertActive.store(false, relaxed);
                recordError("insert thread: unknown exception");
            }
        });

        start.store(true, relaxed);
        insertThread.join();
        stop.store(true, relaxed);
        acceptThread.join();

        auto const elapsed =
            duration_cast<milliseconds>(steady_clock::now() - t0).count();
        auto const writerMetrics = net[0].app().getTxQ().getMetrics(
            *net[0].app().openLedger().current());
        auto const readerMetrics = net[1].app().getTxQ().getMetrics(
            *net[1].app().openLedger().current());

        std::cout << "  txq-parenthashcomp rung=0 multinode iterations="
                  << iterations << " readerPreseed=" << readerPreseed
                  << " queuedSubmits=" << queuedSubmits.load(relaxed)
                  << " unexpectedSubmits=" << unexpectedSubmits.load(relaxed)
                  << " acceptCalls=" << acceptCalls.load(relaxed)
                  << " acceptChanged=" << acceptChanged.load(relaxed)
                  << " acceptDuringInsert=" << acceptDuringInsert.load(relaxed)
                  << " acceptDuringQueuedInserts="
                  << acceptDuringQueuedInserts.load(relaxed)
                  << " writerQueueBefore=" << writerQueueBefore
                  << " readerQueueBefore=" << readerQueueBefore
                  << " writerQueue=" << writerMetrics.txCount
                  << " readerQueue=" << readerMetrics.txCount
                  << " writerParentHash=" << to_string(writerParentHash)
                  << " readerParentHash=" << to_string(readerParentHash)
                  << " parentHashesEqual="
                  << (writerParentHash == readerParentHash ? "true" : "false")
                  << " elapsedMs=" << elapsed << std::endl;

        BEAST_EXPECTS(error.empty(), error);
        BEAST_EXPECT(queuedSubmits.load(relaxed) == iterations);
        BEAST_EXPECT(unexpectedSubmits.load(relaxed) == 0);
        BEAST_EXPECT(acceptCalls.load(relaxed) > 0);
        BEAST_EXPECT(acceptDuringInsert.load(relaxed) > 0);
        BEAST_EXPECT(acceptDuringQueuedInserts.load(relaxed) > 0);
        BEAST_EXPECT(writerQueueBefore >= 16);
        BEAST_EXPECT(readerQueueBefore == readerPreseed);
        BEAST_EXPECT(writerMetrics.txCount >= writerQueueBefore);
        BEAST_EXPECT(readerMetrics.txCount == readerQueueBefore + iterations);
    }

    void
    testTxQParentHashCompSteppingOracle()
    {
        testcase(
            "TxQ parentHashComp deterministic stepping queue-order oracle");

        constexpr std::size_t readerPreseed = 2;
        constexpr std::size_t maxProbeInserts = 64;

        SteppingNetwork stepping(*this);
        auto& net = stepping.multiNode();
        auto factory = simOverlayFactory();
        auto configHook = parentHashCompConfigHook();
        for (std::size_t i = 0; i < 2; ++i)
            net.add(
                std::nullopt,
                factory,
                JobQueue::DispatchHook{},
                /*bindServerListeners=*/false,
                configHook);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // Deterministically diverge the two real open-ledger parent hashes
        // without exposing TxQ internals. OpenLedger::accept is the production
        // path NetworkOPs uses after accepting a new LCL; the test supplies the
        // closed descendants directly so no consensus scheduling luck is
        // involved.
        auto const writerClosed = makeClosedDescendant(net[0].app(), 1);
        auto const readerClosed = makeClosedDescendant(net[1].app(), 2);
        installOpenLedgerParent(net[0].app(), writerClosed);
        installOpenLedgerParent(net[1].app(), readerClosed);

        auto const writerParentHash = openLedgerParentHash(net[0].app());
        auto const readerParentHash = openLedgerParentHash(net[1].app());
        if (!BEAST_EXPECT(writerParentHash != readerParentHash))
            return;

        auto const readerAccounts = makeTxQAccounts(
            "txq-oracle-reader-", readerPreseed + maxProbeInserts);
        try
        {
            seedAccountRoots(
                net, 1, readerAccounts, XRPAmount{50'000'000'000'000});
            fillOpenLedger(net, 1);
        }
        catch (std::exception const& e)
        {
            BEAST_EXPECTS(false, e.what());
            return;
        }

        auto const readerFee = baseFeeDrops(net[1].app()) * 10;
        repopulateTxQWithOpenLedgerParent(net[1].app());
        for (std::size_t i = 0; i < readerPreseed; ++i)
        {
            auto const result =
                submitSequencedNoop(net, 1, readerAccounts[i], 1, readerFee);
            if (!BEAST_EXPECTS(
                    result == terQUEUED,
                    "reader preseed submit returned " + transToken(result)))
                return;
        }

        auto before = queuedTxIds(net[1].app());
        if (!BEAST_EXPECT(before.size() == readerPreseed))
            return;
        if (!BEAST_EXPECT(sameOrder(
                before, sortedByParentHash(before, readerParentHash))))
            return;

        // Regression trigger: before issue 017's fix, a different TxQ instance
        // rebuilt its queue and rewrote MaybeTx::parentHashComp process-wide.
        // Node1's next same-fee insert then compared into a tree previously
        // ordered for readerParentHash.
        repopulateTxQWithOpenLedgerParent(net[0].app());

        std::vector<uint256> observed;
        std::vector<uint256> expected;
        std::vector<uint256> wrongSaltOrder;
        std::size_t probeInserts = 0;
        auto wrongSaltDiverged = false;
        for (; probeInserts < maxProbeInserts;)
        {
            auto const result = submitSequencedNoop(
                net,
                1,
                readerAccounts[readerPreseed + probeInserts],
                1,
                readerFee);
            if (!BEAST_EXPECTS(
                    result == terQUEUED,
                    "reader probe submit returned " + transToken(result)))
                return;
            ++probeInserts;

            observed = queuedTxIds(net[1].app());
            expected = sortedByParentHash(observed, readerParentHash);
            wrongSaltOrder = sortedByParentHash(observed, writerParentHash);
            wrongSaltDiverged =
                wrongSaltDiverged || !sameOrder(expected, wrongSaltOrder);
            if (!sameOrder(observed, expected))
                break;
        }

        log << "  txq-parenthashcomp oracle writerParentHash="
            << to_string(writerParentHash)
            << " readerParentHash=" << to_string(readerParentHash)
            << " preseed=" << readerPreseed << " probeInserts=" << probeInserts
            << " queueSize=" << observed.size() << " observed=["
            << shortIds(observed) << "] expectedReaderSalt=["
            << shortIds(expected) << "] expectedWriterSalt=["
            << shortIds(wrongSaltOrder) << "]" << std::endl;

        BEAST_EXPECTS(
            wrongSaltDiverged,
            "TxQ parent-hash oracle sample did not distinguish reader and "
            "writer salts");
        BEAST_EXPECTS(
            sameOrder(observed, expected),
            "TxQ byFee_ order changed after another TxQ instance rewrote the "
            "process-global parentHashComp");
    }

    void
    testGHttpClientSslContextOracle()
    {
        testcase("HTTP client SSL context per-Application policy oracle");

        constexpr bool firstNodePolicy = true;
        constexpr bool secondNodePolicy = false;
        if (!BEAST_EXPECTS(
                firstNodePolicy != secondNodePolicy,
                "gHttpClientSslContext oracle requires divergent node "
                "policies"))
            return;

        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/true);
        auto factory = simOverlayFactory();

        net.add(
            std::nullopt,
            factory,
            JobQueue::DispatchHook{},
            /*bindServerListeners=*/false,
            sslVerifyConfigHook(firstNodePolicy));
        if (!BEAST_EXPECT(net.allUp()))
            return;

        auto const node0AfterFirstBoot =
            net[0].app().getHTTPClientSSLContext().sslVerify();
        if (!BEAST_EXPECTS(
                node0AfterFirstBoot == firstNodePolicy,
                "node0 HTTP client SSL context reflects node0 policy after "
                "node0 boot"))
            return;

        net.add(
            std::nullopt,
            factory,
            JobQueue::DispatchHook{},
            /*bindServerListeners=*/false,
            sslVerifyConfigHook(secondNodePolicy));
        if (!BEAST_EXPECT(net.allUp()))
            return;

        auto const node0AfterSecondBoot =
            net[0].app().getHTTPClientSSLContext().sslVerify();
        auto const node1AfterSecondBoot =
            net[1].app().getHTTPClientSSLContext().sslVerify();

        log << "  ghttpclientsslcontext oracle node0Intended="
            << yesno(firstNodePolicy)
            << " node1Intended=" << yesno(secondNodePolicy)
            << " node0AfterNode0=" << yesno(node0AfterFirstBoot)
            << " node0AfterNode1=" << yesno(node0AfterSecondBoot)
            << " node1AfterNode1=" << yesno(node1AfterSecondBoot) << std::endl;

        BEAST_EXPECTS(
            node0AfterSecondBoot == firstNodePolicy,
            "node0 HTTP client SSL context changed after node1 booted with a "
            "different policy");
        BEAST_EXPECTS(
            node1AfterSecondBoot == secondNodePolicy,
            "node1 HTTP client SSL context did not reflect node1 policy");
        BEAST_EXPECTS(
            node0AfterSecondBoot != node1AfterSecondBoot,
            "HTTP client SSL oracle requires distinct per-node policies");
    }

    void
    testThreadedTraffic()
    {
        using namespace std::chrono_literals;
        auto const runs = runsArg(/*fallback=*/3);
        auto const mode = stringArg("mode", "seeded");
        testcase(
            mode == "txqPressure"
                ? "threaded traffic: TxQ pressure validates queued chained "
                  "bursts under real worker interleavings"
                : "threaded traffic: seeded bursts validate under real worker "
                  "interleavings");

        RunConfig config;
        if (mode == "txqPressure")
        {
            config.txqPressure = true;
            config.accounts = 12;
            config.maxScenarioBeats = 240;
            config.trafficBeats = 8;
            config.burstsPerBeat = 3;
            config.burstSize = 6;
            config.maxPerSourcePerBeat = 2;
        }
        else if (!BEAST_EXPECTS(
                     mode == "seeded",
                     "ThreadedTraffic mode must be seeded or txqPressure"))
        {
            return;
        }
        config.validators = sizeArg("nodes", config.validators);
        config.validators = sizeArg("validators", config.validators);
        config.accounts = sizeArg("population", config.accounts);
        config.accounts = sizeArg("accounts", config.accounts);
        config.formationTarget = static_cast<std::uint32_t>(
            sizeArg("formationTarget", config.formationTarget));
        config.maxFormationBeats =
            sizeArg("maxFormationBeats", config.maxFormationBeats);
        config.maxFundingBeats =
            sizeArg("maxFundingBeats", config.maxFundingBeats);
        config.maxScenarioBeats =
            sizeArg("maxScenarioBeats", config.maxScenarioBeats);
        config.trafficBeats = sizeArg("beats", config.trafficBeats);
        config.trafficBeats = sizeArg("trafficBeats", config.trafficBeats);
        config.burstsPerBeat = sizeArg("bursts", config.burstsPerBeat);
        config.burstsPerBeat = sizeArg("burstsPerBeat", config.burstsPerBeat);
        config.burstSize = sizeArg("burstSize", config.burstSize);
        config.maxPerSourcePerBeat =
            sizeArg("chainLength", config.maxPerSourcePerBeat);
        config.maxPerSourcePerBeat =
            sizeArg("maxPerSource", config.maxPerSourcePerBeat);

        if (!BEAST_EXPECTS(
                config.validators >= 2,
                "ThreadedTraffic requires validators >= 2"))
            return;
        if (!BEAST_EXPECTS(
                config.accounts >= 2, "ThreadedTraffic requires accounts >= 2"))
            return;
        if (!BEAST_EXPECTS(
                config.formationTarget > 0,
                "ThreadedTraffic requires formationTarget > 0"))
            return;
        if (!BEAST_EXPECTS(
                config.maxFormationBeats > 0 && config.maxFundingBeats > 0 &&
                    config.maxScenarioBeats > 0,
                "ThreadedTraffic requires positive beat budgets"))
            return;
        if (!BEAST_EXPECTS(
                config.trafficBeats > 0 && config.burstsPerBeat > 0 &&
                    config.burstSize > 0 && config.maxPerSourcePerBeat > 0,
                "ThreadedTraffic requires positive traffic shape"))
            return;
        if (!BEAST_EXPECTS(
                config.trafficBeats <= config.maxScenarioBeats,
                "ThreadedTraffic maxScenarioBeats must cover trafficBeats"))
            return;
        if (!BEAST_EXPECTS(
                config.trafficBeats * config.burstsPerBeat >= config.validators,
                "ThreadedTraffic shape must target every validator"))
            return;
        if (!defaultGeneratorShape(config))
        {
            if (!BEAST_EXPECTS(
                    config.accounts >= config.validators * 2,
                    "Scaled ThreadedTraffic requires at least two accounts "
                    "per validator"))
                return;

            auto const minAccountsPerTarget =
                config.accounts / config.validators;
            auto const maxPaymentsPerTargetBeat =
                maxBurstsPerTargetPerBeat(config) * config.burstSize;
            if (!BEAST_EXPECTS(
                    maxPaymentsPerTargetBeat <=
                        minAccountsPerTarget * config.maxPerSourcePerBeat,
                    "Scaled ThreadedTraffic burst shape exceeds per-target "
                    "account pool"))
                return;
        }

        MultiNode::ThreadedTickOptions options;
        options.quietPolls = 3;
        options.pollInterval = 1ms;
        options.stallTimeout =
            std::chrono::milliseconds{sizeArg("stallMs", 30000)};
        options.totalTimeout =
            std::chrono::milliseconds{sizeArg("totalMs", 30000)};
        auto const runTotalTimeout =
            std::chrono::milliseconds{sizeArg("runTotalMs", 30000)};

        RunOutcome aggregate;
        std::size_t converged = 0;
        for (std::size_t run = 0; run < runs; ++run)
        {
            auto outcome =
                runThreadedTraffic(run, config, options, runTotalTimeout);
            if (!BEAST_EXPECT(outcome.has_value()))
                return;

            ++converged;
            aggregate.formationBeats += outcome->formationBeats;
            aggregate.fundingBeats += outcome->fundingBeats;
            aggregate.scenarioBeats += outcome->scenarioBeats;
            aggregate.maxDrain =
                std::max(aggregate.maxDrain, outcome->maxDrain);
            aggregate.totalDrain += outcome->totalDrain;
            aggregate.maxPolls =
                std::max(aggregate.maxPolls, outcome->maxPolls);
            aggregate.maxQuietStreak =
                std::max(aggregate.maxQuietStreak, outcome->maxQuietStreak);
            aggregate.maxBufferedBytes =
                std::max(aggregate.maxBufferedBytes, outcome->maxBufferedBytes);
            aggregate.maxBusyJobQueues =
                std::max(aggregate.maxBusyJobQueues, outcome->maxBusyJobQueues);
            aggregate.maxSuspended =
                std::max(aggregate.maxSuspended, outcome->maxSuspended);
            aggregate.sawBufferedBytes =
                aggregate.sawBufferedBytes || outcome->sawBufferedBytes;
            aggregate.sawTransportPosts =
                aggregate.sawTransportPosts || outcome->sawTransportPosts;
            aggregate.sawJobWork = aggregate.sawJobWork || outcome->sawJobWork;
            aggregate.trafficSawBufferedBytes =
                aggregate.trafficSawBufferedBytes ||
                outcome->trafficSawBufferedBytes;
            aggregate.trafficSawTransportPosts =
                aggregate.trafficSawTransportPosts ||
                outcome->trafficSawTransportPosts;
            aggregate.trafficSawJobWork =
                aggregate.trafficSawJobWork || outcome->trafficSawJobWork;
            aggregate.readStarted += outcome->readStarted;
            aggregate.writeStarted += outcome->writeStarted;
            aggregate.trafficReadStarted += outcome->trafficReadStarted;
            aggregate.trafficWriteStarted += outcome->trafficWriteStarted;
            aggregate.payments += outcome->payments;
            aggregate.finalSeq =
                std::max(aggregate.finalSeq, outcome->finalSeq);
            aggregate.queuedSubmits += outcome->queuedSubmits;
            aggregate.queueSamples += outcome->queueSamples;
            aggregate.queueHitSamples += outcome->queueHitSamples;
            aggregate.maxQueueDepth =
                std::max(aggregate.maxQueueDepth, outcome->maxQueueDepth);
            aggregate.maxOpenLedgerTx =
                std::max(aggregate.maxOpenLedgerTx, outcome->maxOpenLedgerTx);

            if (runs > 1)
            {
                log << "  threaded-traffic progress run=" << (run + 1) << "/"
                    << runs << " converged=1"
                    << " mode="
                    << (config.txqPressure ? "txqPressure" : "seeded")
                    << " payments=" << outcome->payments
                    << " finalSeq=" << outcome->finalSeq
                    << " drainMs=" << outcome->totalDrain.count()
                    << " maxDrainMs=" << outcome->maxDrain.count()
                    << " scenarioBeats=" << outcome->scenarioBeats;
                if (config.txqPressure)
                {
                    log << " queuedSubmits=" << outcome->queuedSubmits
                        << " maxQueueDepth=" << outcome->maxQueueDepth
                        << " queueHitSamples=" << outcome->queueHitSamples
                        << " queueSamples=" << outcome->queueSamples;
                }
                log << std::endl;
            }
        }

        log << "  threaded-traffic R=" << runs << " converged=" << converged
            << " mode=" << (config.txqPressure ? "txqPressure" : "seeded")
            << " validators=" << config.validators
            << " accounts=" << config.accounts
            << " trafficBeats=" << config.trafficBeats
            << " burstsPerBeat=" << config.burstsPerBeat
            << " burstSize=" << config.burstSize
            << " maxPerSource=" << config.maxPerSourcePerBeat
            << " payments=" << aggregate.payments
            << " finalSeqMax=" << aggregate.finalSeq
            << " queuedSubmits=" << aggregate.queuedSubmits
            << " queueHitSamples=" << aggregate.queueHitSamples
            << " queueSamples=" << aggregate.queueSamples
            << " maxQueueDepth=" << aggregate.maxQueueDepth
            << " maxOpenLedgerTx=" << aggregate.maxOpenLedgerTx
            << " formationBeats=" << aggregate.formationBeats
            << " fundingBeats=" << aggregate.fundingBeats
            << " scenarioBeats=" << aggregate.scenarioBeats
            << " maxDrainMs=" << aggregate.maxDrain.count()
            << " totalDrainMs=" << aggregate.totalDrain.count()
            << " maxPolls=" << aggregate.maxPolls
            << " maxQuietStreak=" << aggregate.maxQuietStreak
            << " maxPipeBytes=" << aggregate.maxBufferedBytes
            << " maxBusyJobQueues=" << aggregate.maxBusyJobQueues
            << " maxSuspended=" << aggregate.maxSuspended
            << " readPosts=" << aggregate.readStarted
            << " writePosts=" << aggregate.writeStarted
            << " trafficReadPosts=" << aggregate.trafficReadStarted
            << " trafficWritePosts=" << aggregate.trafficWriteStarted
            << " stallMs=" << options.stallTimeout.count()
            << " totalMs=" << options.totalTimeout.count()
            << " runTotalMs=" << runTotalTimeout.count() << std::endl;

        BEAST_EXPECT(converged == runs);
        BEAST_EXPECT(
            aggregate.payments ==
            runs * config.trafficBeats * config.burstsPerBeat *
                config.burstSize);
        BEAST_EXPECT(aggregate.trafficSawBufferedBytes);
        BEAST_EXPECT(aggregate.trafficSawTransportPosts);
        BEAST_EXPECT(aggregate.trafficSawJobWork);
        BEAST_EXPECT(aggregate.trafficReadStarted > 0);
        BEAST_EXPECT(aggregate.trafficWriteStarted > 0);
        if (config.txqPressure)
        {
            BEAST_EXPECT(aggregate.queuedSubmits > 0);
            BEAST_EXPECT(aggregate.maxQueueDepth > 0);
            BEAST_EXPECT(aggregate.queueHitSamples > 0);
        }
        BEAST_EXPECT(aggregate.maxDrain < options.totalTimeout);
    }

public:
    void
    run() override
    {
        auto const mode = stringArg("mode", "seeded");
        if (mode == "txqParentHashComp")
            testTxQParentHashCompRung0();
        else if (mode == "txqParentHashCompOracle")
            testTxQParentHashCompSteppingOracle();
        else if (mode == "gHttpClientSslContextOracle")
            testGHttpClientSslContextOracle();
        else
            testThreadedTraffic();
    }
};

BEAST_DEFINE_TESTSUITE(ThreadedTraffic, consensus, ripple);

}  // namespace ripple::test
