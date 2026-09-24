#pragma once
//------------------------------------------------------------------------------
// MultiNode — an in-process harness that stands up N real ApplicationImp
// instances, peers them over the REAL loopback overlay (PeerImp handshake), and
// drives them through REAL consensus. The middle ground between jtx::Env (one
// standalone app) and csf::Sim (N toy ledgers): N real applications, real
// overlay, real consensus, one process.
//
// Extracted from src/test/consensus/HarnessNet_test.cpp once rungs A–D were
// green. See .ai-docs/specs/csf-peerimp-hybrid-overlay-harness.md (Stage 0):
//   - peeredEnvconfig (S0.7): non-standalone + no process signal handlers /
//   stall
//     detector, so N apps share one process;
//   - one shared debug log sink set ONCE for the harness (only one owner
//   allowed,
//     Log.cpp / Env.cpp:82-121) — owned by MultiNode, never per node;
//   - per-node TempDir database_path (Config.cpp:1230 requires it
//   non-standalone);
//   - static [validators] UNL + per-node [validation_seed] for trust/quorum;
//   - default Config::NORMAL → no needNetworkLedger, so a genesis network
//     bootstraps consensus directly.
//------------------------------------------------------------------------------
#include <test/jtx/Account.h>
#include <test/jtx/Env.h>  // SuiteLogs
#include <test/jtx/ManualTimeKeeper.h>
#include <test/jtx/SimOverlay.h>
#include <test/jtx/SteppingController.h>
#include <test/jtx/envconfig.h>
#include <test/jtx/utility.h>  // parse/sign/fillFee/fillSeq (Env-free)
#include <test/unit_test/SuiteJournal.h>

#include <xrpld/app/consensus/RCLValidations.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/ledger/detail/TimeoutCounter.h>  // TimeoutCounterTimer seam
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/overlay/Peer.h>

#include <xrpld/app/misc/NetworkOPs.h>  // getOPs().heartbeatTick() (virtual driver)
#include <xrpld/core/JobQueue.h>
#include <xrpl/basics/FileUtilities.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/clock/abstract_clock.h>
#include <xrpl/beast/net/IPEndpoint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/beast/utility/temp_dir.h>
#include <xrpl/beast/xor_shift_engine.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/KeyType.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STTx.h>  // sterilize
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/jss.h>

#include <boost/asio/ip/address.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ripple::test {

using TempDir = beast::temp_dir;

// A validator identity: [validation_seed] (base58 s...) plus the matching
// trusted node public key (base58 n...) that other nodes list in [validators].
// The secp256k1 derivation mirrors ValidatorKeys.cpp so the advertised key
// equals what this node actually signs validations with.
struct ValidatorKey
{
    std::string seed;    // base58 s...
    std::string pubKey;  // base58 n...

    static ValidatorKey
    fromPassphrase(std::string const& passphrase)
    {
        auto const s = generateSeed(passphrase);
        auto const sk = generateSecretKey(KeyType::secp256k1, s);
        auto const pk = derivePublicKey(KeyType::secp256k1, sk);
        return {toBase58(s), toBase58(TokenType::NodePublic, pk)};
    }
};

// Per-node trust: this node's own validator seed + the static UNL it enforces.
// An EMPTY validationSeed makes the node a non-validator OBSERVER: it trusts
// (and requires quorum from) the listed validators but never signs validations
// itself — the shape of a client-facing tracking node.
struct TrustConfig
{
    std::string validationSeed;           // base58 s... (empty = observer)
    std::vector<std::string> validators;  // base58 n... keys (the UNL)
};

// Bounded real-time poll (Stage 0 runs in wall-clock; virtual time is Stage 2).
// Returns pred()'s final value — never hangs.
template <class Pred>
bool
waitUntil(
    Pred pred,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds step = std::chrono::milliseconds{25})
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

// A thread-safe MANUAL STEADY clock for Stage 2 virtual time. It is the steady
// clock injected into consensus + validations (NetworkOPs::clock_type ==
// beast::abstract_clock<std::chrono::steady_clock>). The harness thread is the
// single writer (advance() in MultiNode::tick); consensus/validation jobs read
// now() off the job-queue / io threads, so storage is atomic — mirroring
// ManualTimeKeeper's atomic<time_point>. (beast::ManualClock is NOT
// thread-safe; it would race those readers and trip ThreadSanitizer.)
class ManualSteadyClock
    : public beast::abstract_clock<std::chrono::steady_clock>
{
    std::atomic<time_point> now_{time_point{duration{0}}};

public:
    [[nodiscard]] time_point
    now() const override
    {
        return now_.load(std::memory_order_acquire);
    }

    // Single-writer; readers only load(). Steady clocks must move forward.
    void
    advance(duration d)
    {
        now_.store(
            now_.load(std::memory_order_relaxed) + d,
            std::memory_order_release);
    }

    // Set the clock to an ABSOLUTE virtual time (Stage 3 stepping: the harness
    // scheduler's now() is the master clock, so every event syncs this clock up
    // to it). Single-writer (stepping thread). Never moves backward — scheduler
    // time is monotonic, so a past `t` (e.g. equal-instant events) is a no-op.
    void
    advanceTo(time_point t)
    {
        auto const cur = now_.load(std::memory_order_relaxed);
        if (t > cur)
            now_.store(t, std::memory_order_release);
    }
};

// The four passive observation hooks a node can carry (Config::harnessX).
// Installed on the Config BEFORE Application construction so no reader thread
// can race the assignment (same install-before-flow contract as the JobQueue
// dispatch hook).
struct NodeHooks
{
    Config::HarnessPeerMessageHook peerMessage;
    Config::HarnessPeerSendHook peerSend;
    Config::HarnessPeerLifecycleHook peerLifecycle;
    Config::HarnessValidationHook validation;
};

using ConfigHook = std::function<void(Config&)>;

/** Ledger startup policy; either choice retains the wallet/identity directory.
 */
enum class LedgerStart { Fresh, LoadLatest };

// Everything needed to bring one node up. Aggregate-initialized at the two
// construction sites (MultiNode::add / restartNode); every field has a
// production-faithful default so a NodeSpec{dbPath} is a plain wall-clock node.
struct NodeSpec
{
    std::string dbPath;
    std::optional<TrustConfig> trust;
    OverlayFactory overlayFactory;
    // Stage 2: when non-null, this node runs on VIRTUAL time — the asio
    // heartbeat is suppressed (manualHeartbeat) and consensus + validations
    // read this injected manual steady clock instead of the wall clock.
    beast::abstract_clock<std::chrono::steady_clock>* injectedSteadyClock =
        nullptr;
    // Stage 3: an optional JobQueue dispatch hook, installed BEFORE setup()
    // (i.e. before any job flow), so the harness can observe (discovery) or
    // claim (stepping) jobs. Default-empty → no install, unchanged behavior.
    JobQueue::DispatchHook jobHook;
    // The four passive observation hooks (Config::harnessX), installed on the
    // Config BEFORE the Application exists — so no io/run thread can ever
    // observe a torn std::function assignment (same install-before-flow
    // contract as jobHook). Default-empty → no hooks, unchanged behavior.
    NodeHooks hooks;
    // Optional per-node config mutation. Installed before Application
    // construction so scenario-only knobs, such as a small TxQ, do not race any
    // runtime readers and survive restartNode through the stable slot.
    ConfigHook configHook;
    // When false, Application::setup parses server config but does not bind
    // real RPC/peer listeners. SimOverlay/SimTransport tests use this even
    // outside strict stepping because peer traffic is supplied in-process.
    bool bindServerListeners = true;
    // Stage 3 STEPPING mode: 0 io threads + 0 JobQueue workers
    // (Config::steppingMode) and inline PeerImp strands
    // (Config::inlineStrands), so ALL app-visible work runs only when the
    // harness steps its scheduler on the test thread. No run() thread is
    // started; teardown poll-pumps the io for the orderly run() shutdown (the
    // S3.6 spike pattern).
    bool stepping = false;
    LedgerStart ledgerStart = LedgerStart::Fresh;
    // Stepping mode: the node's deterministic PRNG (Application::getPrng),
    // pinning protocol-adjacent random choices (acquire peer sampling, relay
    // selection). nullptr -> production per-thread default_prng(). Injected
    // ONLY in stepping mode: an injected engine has no per-thread isolation,
    // and only stepping is single-threaded by construction.
    beast::xor_shift_engine* injectedPrng = nullptr;
    // Stepping mode: the acquire-retry timer factory (issue 005, the
    // TimeoutCounter seam). Empty -> production asio retry timers,
    // unchanged. Installed at make_Application (install-before-flow) so
    // every acquire machine a node ever constructs arms virtual-time
    // retries instead of wall ones.
    TimeoutCounterTimerFactory timeoutCounterTimerFactory;
    // Stepping mode: the PeerImp heartbeat timer factory (005 slice 4 —
    // the LAST gated timer, virtualized). Empty -> production nullptr:
    // PeerImp keeps its raw asio member untouched.
    TimeoutCounterTimerFactory peerTimerFactory;
    // Startup NetClock override, applied before the app runs. Threaded virtual
    // nodes use the runner's current time, including time spent stopped.
    std::optional<NetClock::time_point> restoredNetClock;
};

// The stepping implementation of the acquire-retry timer seam (issue 005 /
// codex round-6): TimeoutCounter::setTimer() becomes a Tier::timer scheduler
// event at virtual now + interval, so acquire retries live INSIDE the
// deterministic timeline instead of escaping to wall time. The handler the
// TimeoutCounter passes wraps queueJob() — not onTimer() — so the JobQueue
// path, job limits, labels, and invokeOnTimer semantics stay real (the
// dispatch hook then classifies the posted job at Tier::timer as before).
//
// Cancellation drops the wrapper's reference to the current arm's HANDLER
// HOLDER; the scheduled event holds only a WEAK reference to it and runs the
// handler only if it can still lock it. So re-arm/cancel/destroy makes stale
// arms no-op (only the latest fires) AND releases whatever the handler
// captured PROMPTLY, without ever touching the scheduler (no dangling
// CancelToken through a teardown-cleared queue). The prompt release matters:
// PeerImp's heartbeat handler captures a STRONG shared_from_this() (matching
// its production asio timer), so a stale arm left pinning that ref for a full
// virtual interval would keep a SEVERED peer — and its peerfinder slot —
// alive, breaking a same-identity reconnect with DuplicatePeer. Production's
// timer.cancel() releases the handler at once; dropping the holder is the
// virtual equivalent.
//
// The weak/lock split also makes re-entrancy safe: PeerImp's onTimer re-arms
// (setTimer) and can cancel (fail→cancelTimer) from INSIDE the firing
// handler. The event lock()s the holder for the duration of the call, so a
// re-entrant reset only drops the wrapper's reference while the locked temp
// keeps the executing std::function alive until it returns.
class SteppingTimeoutCounterTimer : public TimeoutCounterTimer
{
    using Holder = std::shared_ptr<std::function<void()>>;

public:
    SteppingTimeoutCounterTimer(
        SteppingController& controller,
        std::uint32_t nodeId,
        std::string label = "TimeoutCounter retry")
        : controller_(controller), nodeId_(nodeId), label_(std::move(label))
    {
    }

    ~SteppingTimeoutCounterTimer() override = default;  // armed_ drop releases

