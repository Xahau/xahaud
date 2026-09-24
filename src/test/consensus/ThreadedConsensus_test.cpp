//------------------------------------------------------------------------------
// ThreadedConsensus -- K=0 threaded virtual-time consensus driver.
//
// This keeps production io/job threads live, suppresses wall-clock heartbeats,
// and replaces the old fixed 40ms settle with a quiescence drain over harness
// seams: SimPipe bytes, SimTransport completion posts, JobQueue idle, and a
// monotonic transport activity epoch.
//------------------------------------------------------------------------------
#include <test/jtx/MultiNode.h>

#include <xrpld/overlay/Overlay.h>

#include <xrpl/beast/unit_test/suite.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace ripple::test {

class ThreadedConsensus_test : public beast::unit_test::suite
{
    struct RunOutcome
    {
        std::size_t beats = 0;
        std::chrono::milliseconds maxDrain{0};
        std::chrono::milliseconds totalDrain{0};
        std::vector<std::chrono::milliseconds> drains;
        std::size_t maxPolls = 0;
        std::size_t maxQuietStreak = 0;
        std::size_t maxBufferedBytes = 0;
        std::size_t maxBusyJobQueues = 0;
        int maxSuspended = 0;
        bool sawBufferedBytes = false;
        bool sawTransportPosts = false;
        bool sawJobWork = false;
        std::uint64_t readStarted = 0;
        std::uint64_t writeStarted = 0;
        std::uint64_t shutdownStarted = 0;
    };