    void
    expiresAfter(
        std::chrono::milliseconds interval,
        std::function<void()> handler) override
    {
        // Teardown tolerance (005 slice 4): residual peer cleanup runs on
        // the io pump thread while the harness drains — a PeerImp
        // gracefulClose can re-arm there. The node is going away and the
        // scheduler may already be cleared: drop the arm, mirroring how
        // the delivery routers drop residual completions while draining.
        if (controller_.draining())
            return;
        armed_ = std::make_shared<std::function<void()>>(std::move(handler));
        controller_.scheduleTimer(
            nodeId_,
            std::chrono::duration_cast<SteppingController::duration>(interval),
            [weak = std::weak_ptr<std::function<void()>>(armed_)]() {
                if (auto const h = weak.lock(); h && *h)
                    (*h)();
            },
            label_);
    }

    void
    cancel() override
    {
        armed_.reset();
    }

private:
    SteppingController& controller_;
    std::uint32_t nodeId_;
    std::string label_;
    Holder armed_;
};

// One in-process node: a real Application, virtual clock, and run() thread.
// Mirrors Env::AppBundle (Env.cpp:72-122) but standalone-free. Heap-held by
// MultiNode (thread makes it non-movable). Does NOT touch the process-global
// debug sink — MultiNode owns that. The database dir belongs to MultiNode's
// stable node slot, so stop/restart can preserve disk state while replacing
// this live app bundle.
class NodeBundle
{
    std::unique_ptr<Application> app_;
    ManualTimeKeeper* tk_ = nullptr;
    std::thread runThread_;
    bool stepping_ = false;

public:
    NodeBundle(beast::unit_test::suite& suite, NodeSpec spec)
        : stepping_(spec.stepping)
    {
        using namespace jtx;

        auto logs = std::make_unique<SuiteLogs>(suite);

        // peeredEnvconfig (S0.7): standalone->false, no process signal handlers
        // / stall detector (S0.2/S0.3).
        auto cfg = peeredEnvconfig(envconfig());
        // Non-standalone REQUIRES a real database_path (Config.cpp:1230).
        cfg->legacy("database_path", spec.dbPath);
        // xahaud envconfig defaults [relational_db] to rwdb (per-Application
        // in-memory). Donor envconfig leaves sqlite, so restartNode can LOAD
        // latest from files under database_path. Keep that persistence here;
        // otherwise setup() fails with "specified ledger could not be loaded".
        cfg->overwrite(SECTION_RELATIONAL_DB, "backend", "sqlite");
        // The memory nodestore backend is a PROCESS-GLOBAL static keyed by
        // the [node_db] path string, and envconfig's fixed "main" would make
        // every node in every run share one content-addressed table that
        // SURVIVES close() — cross-node and cross-run leakage: a joiner can
        // "acquire" ledgers straight out of a prior run's store (found by
        // SteppingLargeNet's trace fingerprint as a first-run-in-process
        // divergence). Key it by this node's unique db dir instead; the slot
        // path is stable across stop/restart, which preserves the restart
        // catch-up semantics that (accidentally) relied on the static's
        // persistence.
        // xahaud's envconfig selects rwdb, which clears its per-instance
        // table on close. Use the donor's path-keyed memory backend so a
        // restarted node can load the state tree behind its saved SQL row.
        cfg->overwrite(ConfigSection::nodeDatabase(), "type", "memory");
        cfg->overwrite(ConfigSection::nodeDatabase(), "path", spec.dbPath);
        // A from-genesis network's true earliest ledger is 1. The default
        // (XRP_LEDGER_EARLIEST_SEQ = 32570, mainnet's first available ledger)
        // floors LedgerMaster's prevMissing() ABOVE every sequence a genesis
        // world produces, silently disabling the whole history-backfill
        // subsystem (doAdvance -> fetchForHistory -> TryFill / fetch packs)
        // in every scenario — found when SteppingCombined's late joiner
        // ended with complete=[11-15] and zero backfill. Real from-genesis
        // networks (altnets, sidechains) set this; so does the harness.
        cfg->overwrite(ConfigSection::nodeDatabase(), "earliest_seq", "1");
        switch (spec.ledgerStart)
        {
            case LedgerStart::Fresh:
                break;  // Keep the normal fresh-ledger startup from envconfig.
            case LedgerStart::LoadLatest:
                cfg->START_UP = Config::LOAD;
                cfg->START_LEDGER = "latest";
                break;
        }

        // Virtual-clock mode: suppress the asio heartbeat so it is driven only
        // by MultiNode::tick() -> getOPs().heartbeatTick() (no wall-clock
        // waits).
        if (spec.injectedSteadyClock)
            cfg->manualHeartbeat = true;

        if (!spec.bindServerListeners || spec.stepping)
        {
            cfg->bindServerListeners = false;
            static std::atomic<std::uint16_t> nextSyntheticPeerPort{30000};
            cfg->section(PORT_PEER).set(
                "port", std::to_string(nextSyntheticPeerPort++));
        }

        // Stepping mode: 0 io threads + 0 JobQueue workers (so nothing
        // app-visible runs except a stepped scheduler event) and inline PeerImp
        // strands (so a send/deliver runs inline on the stepping thread rather
        // than via the io pool). Both gated; harmless without the dispatch hook
        // + scheduler driver.
        if (spec.stepping)
        {
            cfg->steppingMode = true;
            cfg->inlineStrands = true;
        }

        // Passive observation hooks: set on the Config BEFORE the Application
        // exists, so no io/run thread can ever observe a torn std::function
        // assignment.
        cfg->harnessPeerMessage = std::move(spec.hooks.peerMessage);
        cfg->harnessPeerSend = std::move(spec.hooks.peerSend);
        cfg->harnessPeerLifecycle = std::move(spec.hooks.peerLifecycle);
        cfg->harnessValidation = std::move(spec.hooks.validation);

        if (spec.configHook)
            spec.configHook(*cfg);

        // Optional validator identity + static UNL (no [validator_list_sites]
        // needed — Application.cpp:1338/1349). Default Config::NORMAL means
        // needNetworkLedger is not set, so a genesis network can converge. An
        // empty validationSeed = OBSERVER: UNL only, no signing identity.
        if (spec.trust)
        {
            // A config-time token supplies a rotated signing identity. Keep
            // the static master-key UNL, without also supplying a seed (the
            // production startup correctly rejects that combination).
            if (!spec.trust->validationSeed.empty() &&
                !cfg->exists(SECTION_VALIDATOR_TOKEN))
                cfg->section(SECTION_VALIDATION_SEED)
                    .append(
                        std::vector<std::string>{spec.trust->validationSeed});
            cfg->section(SECTION_VALIDATORS).append(spec.trust->validators);
        }

        auto tk = std::make_unique<ManualTimeKeeper>();
        tk_ = tk.get();

        // Inject a custom Overlay (e.g. SimOverlay) via the S0.1 factory hook;
        // default = the real loopback makeOverlay(). injectedSteadyClock is
        // nullptr outside virtual mode -> production wall-clock steady clocks.
        app_ = make_Application(
            std::move(cfg),
            std::move(logs),
            std::move(tk),
            std::move(spec.overlayFactory),
            spec.injectedSteadyClock,
            spec.injectedPrng,
            std::move(spec.timeoutCounterTimerFactory),
            std::move(spec.peerTimerFactory));
        // Install the dispatch hook before any job flow (setup posts jobs).
        if (spec.jobHook)
            app_->getJobQueue().setDispatchHook(std::move(spec.jobHook));
        if (!app_->setup({}))
        {
            app_.reset();  // setup failed → isUp() == false
            return;
        }

        tk_->set(app_->getLedgerMaster().getClosedLedger()->info().closeTime);
        if (spec.restoredNetClock)
            tk_->set(*spec.restoredNetClock);
        // Don't start timers explicitly; the consensus heartbeat is armed by
        // setStateTimer in setup() (Application.cpp:1422) for non-standalone.
        app_->start(false);
        // Stepping mode runs with NO run() thread — the test thread is the only
        // driver (scheduler steps). run() is invoked inline at teardown for the
        // orderly shutdown, pumped by a helper (see ~NodeBundle). Other modes
        // run app->run() on a background thread as usual.
        if (!stepping_)
            runThread_ = std::thread([app = app_.get()]() { app->run(); });
    }

    ~NodeBundle()
    {
        if (!app_)
            return;

        if (stepping_)
        {
            // 0-io-thread teardown (S3.6 spike): the orderly stop in run()
            // dispatches resolver/waitHandler work onto the io_context, so a
            // helper thread must pump it via poll() while run() performs
            // shutdown. The harness MUST have already dropPending() (the
            // scheduler's claimed-job closures hold JobCounter tokens;
            // JobQueue::stop() joins on them).
            auto& io = app_->getIOService();
            std::atomic<bool> stopped{false};
            std::thread pump([&io, &stopped]() {
                while (!stopped.load(std::memory_order_relaxed))
                {
                    io.poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            });
            app_->getJobQueue().rendezvous();
            app_->signalStop("MultiNode");
            app_->run();  // orderly shutdown, pumped by `pump`
            stopped.store(true, std::memory_order_relaxed);
            pump.join();
            return;
        }

        // Mirrors ~AppBundle: drain jobs, signal stop, join run().
        app_->getJobQueue().rendezvous();
        app_->signalStop("MultiNode");
        if (runThread_.joinable())
            runThread_.join();

        // Overlay::stop returns once the child list is empty. That list can
        // already be empty, so stopChildren may still be queued on the io
        // service. One handler per io thread, posted after run() returns, runs
        // only after that queued work. Then stop the service so those threads
        // leave run() before the overlay object is destroyed.
        auto const ioThreads = [](Config const& config) -> std::size_t {
            if (config.steppingMode)
                return 0;
#if RIPPLE_SINGLE_IO_SERVICE_THREAD
            return 1;
#else
            if (config.IO_WORKERS > 0)
                return static_cast<std::size_t>(config.IO_WORKERS);
            auto const cores = std::thread::hardware_concurrency();
            if (cores == 1 || (config.NODE_SIZE == 0 && cores == 2))
                return 1;
            return 2;
#endif
        };
        auto const n = ioThreads(app_->config());
        if (n > 0)
        {
            auto& io = app_->getIOService();
            std::atomic<int> ran{0};
            for (std::size_t i = 0; i < n; ++i)
                io.post(
                    [&ran] { ran.fetch_add(1, std::memory_order_release); });
            while (ran.load(std::memory_order_acquire) < static_cast<int>(n))
                std::this_thread::yield();
            io.stop();
        }
    }

    NodeBundle(NodeBundle const&) = delete;
    NodeBundle&
    operator=(NodeBundle const&) = delete;

    [[nodiscard]] bool
    isUp() const
    {
        return app_ != nullptr;
    }

    [[nodiscard]] Application&
    app()
    {
        return *app_;
    }

    [[nodiscard]] ManualTimeKeeper&
    clock()
    {
        return *tk_;
    }

    // The configured peer-listening port. When listeners are enabled this is
    // the actual bound port after fixConfigPorts(); in no-listener SimOverlay
    // modes this is a synthetic identity.
    [[nodiscard]] std::uint16_t
    peerPort() const
    {
        auto const p = app_->config()[PORT_PEER].get<std::uint16_t>("port");
        return p.value_or(0);
    }
};

// A container of N in-process nodes that owns the single shared debug log sink
// and provides the overlay/consensus orchestration (connect, wait-for-peers,
// wait-for-validated, ledger agreement, clock pumping).
class MultiNode
{
    beast::unit_test::suite& suite_;
    // Shared virtual steady clock (Stage 2). Created ONLY in virtual-clock
    // mode. Declared before nodes_ so reverse member-destruction tears the
    // nodes down first (their consensus/validation readers stop) and the clock
    // last; the explicit ~MultiNode also clears nodes_ before anything else.
    // nullptr in the default (wall-clock) mode used by rungs A–J.
    std::unique_ptr<ManualSteadyClock> steadyClock_;
    // Stage 3: non-null in STEPPING mode. Owns the single virtual-time
    // scheduler; every node's JobQueue dispatch hook (makeJobHook) enqueues
    // consensus work here and SimTransport delivery is routed here, so the test
    // thread is the sole executor. Declared before nodes_ so it outlives them;
    // teardown dropPending()s it (releasing the JobCounter tokens held in its
    // events) before the nodes stop.
    std::unique_ptr<SteppingController> stepper_;
    // Threaded K=0 tracker: installed only by MultiNode::simConnect() when the
    // network is virtual-clock but not stepping. Declared before nodes_ so it
    // outlives peer teardown and any completion that observes it.
    std::shared_ptr<SimTransportActivity> simActivity_;
    // Genesis NetClock close time, captured at the first add(); the syncClock
    // callback maps scheduler virtual time onto each node's NetClock from this
    // base.
    NetClock::time_point netBase_{};
    // Current network time in non-stepping virtual mode, owned by the test
    // thread. Unlike live node clocks, it advances while every node is stopped.
    // Keep the same per-tick whole-second conversion as the existing driver.
    std::optional<NetClock::time_point> threadedNetTime_;
    struct NodeSlot
    {
        TempDir dbDir;
        std::optional<TrustConfig> trust;
        OverlayFactory overlayFactory;
        Config::HarnessPeerMessageHook peerMessageHook;
        Config::HarnessPeerSendHook peerSendHook;
        Config::HarnessPeerLifecycleHook peerLifecycleHook;
        Config::HarnessValidationHook validationHook;
        ConfigHook configHook;
        bool bindServerListeners = true;
        // Stepping mode: the node's deterministic PRNG. Lives on the stable
        // slot (like the identity and db dir) so a restart CONTINUES the
        // engine's stream rather than resetting it — deterministic within a
        // run, and identical across whole-run replays (fresh MultiNode →
        // fresh engines with the same fixed seeds). Null outside stepping.
        std::unique_ptr<beast::xor_shift_engine> prng;
        // Axis A (explorer-takeoff.md §2): what time this node THINKS it is.
        // Applied by syncClocks to the node's NetClock (TimeKeeper) ONLY —
        // the shared steady clock stays offset-free (elapsed-time physics
        // must remain common; the axis is EPOCH disagreement: close times,
        // validation sign times, handshake timestamps, freshness windows).
        // Whole seconds (syncClocks truncates to NetClock seconds anyway);
        // lives on the slot so a restart keeps lying consistently. May be
        // changed on a LIVE node at a stepping boundary — a deterministic
        // "clock step" (the NTP-jump shape). Constraint: the REAL handshake
        // rejects >20s relative skew between connecting peers
        // (Handshake.cpp handshake-clock-tolerance) — stay inside it, or
        // connect first and skew after.
        std::chrono::seconds clockOffset{0};
        // NetClock sampled in stopNode and restored by restartNodeImpl.
        std::optional<NetClock::time_point> savedNetClock;
    };
    // Stable node slots. The slot outlives the live NodeBundle so a stopped
    // node can restart from the same database path and identity.
    std::vector<std::unique_ptr<NodeSlot>> slots_;
    // Harness-retained SimWires for threaded/hybrid sim meshes. Declared before
    // nodes_ so live PeerImps stop before the retained pipes are released.
    std::vector<std::shared_ptr<SimWire>> simWires_;
    std::vector<std::unique_ptr<NodeBundle>> nodes_;
    std::uint64_t threadedBeat_ = 0;
    // Base seed for the per-node stepping PRNGs (node i seeds at base + i).
    // Fixed default -> replayable out of the box; setPrngSeedBase() before
    // the first add() to sweep seeds or replay a specific stream.
    std::uint64_t prngSeedBase_ = 0xFAB1E5EED0000000ull;

public:
    struct PeerSnapshot
    {
        Peer::id_t id = 0;
        beast::IP::Endpoint remote;
        uint256 closed;
        std::uint32_t minSeq = 0;
        std::uint32_t maxSeq = 0;
    };

    // virtualClock=true brings every node up on the shared manual steady clock
    // with the asio heartbeat suppressed (Config::manualHeartbeat); drive them
    // with tick()/runVirtual() instead of pumpClocks(). Default false keeps the
    // existing wall-clock behaviour byte-for-byte (rungs A–J).
    //
    // stepping=true is Stage 3 STRICT determinism: it implies virtual time AND
    // brings every node up in steppingMode (0 io threads, 0 JobQueue workers,
    // inline strands) with a per-node closed-world dispatch hook, and owns one
    // SteppingController. Drive with runStepping(); inspect via controller().
    // The two flags are independent only in that stepping forces a steady
    // clock.
    explicit MultiNode(
        beast::unit_test::suite& suite,
        bool virtualClock = false,
        bool stepping = false)
        : suite_(suite)
        , steadyClock_(
              (virtualClock || stepping) ? std::make_unique<ManualSteadyClock>()
                                         : nullptr)
        , stepper_(stepping ? std::make_unique<SteppingController>() : nullptr)
        , simActivity_(
              (virtualClock && !stepping)
                  ? std::make_shared<SimTransportActivity>()
                  : nullptr)
    {
        // ONE shared debug sink for the whole harness (Stage 0 §6.6; only one
        // owner process-wide).
        setDebugLogSink(std::make_unique<SuiteJournalSink>(
            "Debug", beast::severities::kFatal, suite));

        // Clock coherence: every scheduler event first advances injected
        // clocks. Normal/global runs still refresh every node from one virtual
        // time. In per-node K horizon mode, the shared steady clock stays on
        // scheduler global time while only the event owner has its NetClock set
        // to global_now - lag(node). That keeps the synthetic slow node
        // coherent: the same lag that burned its per-node beat budget is also
        // the stale close-time view it observes when its handler runs.
        if (stepper_)
            stepper_->setSyncClock(
                [this](
                    std::uint32_t nodeId,
                    SteppingController::time_point globalNow,
                    SteppingController::time_point observedNow,
                    bool ownerOnly) {
                    syncClocks(nodeId, globalNow, observedNow, ownerOnly);
                });
    }

    [[nodiscard]] bool
    isVirtual() const
    {
        return steadyClock_ != nullptr;
    }

    [[nodiscard]] bool
    isStepping() const
    {
        return stepper_ != nullptr;
    }

    // The stepping scheduler/executor (stepping mode only; null otherwise).
    [[nodiscard]] SteppingController&
    controller()
    {
        return *stepper_;
    }

    ~MultiNode()
    {
        // Stepping teardown: (1) enter draining so the delivery routers DROP
        // any residual cross-node completion that fires on the io poll-pump
        // thread as peers close during shutdown (else scheduleDelivery would
        // hard-fail off the stepping thread); (2) release the JobCounter tokens
        // held in the scheduler's claimed-job closures BEFORE the nodes'
        // JobQueues stop (JobQueue::stop() joins jobCounter_; a still-queued
        // counted closure hangs the join).
        if (stepper_)
        {
            stepper_->beginDraining();
            stepper_->dropPending();
        }
        nodes_.clear();  // tear down all nodes (joins their threads)…
        setDebugLogSink(nullptr);  // …then drop the shared sink.
    }

    MultiNode(MultiNode const&) = delete;
    MultiNode&
    operator=(MultiNode const&) = delete;

    // Set the base seed for the per-node stepping PRNGs (node i draws from an
    // engine seeded base + i). Call BEFORE the first add(): the engines are
    // created with their nodes, and a mid-network base change would give the
    // nodes inconsistent provenance — that is a scenario bug, so it throws.
    void
    setPrngSeedBase(std::uint64_t base)
    {
        if (!slots_.empty())
            throw std::logic_error(
                "MultiNode::setPrngSeedBase: nodes already exist; set the "
                "seed before the first add()");
        prngSeedBase_ = base;
    }

    // Bring up and own a node; returns a reference to it (may be !isUp()).
    // An optional OverlayFactory injects a custom Overlay (e.g. SimOverlay).
    NodeBundle&
    add(std::optional<TrustConfig> trust = std::nullopt,
        OverlayFactory overlayFactory = {},
        JobQueue::DispatchHook jobHook = {},
        bool bindServerListeners = true,
        ConfigHook configHook = {})
    {
        auto const id = static_cast<std::uint32_t>(nodes_.size());
        auto slot = std::make_unique<NodeSlot>();
        slot->trust = trust;
        slot->overlayFactory = std::move(overlayFactory);
        slot->configHook = std::move(configHook);
        slot->bindServerListeners = bindServerListeners;
        // Stepping: a per-node deterministic PRNG seeded seedBase + id, so
        // the same (seedBase, node id) draws the same stream in every run —
        // replayable by default, and sweepable via setPrngSeedBase(). The
        // engine rejects seed 0 (xor-shift all-zero state).
        if (stepper_)
        {
            auto const seed = prngSeedBase_ + id;
            if (seed == 0)
                throw std::logic_error(
                    "MultiNode::add: prng seed base + node id must be nonzero");
            slot->prng = std::make_unique<beast::xor_shift_engine>(seed);
        }
        slots_.push_back(std::move(slot));

        // Stepping mode: this node's closed-world hook claims every job onto
        // the shared scheduler. Stepping owns dispatch, so any caller jobHook
        // (the observe-only modes) is replaced here.
        TimeoutCounterTimerFactory timerFactory;
        TimeoutCounterTimerFactory peerTimerFactory;
        if (stepper_)
        {
            jobHook = stepper_->makeJobHook(id);
            // The acquire-retry timer seam (issue 005): every TimeoutCounter
            // this node constructs arms virtual Tier::timer events.
            auto* ctrl = stepper_.get();
            timerFactory = [ctrl,
                            id]() -> std::unique_ptr<TimeoutCounterTimer> {
                return std::make_unique<SteppingTimeoutCounterTimer>(*ctrl, id);
            };
            // ...and every PeerImp's 60s heartbeat likewise (slice 4).
            peerTimerFactory = [ctrl,
                                id]() -> std::unique_ptr<TimeoutCounterTimer> {
                return std::make_unique<SteppingTimeoutCounterTimer>(
                    *ctrl, id, "PeerImp heartbeat");
            };
        }

        // steadyClock_.get() is nullptr outside virtual mode -> wall-clock
        // node.
        nodes_.push_back(std::make_unique<NodeBundle>(
            suite_,
            NodeSpec{
                slots_[id]->dbDir.path(),
                slots_[id]->trust,
                slots_[id]->overlayFactory,
                steadyClock_.get(),
                std::move(jobHook),
                NodeHooks{
                    slots_[id]->peerMessageHook,
                    slots_[id]->peerSendHook,
                    slots_[id]->peerLifecycleHook,
                    slots_[id]->validationHook},
                slots_[id]->configHook,
                slots_[id]->bindServerListeners,
                /*stepping=*/stepper_ != nullptr,
                LedgerStart::Fresh,
                /*injectedPrng=*/slots_[id]->prng.get(),
                std::move(timerFactory),
                std::move(peerTimerFactory),
                /*restoredNetClock=*/threadedNetTime_}));

        if (steadyClock_ && !stepper_ && !threadedNetTime_ &&
            nodes_.back()->isUp())
            threadedNetTime_ = nodes_.back()->clock().now();

        // Capture the genesis NetClock base from the first node for
        // syncClocks(). Gate on isUp(): a setup failure resets app_ (destroying
        // the app-owned ManualTimeKeeper that clock() dereferences) — the
        // caller checks allUp() and bails, so leaving netBase_ default is fine.
        if (stepper_ && id == 0 && nodes_.back()->isUp())
            netBase_ = nodes_.back()->clock().now();
        // A node added MID-SCENARIO (spawn-late) is born with its TimeKeeper
        // at genesis close time while the network's virtual clocks are far
        // ahead — the real handshake rejects that skew ("Peer clock is too
        // far off"). Sync every clock to scheduler time, exactly as
        // restartNode does; a no-op for the normal t=0 bring-up. Threaded
        // virtual nodes receive threadedNetTime_ before app startup above.
        if (stepper_ && nodes_.back()->isUp())
            syncClocks(stepper_->now());
        return *nodes_.back();
    }