    [[nodiscard]] static OverlayFactory
    simOverlayFactory()
    {
        return [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
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

    [[nodiscard]] static std::chrono::milliseconds
    median(std::vector<std::chrono::milliseconds> values)
    {
        if (values.empty())
            return std::chrono::milliseconds{0};
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    void
    logThreadedState(MultiNode& net, char const* label)
    {
        auto const activity = net.simActivitySnapshot();
        log << "  " << label << ": pipeBytes=" << net.simBufferedBytes()
            << " posts=" << activity.inFlightPosts
            << " epoch=" << activity.epoch << " read=" << activity.readStarted
            << "/" << activity.readFinished
            << " write=" << activity.writeStarted << "/"
            << activity.writeFinished
            << " shutdown=" << activity.shutdownStarted << "/"
            << activity.shutdownFinished << " seqs=[";
        char const* sep = "";
        for (std::uint32_t i = 0; i < net.size(); ++i)
        {
            if (!net.isLive(i))
                continue;
            auto& jq = net[i].app().getJobQueue();
            log << sep << "n" << i << "{closed=" << net.closedSeq(i)
                << ",valid=" << net.validSeq(i)
                << ",idle=" << (jq.isIdle() ? "true" : "false")
                << ",suspended=" << jq.suspendedCount() << "}";
            sep = ",";
        }
        log << "]" << std::endl;
    }

    [[nodiscard]] std::optional<RunOutcome>
    runThreadedNetwork(
        std::size_t run,
        std::uint32_t target,
        std::size_t maxBeats,
        MultiNode::ThreadedTickOptions const& options,
        std::chrono::milliseconds runTotalTimeout)
    {
        using namespace std::chrono_literals;

        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/false);

        std::vector<ValidatorKey> keys;
        std::vector<std::string> unl;
        keys.reserve(3);
        unl.reserve(3);
        for (std::size_t i = 0; i < 3; ++i)
        {
            keys.push_back(ValidatorKey::fromPassphrase(
                "threaded-k0-" + std::to_string(i)));
            unl.push_back(keys.back().pubKey);
        }

        auto factory = simOverlayFactory();
        for (auto const& key : keys)
            net.add(
                TrustConfig{key.seed, unl},
                factory,
                JobQueue::DispatchHook{},
                /*bindServerListeners=*/false);
        if (!BEAST_EXPECT(net.allUp()))
            return std::nullopt;

        for (std::size_t i = 0; i < 3; ++i)
            for (std::size_t j = i + 1; j < 3; ++j)
                if (!BEAST_EXPECT(net.simConnect(i, j) != nullptr))
                    return std::nullopt;
        if (!BEAST_EXPECT(net.waitForPeers(/*expected=*/2, 20s)))
            return std::nullopt;

        auto const startActivity = net.simActivitySnapshot();
        RunOutcome out;
        try
        {
            while (out.beats < maxBeats && net.minValidated() < target)
            {
                auto const tick = net.threadedTick(1s, options);
                ++out.beats;
                out.maxDrain = std::max(out.maxDrain, tick.wallElapsed);
                out.totalDrain += tick.wallElapsed;
                out.drains.push_back(tick.wallElapsed);
                out.maxPolls = std::max(out.maxPolls, tick.polls);
                out.maxQuietStreak =
                    std::max(out.maxQuietStreak, tick.maxQuietStreak);
                out.maxBufferedBytes =
                    std::max(out.maxBufferedBytes, tick.maxBufferedBytes);
                out.maxBusyJobQueues =
                    std::max(out.maxBusyJobQueues, tick.maxBusyJobQueues);
                out.maxSuspended =
                    std::max(out.maxSuspended, tick.maxSuspended);
                out.sawBufferedBytes =
                    out.sawBufferedBytes || tick.sawBufferedBytes;
                out.sawTransportPosts =
                    out.sawTransportPosts || tick.sawTransportPosts;
                out.sawJobWork = out.sawJobWork || tick.sawJobWork;
                if (!BEAST_EXPECT(net.validatedForkFree()))
                {
                    logThreadedState(net, "fork failure");
                    return std::nullopt;
                }
                if (!BEAST_EXPECTS(
                        out.totalDrain <= runTotalTimeout,
                        "threaded-k0 cumulative drain budget exceeded"))
                {
                    logThreadedState(net, "drain budget failure");
                    return std::nullopt;
                }
            }
        }
        catch (std::exception const& e)
        {
            log << "  run " << run << " threadedTick exception: " << e.what()
                << std::endl;
            logThreadedState(net, "exception");
            BEAST_EXPECT(false);
            return std::nullopt;
        }

        if (!BEAST_EXPECT(net.minValidated() >= target))
        {
            log << "  run " << run << " stalled after " << out.beats
                << " beats (cap " << maxBeats << ")" << std::endl;
            logThreadedState(net, "stalled");
            return std::nullopt;
        }
        if (!BEAST_EXPECT(net.ledgersAgree(target)))
        {
            logThreadedState(net, "ledger disagreement");
            return std::nullopt;
        }
        if (!BEAST_EXPECT(net.validatedForkFree()))
        {
            logThreadedState(net, "final fork failure");
            return std::nullopt;
        }

        BEAST_EXPECT(net.waitForSimQuiescence(options.stallTimeout));
        auto const endActivity = net.simActivitySnapshot();
        out.readStarted = endActivity.readStarted - startActivity.readStarted;
        out.writeStarted =
            endActivity.writeStarted - startActivity.writeStarted;
        out.shutdownStarted =
            endActivity.shutdownStarted - startActivity.shutdownStarted;
        out.maxBufferedBytes = std::max<std::size_t>(
            out.maxBufferedBytes,
            static_cast<std::size_t>(endActivity.maxBufferedBytes));

        BEAST_EXPECT(net.simBufferedBytes() == 0);
        BEAST_EXPECT(endActivity.inFlightPosts == 0);
        return out;
    }

    void
    testTrackerSites()
    {
        testcase(
            "threaded tracker covers pipe bytes and read/write/shutdown posts");

        boost::asio::io_context ioc;
        auto exec = Transport::executor_type(ioc.get_executor());
        auto activity = std::make_shared<SimTransportActivity>();
        SimWire wire;
        auto [a, b] = wire.endpoints(exec, exec, uint256{}, {}, {}, activity);

        std::array<std::uint8_t, 64> rbuf{};
        bool readDone = false;
        bool writeDone = false;
        bool shutdownDone = false;
        std::string const payload = "threaded-k0";

        b->async_read_some(
            {boost::asio::buffer(rbuf)},
            exec,
            [&](Transport::error_code ec, std::size_t n) {
                BEAST_EXPECT(!ec);
                BEAST_EXPECT(n == payload.size());
                readDone = true;
            });
        a->async_write(
            {boost::asio::buffer(payload)},
            exec,
            [&](Transport::error_code ec, std::size_t n) {
                BEAST_EXPECT(!ec);
                BEAST_EXPECT(n == payload.size());
                writeDone = true;
            });
        auto const posted = activity->snapshot();
        BEAST_EXPECT(posted.inFlightPosts >= 2);
        BEAST_EXPECT(posted.bufferedEvents > 0);
        BEAST_EXPECT(posted.maxBufferedBytes >= payload.size());

        ioc.run();
        auto afterIo = activity->snapshot();
        BEAST_EXPECT(readDone && writeDone);
        BEAST_EXPECT(afterIo.inFlightPosts == 0);
        BEAST_EXPECT(afterIo.readStarted == 1 && afterIo.readFinished == 1);
        BEAST_EXPECT(afterIo.writeStarted == 1 && afterIo.writeFinished == 1);

        ioc.restart();
        a->async_shutdown(exec, [&](Transport::error_code ec) {
            BEAST_EXPECT(!ec);
            shutdownDone = true;
        });
        ioc.run();
        afterIo = activity->snapshot();
        BEAST_EXPECT(shutdownDone);
        BEAST_EXPECT(
            afterIo.shutdownStarted == 1 && afterIo.shutdownFinished == 1);
        BEAST_EXPECT(afterIo.inFlightPosts == 0);
    }

    void
    testThreadedK0Converges()
    {
        testcase(
            "threaded-k0: N=3 validators converge under quiescence drain "
            "(use --unittest-arg=runs=N for local grind)");

        using namespace std::chrono_literals;
        auto const runs = runsArg(/*fallback=*/3);
        constexpr std::uint32_t kTarget = 3;
        constexpr std::size_t kMaxBeats = 120;
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
        std::vector<std::chrono::milliseconds> allDrains;
        for (std::size_t run = 0; run < runs; ++run)
        {
            auto outcome = runThreadedNetwork(
                run, kTarget, kMaxBeats, options, runTotalTimeout);
            if (!BEAST_EXPECT(outcome.has_value()))
                return;
            ++converged;
            aggregate.beats += outcome->beats;
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
            aggregate.readStarted += outcome->readStarted;
            aggregate.writeStarted += outcome->writeStarted;
            aggregate.shutdownStarted += outcome->shutdownStarted;
            allDrains.insert(
                allDrains.end(),
                outcome->drains.begin(),
                outcome->drains.end());
        }

        auto const med = median(allDrains);
        log << "  threaded-k0 R=" << runs << " converged=" << converged
            << " target=" << kTarget << " totalBeats=" << aggregate.beats
            << " maxDrainMs=" << aggregate.maxDrain.count()
            << " totalDrainMs=" << aggregate.totalDrain.count()
            << " medianDrainMs=" << med.count()
            << " maxPolls=" << aggregate.maxPolls
            << " maxQuietStreak=" << aggregate.maxQuietStreak
            << " maxPipeBytes=" << aggregate.maxBufferedBytes
            << " maxBusyJobQueues=" << aggregate.maxBusyJobQueues
            << " maxSuspended=" << aggregate.maxSuspended
            << " readPosts=" << aggregate.readStarted
            << " writePosts=" << aggregate.writeStarted
            << " shutdownPosts=" << aggregate.shutdownStarted
            << " stallMs=" << options.stallTimeout.count()
            << " totalMs=" << options.totalTimeout.count()
            << " runTotalMs=" << runTotalTimeout.count() << std::endl;

        BEAST_EXPECT(converged == runs);
        BEAST_EXPECT(aggregate.sawBufferedBytes);
        BEAST_EXPECT(aggregate.sawTransportPosts);
        BEAST_EXPECT(aggregate.sawJobWork);
        BEAST_EXPECT(aggregate.readStarted > 0);
        BEAST_EXPECT(aggregate.writeStarted > 0);
        BEAST_EXPECT(aggregate.maxBufferedBytes > 0);
        BEAST_EXPECT(aggregate.maxDrain < options.totalTimeout);
    }

public:
    void
    run() override
    {
        testTrackerSites();
        testThreadedK0Converges();
    }
};

BEAST_DEFINE_TESTSUITE(ThreadedConsensus, consensus, ripple);

}  // namespace ripple::test