    [[nodiscard]] std::size_t
    size() const
    {
        return nodes_.size();
    }

    [[nodiscard]] NodeBundle&
    operator[](std::size_t i)
    {
        return *nodes_[i];
    }

    [[nodiscard]] bool
    isLive(std::size_t i) const
    {
        return i < nodes_.size() && nodes_[i] && nodes_[i]->isUp();
    }

    //@@start issue-024-stable-restart-database-path
    /** Stable database directory retained across stop/restart. */
    [[nodiscard]] std::string
    databasePath(std::size_t i) const
    {
        if (i >= slots_.size())
            throw std::logic_error(
                "MultiNode::databasePath: node index out of range");
        return slots_[i]->dbDir.path();
    }
    //@@end issue-024-stable-restart-database-path

    // Stop a live node while preserving its stable slot: database directory,
    // validator identity, and overlay factory remain available for restart.
    // Call at a controlled stepping boundary after severing its SimWires; this
    // drops this node's residual scheduler closures before its JobQueue shuts
    // down while preserving the rest of the network's queued work.
    void
    stopNode(std::size_t i)
    {
        if (i >= nodes_.size())
            throw std::logic_error(
                "MultiNode::stopNode: node index out of range");
        if (!nodes_[i])
            return;
        if (nodes_[i]->isUp())
            slots_[i]->savedNetClock = nodes_[i]->clock().now();
        if (stepper_)
        {
            stepper_->deactivateNode(static_cast<std::uint32_t>(i));
            stepper_->dropPendingForNode(static_cast<std::uint32_t>(i));
        }
        nodes_[i].reset();
    }

    // Restart a stopped node from the same disk state and identity, loading its
    // latest ledger so consensus scenarios catch up from where they left off.
    NodeBundle&
    restartNode(std::size_t i)
    {
        return restartNodeImpl(i, LedgerStart::LoadLatest);
    }

    // Reopen the same wallet/identity directory without loading a latest
    // ledger. This is for subsystem-persistence scenarios that never validated
    // a loadable ledger; keeping a distinct name preserves restartNode's source
    // and pointer-to-member compatibility.
    NodeBundle&
    restartNodeFresh(std::size_t i)
    {
        return restartNodeImpl(i, LedgerStart::Fresh);
    }

private:
    NodeBundle&
    restartNodeImpl(std::size_t i, LedgerStart const ledgerStart)
    {
        if (i >= nodes_.size())
            throw std::logic_error(
                "MultiNode::restartNode: node index out of range");
        if (nodes_[i])
            throw std::logic_error(
                "MultiNode::restartNode: node is already live");
        JobQueue::DispatchHook jobHook;
        TimeoutCounterTimerFactory timerFactory;
        TimeoutCounterTimerFactory peerTimerFactory;
        if (stepper_)
        {
            stepper_->activateNode(static_cast<std::uint32_t>(i));
            jobHook = stepper_->makeJobHook(static_cast<std::uint32_t>(i));
            auto* ctrl = stepper_.get();
            auto const nid = static_cast<std::uint32_t>(i);
            timerFactory = [ctrl,
                            nid]() -> std::unique_ptr<TimeoutCounterTimer> {
                return std::make_unique<SteppingTimeoutCounterTimer>(
                    *ctrl, nid);
            };
            peerTimerFactory = [ctrl,
                                nid]() -> std::unique_ptr<TimeoutCounterTimer> {
                return std::make_unique<SteppingTimeoutCounterTimer>(
                    *ctrl, nid, "PeerImp heartbeat");
            };
        }
        nodes_[i] = std::make_unique<NodeBundle>(
            suite_,
            NodeSpec{
                slots_[i]->dbDir.path(),
                slots_[i]->trust,
                slots_[i]->overlayFactory,
                steadyClock_.get(),
                std::move(jobHook),
                NodeHooks{
                    slots_[i]->peerMessageHook,
                    slots_[i]->peerSendHook,
                    slots_[i]->peerLifecycleHook,
                    slots_[i]->validationHook},
                slots_[i]->configHook,
                slots_[i]->bindServerListeners,
                /*stepping=*/stepper_ != nullptr,
                ledgerStart,
                /*injectedPrng=*/slots_[i]->prng.get(),
                std::move(timerFactory),
                std::move(peerTimerFactory),
                threadedNetTime_ ? threadedNetTime_
                                 : slots_[i]->savedNetClock});
        if (stepper_ && nodes_[i]->isUp())
            syncClocks(stepper_->now());
        return *nodes_[i];
    }

public:
    [[nodiscard]] bool
    allUp() const
    {
        return std::all_of(nodes_.begin(), nodes_.end(), [](auto const& n) {
            return n && n->isUp();
        });
    }

    // node[from] dials node[to]'s real bound peer port over loopback. No
    // [ips_fixed] needed — a default peered config has free outbound slots.
    void
    connect(std::size_t from, std::size_t to)
    {
        beast::IP::Endpoint const ep(
            boost::asio::ip::make_address(getEnvLocalhostAddr()),
            nodes_[to]->peerPort());
        nodes_[from]->app().overlay().connect(ep);
    }

    // Stand up a retained SimWire between two SimOverlay nodes. In threaded
    // virtual mode (virtualClock=true, stepping=false), this also installs the
    // K=0 activity tracker on the transport completions and pipe bytes.
    std::shared_ptr<SimWire>
    simConnect(std::size_t a, std::size_t b)
    {
        if (!isLive(a) || !isLive(b))
            throw std::logic_error("MultiNode::simConnect: node is not live");
        SimSteppingLink link;
        if (stepper_)
            throw std::logic_error(
                "MultiNode::simConnect: use SteppingNetwork::connect for "
                "stepping links");
        link.activity = simActivity_;
        auto wire =
            ripple::test::simConnect(nodes_[a]->app(), nodes_[b]->app(), link);
        if (wire)
            simWires_.push_back(wire);
        return wire;
    }

    [[nodiscard]] std::size_t
    simBufferedBytes() const
    {
        std::size_t bytes = 0;
        for (auto const& wire : simWires_)
            if (wire && !wire->severed())
                bytes += wire->bufferedBytes();
        return bytes;
    }

    [[nodiscard]] SimTransportActivitySnapshot
    simActivitySnapshot() const
    {
        return simActivity_ ? simActivity_->snapshot()
                            : SimTransportActivitySnapshot{};
    }

    // Wait until no tracked sim transport post is in flight and no wire
    // holds buffered bytes. threadedTick can return while a slow post is
    // still running on a node's io thread, so a caller that asserts
    // quiescence after its last tick waits here first. Covers the sim
    // transport only, not job queues or timers, and needs the transport
    // activity tracker (simOverlayFactory). Returns false on timeout.
    [[nodiscard]] bool
    waitForSimQuiescence(std::chrono::milliseconds timeout) const
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            if (simBufferedBytes() == 0 &&
                simActivitySnapshot().inFlightPosts == 0)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    // Wait until every node has at least `expected` active (post-handshake)
    // peers.
    bool
    waitForPeers(std::size_t expected, std::chrono::milliseconds timeout)
    {
        return waitUntil(
            [&]() {
                return std::all_of(
                    nodes_.begin(), nodes_.end(), [&](auto const& n) {
                        if (!n)
                            return true;
                        return n->app().overlay().size() >= expected;
                    });
            },
            timeout);
    }

    // Wait until every node has fully-validated a ledger with seq >= target.
    bool
    waitForValidated(std::uint32_t target, std::chrono::milliseconds timeout)
    {
        return waitUntil(
            [&]() {
                return std::all_of(
                    nodes_.begin(), nodes_.end(), [&](auto const& n) {
                        if (!n)
                            return true;
                        return n->app()
                                   .getLedgerMaster()
                                   .getValidLedgerIndex() >= target;
                    });
            },
            timeout);
    }

    // The lowest fully-validated ledger index across all nodes.
    [[nodiscard]] std::uint32_t
    minValidated()
    {
        std::uint32_t m = std::numeric_limits<std::uint32_t>::max();
        for (auto const& n : nodes_)
        {
            if (!n)
                continue;
            m = std::min(m, n->app().getLedgerMaster().getValidLedgerIndex());
        }
        if (m == std::numeric_limits<std::uint32_t>::max())
            return 0;
        return m;
    }

    // The hash of node i's ledger at `seq` (uint256{} if it doesn't have it).
    [[nodiscard]] uint256
    ledgerHash(std::size_t i, std::uint32_t seq)
    {
        if (!isLive(i))
            return uint256{};
        auto const l = nodes_[i]->app().getLedgerMaster().getLedgerBySeq(seq);
        return l ? l->info().hash : uint256{};
    }

    [[nodiscard]] std::shared_ptr<Ledger const>
    ledger(std::size_t i, std::uint32_t seq)
    {
        if (!isLive(i))
            return {};
        return nodes_[i]->app().getLedgerMaster().getLedgerBySeq(seq);
    }

    // One entry per transaction in node i's ledger at seq, ordered by the
    // ledger's canonical apply index.
    struct AppliedTx
    {
        std::uint32_t index = 0;
        uint256 txid;
        AccountID account;
        TER result = tesSUCCESS;
    };

    [[nodiscard]] std::vector<AppliedTx>
    appliedTxs(std::size_t i, std::uint32_t seq)
    {
        auto const l = ledger(i, seq);
        if (!l)
            return {};

        std::vector<AppliedTx> out;
        for (auto const& [tx, metaObj] : l->txs)
        {
            if (!tx || !metaObj)
                continue;

            AppliedTx a;
            a.txid = tx->getTransactionID();
            a.account = tx->getAccountID(sfAccount);
            TxMeta const meta(a.txid, l->info().seq, *metaObj);
            a.index = meta.getIndex();
            a.result = meta.getResultTER();
            out.push_back(a);
        }
        std::sort(out.begin(), out.end(), [](auto const& x, auto const& y) {
            return x.index < y.index;
        });
        return out;
    }

    // Node i's CURRENT closed ledger (last closed, NOT necessarily validated) —
    // what the node advertises to peers (getClosedLedgerHash) and the basis for
    // the peer-count fallback. Use this (not ledgerHash/getLedgerBySeq) to
    // observe a sub-quorum group that closes ledgers it cannot fully validate.
    [[nodiscard]] uint256
    closedHash(std::size_t i)
    {
        if (!isLive(i))
            return uint256{};
        auto const l = nodes_[i]->app().getLedgerMaster().getClosedLedger();
        return l ? l->info().hash : uint256{};
    }

    [[nodiscard]] std::uint32_t
    closedSeq(std::size_t i)
    {
        if (!isLive(i))
            return 0;
        auto const l = nodes_[i]->app().getLedgerMaster().getClosedLedger();
        return l ? l->info().seq : 0;
    }

    [[nodiscard]] std::vector<uint256>
    peerClosedHashes(std::size_t i)
    {
        if (!isLive(i))
            return {};
        std::vector<uint256> hashes;
        for (auto const& peer : nodes_[i]->app().overlay().getActivePeers())
            hashes.push_back(peer->getClosedLedgerHash());
        return hashes;
    }

    [[nodiscard]] std::vector<PeerSnapshot>
    peerSnapshots(std::size_t i)
    {
        if (!isLive(i))
            return {};
        std::vector<PeerSnapshot> snapshots;
        for (auto const& peer : nodes_[i]->app().overlay().getActivePeers())
        {
            PeerSnapshot p;
            p.id = peer->id();
            p.remote = peer->getRemoteAddress();
            p.closed = peer->getClosedLedgerHash();
            peer->ledgerRange(p.minSeq, p.maxSeq);
            snapshots.push_back(p);
        }
        return snapshots;
    }

    [[nodiscard]] Json::Value
    validationTrie(std::size_t i)
    {
        if (!isLive(i))
            return {};
        return nodes_[i]->app().getValidations().getJsonTrie();
    }

    // Install node i's passive peer-message observation hook. The hook is
    // always stored on the stable slot, so add()/restartNode() reinstall it on
    // the Config BEFORE the Application exists — race-free, because no io/run
    // thread has spun up yet. A LIVE-node reassignment is only safe in stepping
    // mode (0 io threads
    // + 0 workers → the test thread is the sole reader); in wall-clock modes
    // the node's io/run threads read Config::harnessPeerMessage on every
    // message, so a live assignment is a torn-std::function data race and is
    // rejected. Set the hook BEFORE add()/restartNode() in wall-clock modes.
    void
    setPeerMessageHook(std::size_t i, Config::HarnessPeerMessageHook hook)
    {
        if (i >= slots_.size())
            throw std::logic_error(
                "MultiNode::setPeerMessageHook: node index out of range");
        slots_[i]->peerMessageHook = std::move(hook);
        if (isLive(i))
        {
            // Live (re)assignment is only race-free in stepping mode: 0 io
            // threads + 0 workers means no concurrent reader. In wall-clock
            // modes the node's io/run threads read these fields on every
            // message — set the hook on the slot BEFORE add()/restartNode()
            // instead.
            if (!isStepping())
                throw std::logic_error(
                    "MultiNode::setPeerMessageHook: cannot install a hook on a "
                    "live "
                    "wall-clock node (racy); set it before add()/restart");
            nodes_[i]->app().config().harnessPeerMessage =
                slots_[i]->peerMessageHook;
        }
    }

    // As setPeerMessageHook, for the passive peer-SEND observation hook. Stored
    // on the slot; a live reassignment is stepping-only (wall-clock io/run
    // threads read Config::harnessPeerSend on every send — set it before
    // add()/restartNode()).
    void
    setPeerSendHook(std::size_t i, Config::HarnessPeerSendHook hook)
    {
        if (i >= slots_.size())
            throw std::logic_error(
                "MultiNode::setPeerSendHook: node index out of range");
        slots_[i]->peerSendHook = std::move(hook);
        if (isLive(i))
        {
            // Live (re)assignment is only race-free in stepping mode: 0 io
            // threads + 0 workers means no concurrent reader. In wall-clock
            // modes the node's io/run threads read these fields on every
            // message — set the hook on the slot BEFORE add()/restartNode()
            // instead.
            if (!isStepping())
                throw std::logic_error(
                    "MultiNode::setPeerSendHook: cannot install a hook on a "
                    "live "
                    "wall-clock node (racy); set it before add()/restart");
            nodes_[i]->app().config().harnessPeerSend = slots_[i]->peerSendHook;
        }
    }

    // As setPeerMessageHook, for the passive peer-LIFECYCLE observation hook.
    // Stored on the slot; a live reassignment is stepping-only (wall-clock
    // io/run threads read Config::harnessPeerLifecycle — set it before
    // add()/restartNode()).
    void
    setPeerLifecycleHook(std::size_t i, Config::HarnessPeerLifecycleHook hook)
    {
        if (i >= slots_.size())
            throw std::logic_error(
                "MultiNode::setPeerLifecycleHook: node index out of range");
        slots_[i]->peerLifecycleHook = std::move(hook);
        if (isLive(i))
        {
            // Live (re)assignment is only race-free in stepping mode: 0 io
            // threads + 0 workers means no concurrent reader. In wall-clock
            // modes the node's io/run threads read these fields on every
            // message — set the hook on the slot BEFORE add()/restartNode()
            // instead.
            if (!isStepping())
                throw std::logic_error(
                    "MultiNode::setPeerLifecycleHook: cannot install a hook on "
                    "a live "
                    "wall-clock node (racy); set it before add()/restart");
            nodes_[i]->app().config().harnessPeerLifecycle =
                slots_[i]->peerLifecycleHook;
        }
    }

    // As setPeerMessageHook, for the passive VALIDATION-outcome observation
    // hook. Stored on the slot; a live reassignment is stepping-only
    // (wall-clock io/run threads read Config::harnessValidation — set it before
    // add()/restartNode()).
    void
    setValidationHook(std::size_t i, Config::HarnessValidationHook hook)
    {
        if (i >= slots_.size())
            throw std::logic_error(
                "MultiNode::setValidationHook: node index out of range");
        slots_[i]->validationHook = std::move(hook);
        if (isLive(i))
        {
            // Live (re)assignment is only race-free in stepping mode: 0 io
            // threads + 0 workers means no concurrent reader. In wall-clock
            // modes the node's io/run threads read these fields on every
            // message — set the hook on the slot BEFORE add()/restartNode()
            // instead.
            if (!isStepping())
                throw std::logic_error(
                    "MultiNode::setValidationHook: cannot install a hook on a "
                    "live "
                    "wall-clock node (racy); set it before add()/restart");
            nodes_[i]->app().config().harnessValidation =
                slots_[i]->validationHook;
        }
    }

    [[nodiscard]] std::uint32_t
    validSeq(std::size_t i)
    {
        if (!isLive(i))
            return 0;
        return nodes_[i]->app().getLedgerMaster().getValidLedgerIndex();
    }

    // True iff all nodes agree on the ledger hash at `seq` (and all have it).
    [[nodiscard]] bool
    ledgersAgree(std::uint32_t seq)
    {
        std::optional<uint256> h;
        for (auto const& n : nodes_)
        {
            if (!n)
                continue;
            auto const l = n->app().getLedgerMaster().getLedgerBySeq(seq);
            if (!l)
                return false;
            if (!h)
                h = l->info().hash;
            else if (*h != l->info().hash)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool
    validatedForkFree()
    {
        std::uint32_t hi = 0;
        for (std::uint32_t i = 0; i < nodes_.size(); ++i)
            if (isLive(i))
                hi = std::max(hi, validSeq(i));
        for (std::uint32_t seq = 2; seq <= hi; ++seq)
        {
            std::optional<uint256> h;
            for (std::uint32_t i = 0; i < nodes_.size(); ++i)
            {
                if (!isLive(i) || validSeq(i) < seq)
                    continue;
                auto const hash = ledgerHash(i, seq);
                if (hash == uint256{})
                    continue;
                if (!h)
                    h = hash;
                else if (*h != hash)
                    return false;
            }
        }
        return true;
    }

    // ── §5.1 keystone: submit a transaction through the REAL local entry ─────
    // Autofills Fee/Sequence from node i's CURRENT open ledger (unless the
    // caller already set them), signs as `signer` (single-sign), sterilizes,
    // and drives the result through NetworkOPs::processTransaction with
    // bLocal=true — the same path a real client submission takes:
    // doTransactionSync -> TxQ::apply inside OpenLedger::modify (INLINE on the
    // calling thread), then the REAL overlay relay (which, in stepping mode,
    // fans out as scheduler deliveries at +linkDelay). Call on the stepping
    // thread at a controlled boundary. Returns the (canonicalized) Transaction:
    // getResult() is the open-ledger apply TER (tesSUCCESS/terQUEUED/...).
    std::shared_ptr<Transaction>
    submit(std::size_t i, Json::Value tx, jtx::Account const& signer)
    {
        if (!isLive(i))
            throw std::logic_error("MultiNode::submit: node is not live");
        auto& app = nodes_[i]->app();
        auto const view = app.openLedger().current();
        if (!tx.isMember(jss::Fee))
            jtx::fill_fee(tx, *view);
        if (!tx.isMember(jss::Sequence))
            jtx::fill_seq(tx, *view);
        if (!tx.isMember(jss::TxnSignature))
            jtx::sign(tx, signer);
        auto stx = sterilize(STTx{jtx::parse(tx)});
        std::string reason;
        auto txn = std::make_shared<Transaction>(stx, reason, app);
        if (txn->getStatus() == TransStatus::INVALID)
            throw std::logic_error(
                "MultiNode::submit: invalid transaction: " + reason);
        app.getOPs().processTransaction(
            txn,
            /*bUnlimited=*/false,
            /*bLocal=*/true,
            NetworkOPs::FailHard::no);
        return txn;
    }

    // RAII clock advancer: a thread that bumps every node's ManualTimeKeeper in
    // lockstep with wall time. Consensus reads CLOSE time from the (frozen) TK,
    // so it must advance or openTime never reaches ledgerMinClose and no ledger
    // ever closes. Stops + joins on destruction. Held via unique_ptr
    // (non-movable).
    class ClockPump
    {
        std::vector<std::unique_ptr<NodeBundle>>& nodes_;
        std::atomic<bool> stop_{false};
        std::thread thread_;

    public:
        explicit ClockPump(std::vector<std::unique_ptr<NodeBundle>>& nodes)
            : nodes_(nodes)
        {
            thread_ = std::thread([this]() {
                using namespace std::chrono;
                std::vector<NetClock::time_point> base;
                base.reserve(nodes_.size());
                for (auto& n : nodes_)
                    base.push_back(
                        n ? n->clock().now() : NetClock::time_point{});
                auto const realStart = steady_clock::now();
                while (!stop_.load(std::memory_order_relaxed))
                {
                    auto const delta = duration_cast<NetClock::duration>(
                        steady_clock::now() - realStart);
                    for (std::size_t i = 0; i < nodes_.size(); ++i)
                        if (nodes_[i])
                            nodes_[i]->clock().set(base[i] + delta);
                    std::this_thread::sleep_for(milliseconds{100});
                }
            });
        }

        ~ClockPump()
        {
            stop_.store(true);
            if (thread_.joinable())
                thread_.join();
        }

        ClockPump(ClockPump const&) = delete;
        ClockPump&
        operator=(ClockPump const&) = delete;
    };

    [[nodiscard]] std::unique_ptr<ClockPump>
    pumpClocks()
    {
        return std::make_unique<ClockPump>(nodes_);
    }

    struct ThreadedTickOptions
    {
        std::size_t quietPolls;
        std::chrono::milliseconds pollInterval;
        std::chrono::milliseconds stallTimeout;
        // Callers still record drains against this budget. It does not fail
        // the tick: a slow drain that is still advancing is not a failure.
        std::chrono::milliseconds totalTimeout;
        // Wall-clock backstop for a drain that never goes quiet. Contention
        // stretches a healthy drain; only this cap, or a stall with no
        // ledger and no job-token progress, fails it.
        std::chrono::milliseconds safetyTimeout;
        // Non-quiet polls with a transport that is not moving. Past this, the
        // drain ends so virtual time can advance even if a job keeps the
        // queue occupied.
        std::size_t nonQuietLimit;

        ThreadedTickOptions(
            std::size_t quietPolls_ = 3,
            std::chrono::milliseconds pollInterval_ =
                std::chrono::milliseconds{1},
            std::chrono::milliseconds stallTimeout_ = std::chrono::seconds{30},
            std::chrono::milliseconds totalTimeout_ = std::chrono::seconds{30},
            std::chrono::milliseconds safetyTimeout_ = std::chrono::minutes{10},
            std::size_t nonQuietLimit_ = 4)
            : quietPolls(quietPolls_)
            , pollInterval(pollInterval_)
            , stallTimeout(stallTimeout_)
            , totalTimeout(totalTimeout_)
            , safetyTimeout(safetyTimeout_)
            , nonQuietLimit(nonQuietLimit_)
        {
        }
    };

    struct ThreadedTickStats
    {
        std::uint64_t beat = 0;
        std::size_t polls = 0;
        std::size_t quietPolls = 0;
        std::size_t maxQuietStreak = 0;
        std::chrono::milliseconds wallElapsed{0};
        std::size_t maxBufferedBytes = 0;
        std::size_t maxBusyJobQueues = 0;
        int maxSuspended = 0;
        bool sawBufferedBytes = false;
        bool sawTransportPosts = false;
        bool sawJobWork = false;
        SimTransportActivitySnapshot transportStart;
        SimTransportActivitySnapshot transportEnd;
        std::uint64_t lastJobsStart = 0;
        std::uint64_t completedJobsStart = 0;
        std::size_t idleBusyPolls = 0;
    };

    [[nodiscard]] ThreadedTickStats
    threadedTick(std::chrono::milliseconds dt, ThreadedTickOptions options = {})
    {
        using namespace std::chrono;
        if (!isVirtual() || isStepping())
            throw std::logic_error(
                "MultiNode::threadedTick: requires virtualClock=true and "
                "stepping=false");
        if (!simActivity_)
            throw std::logic_error(
                "MultiNode::threadedTick: no SimTransport activity tracker");
        if (options.quietPolls == 0 || options.nonQuietLimit == 0 ||
            options.pollInterval <= milliseconds{0} ||
            options.stallTimeout <= milliseconds{0} ||
            options.totalTimeout <= milliseconds{0} ||
            options.safetyTimeout <= milliseconds{0})
            throw std::logic_error(
                "MultiNode::threadedTick: invalid budget options");

        ThreadedTickStats stats;
        stats.beat = ++threadedBeat_;
        stats.transportStart = simActivitySnapshot();

        // Advance both domains once, including the runner's current NetClock
        // used when a stopped node returns before the next heartbeat.
        advanceInjectedClocks(dt);

        struct Signal
        {
            std::size_t bufferedBytes = 0;
            SimTransportActivitySnapshot activity;
            std::uint64_t lastJobs = 0;
            std::uint64_t completedJobs = 0;
            std::uint64_t token = 0;
            std::size_t busyJobQueues = 0;
            int suspended = 0;
        };

        auto sample = [this]() {
            Signal s;
            s.bufferedBytes = simBufferedBytes();
            s.activity = simActivitySnapshot();
            for (auto& n : nodes_)
            {
                if (!n)
                    continue;
                auto& jq = n->app().getJobQueue();
                if (!jq.isIdle())
                    ++s.busyJobQueues;
                s.lastJobs += jq.lastJob();
                s.completedJobs += jq.completedJobs();
                s.suspended += jq.suspendedCount();
            }
            s.token = s.activity.epoch + s.lastJobs + s.completedJobs;
            return s;
        };

        auto note = [&stats](Signal const& s) {
            stats.maxBufferedBytes =
                std::max(stats.maxBufferedBytes, s.bufferedBytes);
            stats.maxBusyJobQueues =
                std::max(stats.maxBusyJobQueues, s.busyJobQueues);
            stats.maxSuspended = std::max(stats.maxSuspended, s.suspended);
            stats.sawBufferedBytes = stats.sawBufferedBytes ||
                s.bufferedBytes != 0 ||
                s.activity.bufferedEvents > stats.transportStart.bufferedEvents;
            stats.sawTransportPosts = stats.sawTransportPosts ||
                s.activity.inFlightPosts != 0 ||
                s.activity.readStarted > stats.transportStart.readStarted ||
                s.activity.writeStarted > stats.transportStart.writeStarted ||
                s.activity.shutdownStarted >
                    stats.transportStart.shutdownStarted;
            stats.sawJobWork = stats.sawJobWork || s.busyJobQueues != 0 ||
                s.lastJobs > stats.lastJobsStart ||
                s.completedJobs > stats.completedJobsStart;
        };

        auto diagnostics =
            [this, &stats, &options](
                Signal const& s, char const* bound, milliseconds stallElapsed) {
                std::ostringstream os;
                os << "N=" << nodes_.size() << " beat=" << stats.beat
                   << " bound=" << bound << " polls=" << stats.polls
                   << " quiet=" << stats.quietPolls << "/" << options.quietPolls
                   << " maxQuiet=" << stats.maxQuietStreak
                   << " wallMs=" << stats.wallElapsed.count()
                   << " stallElapsedMs=" << stallElapsed.count()
                   << " stallMs=" << options.stallTimeout.count()
                   << " totalMs=" << options.totalTimeout.count()
                   << " safetyMs=" << options.safetyTimeout.count()
                   << " idleBusy=" << stats.idleBusyPolls
                   << " nonQuietLimit=" << options.nonQuietLimit
                   << " valid=" << minValidated()
                   << " pipeBytes=" << s.bufferedBytes
                   << " maxPipeBytes=" << stats.maxBufferedBytes
                   << " posts=" << s.activity.inFlightPosts
                   << " epoch=" << s.activity.epoch << " token=" << s.token
                   << " tokenComponents={transport:" << s.activity.epoch
                   << ",lastJob:" << s.lastJobs
                   << ",completedJobs:" << s.completedJobs << "}"
                   << " postStarted={read:" << s.activity.readStarted
                   << ",write:" << s.activity.writeStarted
                   << ",shutdown:" << s.activity.shutdownStarted << "}"
                   << " postFinished={read:" << s.activity.readFinished
                   << ",write:" << s.activity.writeFinished
                   << ",shutdown:" << s.activity.shutdownFinished << "}"
                   << " busyJobQueues=" << s.busyJobQueues
                   << " suspended=" << s.suspended << " seqs=[";
                char const* sep = "";
                for (std::size_t i = 0; i < nodes_.size(); ++i)
                {
                    if (!nodes_[i])
                        continue;
                    auto& jq = nodes_[i]->app().getJobQueue();
                    os << sep << "n" << i << "{closed=" << closedSeq(i)
                       << ",valid=" << validSeq(i)
                       << ",idle=" << (jq.isIdle() ? "true" : "false")
                       << ",lastJob=" << jq.lastJob()
                       << ",completedJobs=" << jq.completedJobs()
                       << ",suspended=" << jq.suspendedCount() << "}";
                    sep = ",";
                }
                os << "]";
                return os.str();
            };

        auto const startSignal = sample();
        stats.lastJobsStart = startSignal.lastJobs;
        stats.completedJobsStart = startSignal.completedJobs;

        // 3) one manual heartbeat per node.
        for (auto& n : nodes_)
        {
            if (!n)
                continue;
            n->app().getOPs().heartbeatTick();
            note(sample());
        }

        // 4) K=0 drain: time is frozen while outstanding jobs, pipe bytes, and
        // transport completions settle. Quiescence requires every sampled
        // signal to be zero and the progress token to remain unchanged for M
        // polls.
        auto const drainStart = steady_clock::now();
        auto previousToken = sample().token;
        auto previousValidated = minValidated();
        auto lastProgress = drainStart;
        bool haveTransport = false;
        std::size_t previousBytes = 0;
        std::uint64_t previousReadStarted = 0;
        std::uint64_t previousReadFinished = 0;
        std::uint64_t previousWriteStarted = 0;
        std::uint64_t previousWriteFinished = 0;
        std::uint64_t previousShutdownStarted = 0;
        std::uint64_t previousShutdownFinished = 0;
        Signal last;
        for (;;)
        {
            last = sample();
            ++stats.polls;
            note(last);
            auto const now = steady_clock::now();
            auto const validated = minValidated();
            auto const& posts = last.activity;

            bool const quiet = last.bufferedBytes == 0 &&
                posts.inFlightPosts == 0 && last.busyJobQueues == 0;
            bool const countersStill = haveTransport &&
                last.bufferedBytes == previousBytes &&
                posts.readStarted == previousReadStarted &&
                posts.readFinished == previousReadFinished &&
                posts.writeStarted == previousWriteStarted &&
                posts.writeFinished == previousWriteFinished &&
                posts.shutdownStarted == previousShutdownStarted &&
                posts.shutdownFinished == previousShutdownFinished;
            // Idle when the pipe and the post counters are unchanged. That
            // includes finished == started, and a frozen remainder: a busy
            // queue never looks quiet, so waiting on equality never advances
            // virtual time and those posts never complete.
            bool const transportIdle = countersStill;
            if (!quiet && transportIdle)
                ++stats.idleBusyPolls;
            else
                stats.idleBusyPolls = 0;
            previousBytes = last.bufferedBytes;
            previousReadStarted = posts.readStarted;
            previousReadFinished = posts.readFinished;
            previousWriteStarted = posts.writeStarted;
            previousWriteFinished = posts.writeFinished;
            previousShutdownStarted = posts.shutdownStarted;
            previousShutdownFinished = posts.shutdownFinished;
            haveTransport = true;

            bool const tokenStable = last.token == previousToken;
            bool const ledgerAdvanced = validated != previousValidated;
            if (!tokenStable || ledgerAdvanced)
            {
                previousToken = last.token;
                previousValidated = validated;
                lastProgress = now;
            }
            if (quiet && tokenStable)
                ++stats.quietPolls;
            else
                stats.quietPolls = 0;
            stats.maxQuietStreak =
                std::max(stats.maxQuietStreak, stats.quietPolls);
            stats.wallElapsed = duration_cast<milliseconds>(now - drainStart);
            auto const stallElapsed =
                duration_cast<milliseconds>(now - lastProgress);

            if (stats.quietPolls >= options.quietPolls)
                break;
            if (stats.idleBusyPolls >= options.nonQuietLimit)
                break;
            // No new validated ledger and no job-token change. Wall time here
            // is how long that absence lasted, not a budget for a slow drain.
            if (stallElapsed > options.stallTimeout)
                throw std::runtime_error(
                    "MultiNode::threadedTick: stall bound expired: " +
                    diagnostics(last, "stall", stallElapsed));
            if (stats.wallElapsed > options.safetyTimeout)
                throw std::runtime_error(
                    "MultiNode::threadedTick: safety cap expired: " +
                    diagnostics(last, "safety", stallElapsed));
            std::this_thread::sleep_for(options.pollInterval);
        }

        stats.transportEnd = simActivitySnapshot();
        stats.sawBufferedBytes = stats.sawBufferedBytes ||
            stats.transportEnd.bufferedEvents >
                stats.transportStart.bufferedEvents;
        stats.sawTransportPosts = stats.sawTransportPosts ||
            stats.transportEnd.readStarted > stats.transportStart.readStarted ||
            stats.transportEnd.writeStarted >
                stats.transportStart.writeStarted ||
            stats.transportEnd.shutdownStarted >
                stats.transportStart.shutdownStarted;
        return stats;
    }

    // ── Stage 2: virtual-clock driver (requires virtualClock=true) ───────────
    // Advance virtual time by dt and fire ONE consensus heartbeat per node,
    // then drain. This REPLACES pumpClocks()+asio-heartbeat: there is no
    // wall-clock heartbeat cadence, so consensus advances as fast as the CPU +
    // in-process bus allow. Steps:
    //   1) advance the shared steady clock — consensus' openTime / round
    //   timing; 2) advance every node's NetClock ManualTimeKeeper in lockstep —
    //   closeTime
    //      and timeSincePrevClose, keeping the two clocks coherent;
    //   3) post the heartbeat job on each node (the SAME job the asio timer
    //      posts: processHeartbeatTimer -> consensus_.timerEntry);
    //   4) drain: rendezvous() each node, then a BOUNDED settle so the sim bus
    //      can deliver the proposals/validations just emitted and the receivers
    //      can turn them into jobs and run them before the next tick. Message
    //      delivery still runs on the nodes' run() threads — only the heartbeat
    //      cadence is virtual. The settle is bounded, so tick() never hangs.
    /**
     * Advance injected clocks without issuing the harness consensus heartbeat.
     *
     * This deliberately creates an unusual test-only interval: owner-local
     * elapsed-time logic observes the jump while the harness does not
     * explicitly call `NetworkOPs::heartbeatTick()`. Clock-backed timers may
     * still become ready.
     *
     * @warning Use only when the behavior under test owns its transition
     * directly, such as a manifest expiry sweep. It is not a substitute for
     * `tick()` in consensus or network scenarios.
     */
    void
    advanceVirtualClocksWithoutHeartbeat(std::chrono::milliseconds dt)
    {
        if (stepper_)
            throw std::logic_error(
                "MultiNode::advanceVirtualClocksWithoutHeartbeat: requires "
                "non-stepping virtual "
                "time");
        advanceInjectedClocks(dt);
    }

    void
    tick(
        std::chrono::milliseconds dt,
        std::chrono::milliseconds settle = std::chrono::milliseconds{40})
    {
        using namespace std::chrono;

        // 1) steady clock (elapsed-time source for openTime / round duration).
        // 2) NetClock in lockstep (truncates to whole seconds; pass dt >= 1s).
        // Use the private primitive so the established stepping-mode tick
        // semantics are not narrowed by the public owner-local helper.
        advanceInjectedClocks(dt);

        // 3) one manual heartbeat per node.
        for (auto& n : nodes_)
            if (n)
                n->app().getOPs().heartbeatTick();
        // 4a) wait for the heartbeat jobs themselves to finish.
        for (auto& n : nodes_)
            if (n)
                n->app().getJobQueue().rendezvous();
        // 4b) bounded settle for cross-node message propagation.
        auto const deadline = steady_clock::now() + settle;
        do
        {
            std::this_thread::sleep_for(milliseconds{2});
            for (auto& n : nodes_)
                if (n)
                    n->app().getJobQueue().rendezvous();
        } while (steady_clock::now() < deadline);
    }

    // Drive virtual ticks until every node has fully-validated seq >= target,
    // or maxTicks is reached. Returns the number of ticks actually run (<=
    // maxTicks) — the caller asserts BOTH convergence (minValidated() >=
    // target) and a bounded tick count. No wall-clock heartbeat dependence;
    // cannot wedge.
    std::size_t
    runVirtual(
        std::uint32_t target,
        std::size_t maxTicks,
        std::chrono::milliseconds dt = std::chrono::seconds{1})
    {
        std::size_t ticks = 0;
        while (ticks < maxTicks && minValidated() < target)
        {
            tick(dt);
            ++ticks;
        }
        return ticks;
    }

    // ── Stage 3: stepping driver (requires stepping=true) ────────────────────
    // Pre-schedule `maxHeartbeats` rounds of per-node heartbeats at 1·dt, 2·dt,
    // … (each a clock-synced scheduler event that advances virtual time, then
    // fires getOPs().heartbeatTick(), whose posted heartbeat job the dispatch
    // hook re-enqueues at the same instant), then STEP the one scheduler until
    // every node has fully-validated seq >= target, the queue empties, or
    // maxSteps is reached. Returns events stepped.
    //
    // `skew` (§5.5): per-node heartbeat phase offset — node i beats at
    // k·dt + i·skew instead of every node at the same instant. Real networks
    // are phase-skewed; the same-instant default (skew=0, byte-identical to the
    // original driver) can both mask and manufacture same-instant edge
    // behavior. Keep skew·(N-1) < dt so rounds stay ordered.
    //
    // TIME OWNERSHIP (the injection/cadence contract, design-notes §2): each
    // beat steps only events with when <= that beat's HORIZON (its instant
    // + dt). A harness-injected far-future action (SteppingNetwork::at/in) is
    // therefore NOT drained through out of cadence — the intervening beats
    // fire first and the injection interleaves at its own instant. Without the
    // horizon, a +15s injection would swallow beats 2..14 (drain-to-empty ran
    // through it) and then past-schedule round 2 (a hard throw).
    //
    // Determinism: nothing on this path touches wall time — cross-node delivery
    // is scheduler-routed (S3.4), every job runs on the test thread in (when,
    // tier, nodeId, seq) order, and processing duration is zero (we model
    // ORDERING, not performance). Cannot wedge (bounded by maxSteps).
    struct KProfiledOptions
    {
        std::uint32_t k = 0;
        SteppingController::duration unitCost{};
        HarnessScheduler::ProfiledPacer::NodeMultipliers nodeMultipliers;
        HarnessScheduler::ProfiledPacer::HorizonMode horizonMode =
            HarnessScheduler::ProfiledPacer::HorizonMode::global;

        [[nodiscard]] HarnessScheduler::ProfiledPacer
        pacer() const
        {
            HarnessScheduler::ProfiledPacer out{
                k,
                unitCost,
                HarnessScheduler::ProfiledPacer::KindWeights::eventTypeV1(),
                nodeMultipliers,
                horizonMode};
            return out;
        }
    };

    struct KProfiledRunStats
    {
        std::size_t steps = 0;
        std::size_t beats = 0;
        std::uint32_t minValidated = 0;
        std::uint64_t clampHits = 0;
        SteppingController::duration requestedVirtualAdvance{};
        SteppingController::duration consumedVirtualAdvance{};
        SteppingController::time_point schedulerNow{};
        HarnessScheduler::TraceEvent firstClampEvent;
        SteppingController::duration firstClampRequested{};
        SteppingController::duration firstClampBudget{};
        std::uint32_t firstClampWeight = 0;
        std::uint32_t firstClampNodeMultiplier = 0;
        std::uint64_t weightedEvents = 0;
        std::array<std::uint64_t, HarnessScheduler::kKindCount> eventsByKind{};
        std::array<std::uint64_t, HarnessScheduler::kKindCount>
            weightedEventsByKind{};
        std::vector<SteppingController::duration> consumedPerBeat;
        std::vector<std::uint64_t> clampHitsPerBeat;
        std::vector<std::uint32_t> minValidatedPerBeat;
        std::vector<SteppingController::duration> nodeLag;
        std::vector<std::vector<SteppingController::duration>> nodeLagPerBeat;

        // Why runSteppingProfiled's beat loop stopped. none means the
        // unpaced runStepping path, which does not record a reason.
        enum class Stop : std::uint8_t {
            none,
            heartbeatBudget,
            stepLimit,
            targetReached,
            predicate
        };
        Stop stop = Stop::none;

        [[nodiscard]] bool
        saturated() const
        {
            return clampHits != 0;
        }
    };

    std::size_t
    runStepping(
        std::uint32_t target,
        std::size_t maxHeartbeats,
        std::size_t maxSteps,
        std::chrono::milliseconds dt = std::chrono::seconds{1},
        std::chrono::milliseconds skew = std::chrono::milliseconds{0},
        std::function<void()> const& afterBeat = {})
    {
        // Thin loop over the ONE beat engine (SteppingController::beat): this
        // driver owns only the grid anchoring (t0 + k·dt) and the progress
        // condition (the WHOLE network's validated seq — this is the all-nodes
        // driver). Time ownership, fencing, and heartbeat fan-out live in
        // beat().
        auto const t0 = stepper_->now();
        auto const fire = [this](std::uint32_t i) {
            nodes_[i]->app().getOPs().heartbeatTick();
        };
        auto const stop = [this, target]() { return minValidated() >= target; };
        std::size_t steps = 0;
        for (std::size_t k = 1;
             k <= maxHeartbeats && steps < maxSteps && !stop();
             ++k)
        {
            steps += stepper_->beat(
                t0 + dt * static_cast<std::int64_t>(k),
                SteppingController::BeatSpec{liveNodeIds(), dt, skew},
                fire,
                stop,
                maxSteps - steps);
            if (afterBeat)
                afterBeat();
        }
        return steps;
    }

    // `stopAfterBeat` is deliberately checked only between complete beats,
    // after `afterBeat` has observed the quiescent network. It is not passed to
    // profiledBeat(), whose stop callback may run between individual events.
    // This preserves one ProfiledStepStats (and therefore accumulated per-node
    // lag) while supporting predicates that must read a consistent snapshot.
    KProfiledRunStats
    runSteppingProfiled(
        std::uint32_t target,
        std::size_t maxHeartbeats,
        std::size_t maxSteps,
        KProfiledOptions options,
        std::chrono::milliseconds dt = std::chrono::seconds{1},
        std::chrono::milliseconds skew = std::chrono::milliseconds{0},
        std::function<void()> const& afterBeat = {},
        std::function<bool()> const& stopAfterBeat = {})
    {
        auto const pacer = options.pacer();
        if (!pacer.enabled())
        {
            KProfiledRunStats stats;
            if (!stopAfterBeat)
            {
                stats.steps = runStepping(
                    target, maxHeartbeats, maxSteps, dt, skew, afterBeat);
            }
            else
            {
                // Preserve runStepping's original t0 + k*dt grid. Re-entering
                // runStepping for every beat would re-anchor t0 after an early
                // drain and make K=0 predicate runs observe another cadence.
                auto const t0 = stepper_->now();
                auto const fire = [this](std::uint32_t i) {
                    nodes_[i]->app().getOPs().heartbeatTick();
                };
                auto const stop = [this, target]() {
                    return minValidated() >= target;
                };
                for (std::size_t k = 1; k <= maxHeartbeats &&
                     stats.steps < maxSteps && !stop() && !stopAfterBeat();
                     ++k)
                {
                    stats.steps += stepper_->beat(
                        t0 + dt * static_cast<std::int64_t>(k),
                        SteppingController::BeatSpec{liveNodeIds(), dt, skew},
                        fire,
                        stop,
                        maxSteps - stats.steps);
                    ++stats.beats;
                    if (afterBeat)
                        afterBeat();
                }
            }
            stats.minValidated = minValidated();
            stats.schedulerNow = stepper_->now();
            return stats;
        }

        auto const t0 = stepper_->now();
        auto const fire = [this](std::uint32_t i) {
            nodes_[i]->app().getOPs().heartbeatTick();
        };
        auto const stop = [this, target]() { return minValidated() >= target; };
        KProfiledRunStats runStats;
        SteppingController::ProfiledStepStats stepStats;
        bool stoppedByPredicate = false;
        for (std::size_t k = 1;
             k <= maxHeartbeats && runStats.steps < maxSteps && !stop() &&
             !(stopAfterBeat && (stoppedByPredicate = stopAfterBeat()));
             ++k)
        {
            auto const beforeConsumed = stepStats.consumedAdvance;
            auto const beforeClamps = stepStats.clampHits;
            runStats.steps += stepper_->profiledBeat(
                t0 + dt * static_cast<std::int64_t>(k),
                SteppingController::BeatSpec{liveNodeIds(), dt, skew},
                pacer,
                stepStats,
                fire,
                stop,
                maxSteps - runStats.steps);
            ++runStats.beats;
            runStats.consumedPerBeat.push_back(
                stepStats.consumedAdvance - beforeConsumed);
            runStats.clampHitsPerBeat.push_back(
                stepStats.clampHits - beforeClamps);
            runStats.minValidatedPerBeat.push_back(minValidated());
            runStats.nodeLagPerBeat.push_back(stepStats.nodeLag);
            if (afterBeat)
                afterBeat();
        }

        if (stop())
            runStats.stop = KProfiledRunStats::Stop::targetReached;
        else if (runStats.steps >= maxSteps)
            runStats.stop = KProfiledRunStats::Stop::stepLimit;
        // Record the decision already made at the boundary. A predicate may
        // consume a one-shot event; evaluating it again changes its meaning.
        else if (stoppedByPredicate)
            runStats.stop = KProfiledRunStats::Stop::predicate;
        else
            runStats.stop = KProfiledRunStats::Stop::heartbeatBudget;

        runStats.minValidated = minValidated();
        runStats.clampHits = stepStats.clampHits;
        runStats.requestedVirtualAdvance = stepStats.requestedAdvance;
        runStats.consumedVirtualAdvance = stepStats.consumedAdvance;
        runStats.schedulerNow = stepper_->now();
        runStats.firstClampEvent = stepStats.firstClampEvent;
        runStats.firstClampRequested = stepStats.firstClampRequested;
        runStats.firstClampBudget = stepStats.firstClampBudget;
        runStats.firstClampWeight = stepStats.firstClampWeight;
        runStats.firstClampNodeMultiplier = stepStats.firstClampNodeMultiplier;
        runStats.weightedEvents = stepStats.weightedEvents;
        runStats.eventsByKind = stepStats.eventsByKind;
        runStats.weightedEventsByKind = stepStats.weightedEventsByKind;
        runStats.nodeLag = stepStats.nodeLag;
        return runStats;
    }

    // Variant of runStepping() that fires heartbeat rounds only for the listed
    // live nodes. This is useful for restart/lifecycle probes where
    // same-instant all-node heartbeats are the wrong model: a surviving group
    // should be able to advertise a newer LCL and deliver its
    // status/proposal/validation traffic before a restarted stale node takes
    // its first consensus tick. Beats are horizon-bounded like runStepping (see
    // the time-ownership contract there).
    //
    // `target` is measured over the DRIVEN SET's validated seq, not the whole
    // network's: a deliberately-dark node (late-joiner scenarios) would pin the
    // global minimum at 0 and this driver would burn its whole budget without
    // ever observing the quorum's progress.
    std::size_t
    runSteppingForNodes(
        std::vector<std::uint32_t> const& nodeIds,
        std::uint32_t target,
        std::size_t maxHeartbeats,
        std::size_t maxSteps,
        std::chrono::milliseconds dt = std::chrono::seconds{1},
        std::chrono::milliseconds skew = std::chrono::milliseconds{0},
        std::function<void()> const& afterBeat = {})
    {
        // Thin loop over beat() — the driven-SUBSET driver. Progress is the
        // DRIVEN set's validated seq, never the whole network's: a
        // deliberately-dark node would pin the global minimum at 0 and this
        // driver would burn its whole budget blind to the quorum's progress.
        auto const minOfDriven = [this, &nodeIds]() {
            auto m = std::numeric_limits<std::uint32_t>::max();
            for (auto const i : nodeIds)
                if (isLive(i))
                    m = std::min(
                        m,
                        nodes_[i]
                            ->app()
                            .getLedgerMaster()
                            .getValidLedgerIndex());
            return m == std::numeric_limits<std::uint32_t>::max() ? 0 : m;
        };
        auto const t0 = stepper_->now();
        auto const fire = [this](std::uint32_t i) {
            nodes_[i]->app().getOPs().heartbeatTick();
        };
        auto const stop = [target, &minOfDriven]() {
            return minOfDriven() >= target;
        };
        std::size_t steps = 0;
        for (std::size_t k = 1;
             k <= maxHeartbeats && steps < maxSteps && !stop();
             ++k)
        {
            steps += stepper_->beat(
                t0 + dt * static_cast<std::int64_t>(k),
                SteppingController::BeatSpec{nodeIds, dt, skew},
                fire,
                stop,
                maxSteps - steps);
            if (afterBeat)
                afterBeat();
        }
        return steps;
    }

    // Advance virtual time by `dt` WITHOUT firing any heartbeat. The controller
    // settles due work, then advances every live node's steady + NetClock to
    // the boundary, independently of which nodes are stopped. The settle is
    // HORIZON-BOUNDED: only events inside the gap run;
    // a pre-scheduled injection beyond it waits for the beat that owns it. (The
    // original drained the WHOLE queue — the latent time-ownership bug that
    // motivated the fence; design-notes §2.) Models a QUIET GAP: e.g. a node
    // offline long enough for its trusted validations to age past the
    // validation-current window, the "returning node" state where
    // getPreferred() empties and getPreferredLCL falls back to peer counts.
    // Stepping mode only.
    void
    advanceTime(std::chrono::milliseconds dt)
    {
        auto const until = stepper_->now() + dt;
        stepper_->advanceTimeTo(until, /*maxSteps=*/100000);
    }

private:
    void
    advanceInjectedClocks(std::chrono::milliseconds dt)
    {
        if (!steadyClock_)
            throw std::logic_error(
                "MultiNode::advanceInjectedClocks: requires injected virtual "
                "time");

        steadyClock_->advance(dt);
        auto const netDt = std::chrono::duration_cast<NetClock::duration>(dt);
        if (threadedNetTime_)
            *threadedNetTime_ += netDt;
        for (auto& n : nodes_)
            if (n)
                n->clock().set(n->clock().now() + netDt);
    }

    // The currently-live node ids (the all-nodes driver's driven set).
    [[nodiscard]] std::vector<std::uint32_t>
    liveNodeIds() const
    {
        std::vector<std::uint32_t> ids;
        ids.reserve(nodes_.size());
        for (std::uint32_t i = 0; i < nodes_.size(); ++i)
            if (isLive(i))
                ids.push_back(i);
        return ids;
    }

public:
    // Flush each node's io_context (bounded) until quiescent. In stepping mode
    // the io_context normally has no servicing thread, so residual handlers
    // accumulate; notably a SEVERED peer's CANCELED timer completion — which
    // drives ~PeerImp, releasing its peerFinder key/slot so a later simConnect
    // can reactivate that identity (a heal). Call it at a controlled boundary
    // (e.g. between sever and reconnect), on the stepping thread.
    //
    // GUARDED (issue 005): a poll also fires any wall-armed timer whose real
    // deadline has elapsed — machine-speed wall time deciding what enters the
    // deterministic timeline (observed: PeerImp's 60s heartbeat TMPing burst
    // diverging a corpus -j8 replay). BOTH known arm sites now route through
    // injected virtual timers under steppingMode (TimeoutCounter retries and
    // the PeerImp heartbeat — no timer is gated off anymore); this guard
    // makes any FUTURE escape fail loud instead of flaking: if a poll round
    // enqueued a scheduler event, that is nondeterminism entering the
    // timeline — throw at the boundary that admitted it.
    void
    pumpIo(std::size_t maxRounds = 100)
    {
        auto const before = stepper_ ? stepper_->insertionCount() : 0;
        for (std::size_t round = 0; round < maxRounds; ++round)
        {
            std::size_t ran = 0;
            for (auto& n : nodes_)
                if (n)
                    ran += n->app().getIOService().poll();
            if (ran == 0)
                break;
        }
        if (stepper_ && stepper_->insertionCount() != before)
            throw std::logic_error(
                "MultiNode::pumpIo: an io poll enqueued " +
                std::to_string(stepper_->insertionCount() - before) +
                " scheduler event(s) — a wall-armed handler escaped into "
                "the deterministic timeline (issue 005); gate its arm site");
    }

    // Axis A: set node i's clock offset (what time it THINKS it is — see
    // NodeSlot::clockOffset). Stepping mode only (wall-clock nodes read the
    // real TimeKeeper cadence via ClockPump). Takes effect at the next
    // syncClocks, i.e. the next scheduler event; setting it on a live node
    // at a stepping boundary is the deterministic clock-step scenario.
    void
    setClockOffset(std::size_t i, std::chrono::seconds offset)
    {
        if (i >= slots_.size())
            throw std::logic_error(
                "MultiNode::setClockOffset: node index out of range");
        if (!stepper_)
            throw std::logic_error(
                "MultiNode::setClockOffset: stepping mode only (wall-clock "
                "nodes keep real time)");
        slots_[i]->clockOffset = offset;
    }

private:
    void
    syncNodeNetClock(std::size_t i, SteppingController::time_point observed)
    {
        // Modeled lag can precede the network epoch. Keep arithmetic signed
        // until it is bounded: NetClock uses unsigned seconds and would wrap.
        auto const baseSeconds =
            static_cast<std::int64_t>(netBase_.time_since_epoch().count()) +
            std::chrono::duration_cast<std::chrono::seconds>(
                observed.time_since_epoch())
                .count();
        auto const offset = slots_[i]->clockOffset.count();
        constexpr auto maxSeconds = static_cast<std::int64_t>(
            std::numeric_limits<NetClock::rep>::max());
        auto const bounded = offset > maxSeconds - baseSeconds ? maxSeconds
            : offset < -baseSeconds                            ? 0
                                    : baseSeconds + offset;
        nodes_[i]->clock().set(NetClock::time_point{
            NetClock::duration{static_cast<NetClock::rep>(bounded)}});
    }

    // Advance clocks before an event runs. `globalNow` is the monotonic
    // scheduler time and always drives the shared steady clock. `observedNow`
    // is the event owner's NetClock view. In normal/global mode
    // ownerOnly=false, so all nodes retain the historic lockstep NetClock sync.
    // In per-node K horizon mode ownerOnly=true, so no event owned by node A
    // refreshes node B's NetClock.
    void
    syncClocks(
        std::uint32_t owner,
        SteppingController::time_point globalNow,
        SteppingController::time_point observedNow,
        bool ownerOnly)
    {
        steadyClock_->advanceTo(globalNow);
        if (ownerOnly)
        {
            if (owner < nodes_.size() && nodes_[owner])
                syncNodeNetClock(owner, observedNow);
            return;
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i])
                syncNodeNetClock(i, globalNow);
    }

    void
    syncClocks(SteppingController::time_point t)
    {
        syncClocks(/*owner=*/0, t, t, /*ownerOnly=*/false);
    }
};

}  // namespace ripple::test
