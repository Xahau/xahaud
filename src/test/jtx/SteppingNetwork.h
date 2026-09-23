#pragma once
//------------------------------------------------------------------------------
// SteppingNetwork — small scenario-facing wrapper for the deterministic
// stepping harness. MultiNode remains the low-level engine; this layer packages
// the common test ceremony: N static validators, SimOverlay nodes,
// scheduler-routed SimWire links, full mesh, partitions, reconnects, and
// bounded runs.
//------------------------------------------------------------------------------
#include <test/jtx/MultiNode.h>
#include <test/jtx/SimOverlay.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/RPCHandler.h>
#include <xrpld/rpc/Role.h>

#include <xrpld/app/misc/HashRouter.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/ApiVersion.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/resource/Fees.h>
#include <xrpl/resource/ResourceManager.h>

#include <ripple.pb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ripple::test {

// §5.7 canned fault injectors — sugar over SimPipe::WriteFault. The injector
// is just a closure; anything not covered composes by hand. Determinism
// contract: decisions must be pure functions of (captured engine/counters,
// message sequence) — same seed, same faults, same run.
namespace simfaults {

// Drop every message of `type` (protocol::mt*).
inline SimPipe::WriteFault
dropType(std::uint16_t type)
{
    return [type](std::uint16_t t, std::size_t) {
        SimFault f;
        f.drop = (t == type);
        return f;
    };
}

// Drop every nth message (n=2 → drops the 2nd, 4th, ...), optionally only
// counting messages of `type`.
inline SimPipe::WriteFault
dropEveryNth(int n, std::optional<std::uint16_t> type = std::nullopt)
{
    return [n, type, count = 0](std::uint16_t t, std::size_t) mutable {
        SimFault f;
        if (!type || *type == t)
            f.drop = (++count % n) == 0;
        return f;
    };
}

// Drop with probability p, drawing from the caller's engine — the engine is
// the scenario's replay handle (seed it; keep it alive past the injector).
// Integer arithmetic so the stream is identical across platforms.
inline SimPipe::WriteFault
dropWithProbability(beast::xor_shift_engine& engine, double p)
{
    auto const threshold = static_cast<std::uint64_t>(p * 10000.0);
    return [&engine, threshold](std::uint16_t, std::size_t) {
        SimFault f;
        f.drop = (engine() % 10000) < threshold;
        return f;
    };
}

// Add fixed in-flight latency to every message (stepping mode only; whole
// messages may reorder past faster traffic — that is the point).
inline SimPipe::WriteFault
delayAll(std::chrono::steady_clock::duration d)
{
    return [d](std::uint16_t, std::size_t) {
        SimFault f;
        f.delay = d;
        return f;
    };
}

// Seeded per-MESSAGE jitter: draw a FRESH in-flight delay in [minMs, maxMs]
// for each message from the caller's engine — the delay analog of
// dropWithProbability. Packet times are then random but reproducible: the
// engine is the scenario's replay handle (seed it; keep it alive past the
// injector). Integer-millisecond arithmetic so the stream is identical
// across platforms. Every draw is strictly positive (the scheduler rejects
// a same-instant cross-node delivery), so minMs must be >= 1.
inline SimPipe::WriteFault
delayJittered(
    beast::xor_shift_engine& engine,
    std::chrono::milliseconds minMs,
    std::chrono::milliseconds maxMs)
{
    auto const lo = minMs.count() < 1 ? std::int64_t{1} : minMs.count();
    auto const span = static_cast<std::uint64_t>(
        maxMs.count() >= lo ? maxMs.count() - lo + 1 : 1);
    return [&engine, lo, span](std::uint16_t, std::size_t) {
        SimFault f;
        f.delay = std::chrono::milliseconds{
            lo + static_cast<std::int64_t>(engine() % span)};
        return f;
    };
}

// Deliver `copies` extra copies of every message of `type`.
inline SimPipe::WriteFault
duplicateType(std::uint16_t type, int copies = 1)
{
    return [type, copies](std::uint16_t t, std::size_t) {
        SimFault f;
        if (t == type)
            f.duplicates = copies;
        return f;
    };
}

}  // namespace simfaults

namespace latency {

using Profile = std::function<std::optional<
    std::chrono::steady_clock::duration>(std::uint32_t from, std::uint32_t to)>;

inline Profile
uniform(std::chrono::steady_clock::duration delay)
{
    return [delay](std::uint32_t, std::uint32_t) {
        return std::optional<std::chrono::steady_clock::duration>{delay};
    };
}

inline Profile
ring(
    std::uint32_t n,
    std::chrono::steady_clock::duration near,
    std::chrono::steady_clock::duration far)
{
    return [n, near, far](std::uint32_t from, std::uint32_t to) {
        if (n == 0 || from == to)
            return std::optional<std::chrono::steady_clock::duration>{};
        auto const clockwise = (to + n - from) % n;
        auto const counter = (from + n - to) % n;
        return std::optional<std::chrono::steady_clock::duration>{
            std::min(clockwise, counter) == 1 ? near : far};
    };
}

inline Profile
clusters(
    std::vector<std::vector<std::uint32_t>> groups,
    std::chrono::steady_clock::duration intra,
    std::chrono::steady_clock::duration inter)
{
    return [groups = std::move(groups), intra, inter](
               std::uint32_t from, std::uint32_t to) {
        auto const groupOf = [&groups](std::uint32_t node) {
            for (std::size_t i = 0; i < groups.size(); ++i)
                if (std::find(groups[i].begin(), groups[i].end(), node) !=
                    groups[i].end())
                    return std::optional<std::size_t>{i};
            return std::optional<std::size_t>{};
        };
        auto const a = groupOf(from);
        auto const b = groupOf(to);
        if (!a || !b)
            return std::optional<std::chrono::steady_clock::duration>{};
        return std::optional<std::chrono::steady_clock::duration>{
            *a == *b ? intra : inter};
    };
}

}  // namespace latency

class SteppingNetwork
{
public:
    // §5.8: "how long to TRY" and "how time FLOWS" are different axes
    // (design-notes §4) — a budget bounds effort, a cadence shapes the
    // heartbeat grid. (Defaulted ctors, not NSDMIs: these serve as default
    // arguments inside the enclosing class, where aggregate NSDMIs can't.)
    struct RunBudget
    {
        std::size_t heartbeats;
        std::size_t steps;

        RunBudget(std::size_t heartbeats_ = 120, std::size_t steps_ = 1'000'000)
            : heartbeats(heartbeats_), steps(steps_)
        {
        }
    };

    struct Cadence
    {
        std::chrono::milliseconds dt;
        // Per-node heartbeat phase offset (node i beats at k·dt + i·skew).
        // 0 = the same-instant cadence. Keep skew·(N-1) < dt.
        std::chrono::milliseconds skew;

        Cadence(
            std::chrono::milliseconds dt_ = std::chrono::seconds{1},
            std::chrono::milliseconds skew_ = std::chrono::milliseconds{0})
            : dt(dt_), skew(skew_)
        {
        }
    };

    using KProfiledOptions = MultiNode::KProfiledOptions;
    using KProfiledRunStats = MultiNode::KProfiledRunStats;

    struct Link
    {
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        std::shared_ptr<SimWire> wire;

        [[nodiscard]] bool
        touches(std::uint32_t node) const
        {
            return a == node || b == node;
        }

        [[nodiscard]] bool
        connects(std::uint32_t x, std::uint32_t y) const
        {
            return (a == x && b == y) || (a == y && b == x);
        }

        void
        sever() const
        {
            if (wire)
                wire->sever();
        }
    };

public:
    // ── §5.8 forensics: the recorded material the debugging ladder diffs ──
    // (design-notes §7). Populated only after recordForensics(); hooks live
    // on the stable node slots, so restarted nodes keep recording.
    struct Forensics
    {
        bool enabled = false;
        // outbound sends: (virtual when, sender, msg name, remote port —
        // run-varying; normalize by first-seen order when diffing runs)
        std::vector<
            std::tuple<std::int64_t, std::uint32_t, std::string, std::uint16_t>>
            sendLog;
        // validation lifecycle (harnessValidation hook), formatted lines
        std::vector<std::string> valEvents;
    };

    struct ChainSample
    {
        std::uint32_t beat = 0;
        std::uint32_t closedSeq = 0;
        std::uint32_t validSeq = 0;
        uint256 closedHash;
        uint256 validHash;
        OperatingMode mode = OperatingMode::DISCONNECTED;
    };

    struct ClosedJump
    {
        ChainSample from;
        ChainSample to;
    };

private:
    beast::unit_test::suite& suite_;
    MultiNode net_;
    HarnessScheduler::duration linkDelay_;
    std::vector<ValidatorKey> validators_;
    std::vector<std::string> unl_;
    std::vector<std::vector<std::uint32_t>> trustIds_;
    std::vector<Link> links_;
    std::shared_ptr<Forensics> forensics_ = std::make_shared<Forensics>();
    bool chainHistoryEnabled_ = false;
    std::uint32_t chainHistoryBeat_ = 0;
    std::vector<std::vector<ChainSample>> chainHistory_;
    std::function<void(std::uint32_t, Config&)> configureNode_;

    ConfigHook
    configForNode(std::uint32_t id) const
    {
        if (!configureNode_)
            return {};
        return [configure = configureNode_, id](Config& config) {
            configure(id, config);
        };
    }

    // Install the recording hooks on node i's stable slot (stepping-mode
    // live install is race-free; no traffic flows until the first beat).
    void
    installForensics(std::uint32_t i)
    {
        auto f = forensics_;
        auto* ctl = &net_.controller();
        net_.setPeerSendHook(
            i,
            [f, ctl, i](
                std::uint16_t,
                std::string const& name,
                std::uint32_t,
                beast::IP::Endpoint const& remote,
                std::string const& stage,
                Message&) {
                if (stage == "call")
                    f->sendLog.emplace_back(
                        ctl->now().time_since_epoch().count(),
                        i,
                        name,
                        remote.port());
            });
        net_.setValidationHook(
            i,
            [f, ctl, i](
                std::string const&,
                bool trusted,
                std::uint32_t seq,
                uint256 const& hash,
                std::string const& stage) {
                f->valEvents.push_back(
                    "when=" +
                    std::to_string(ctl->now().time_since_epoch().count()) +
                    " n" + std::to_string(i) + " seq=" + std::to_string(seq) +
                    " " + to_string(hash).substr(0, 8) +
                    (trusted ? " T " : " u ") + stage);
            });
    }

    void
    sampleChainHistory()
    {
        if (!chainHistoryEnabled_)
            return;
        chainHistory_.resize(net_.size());
        for (std::uint32_t i = 0; i < net_.size(); ++i)
        {
            if (!net_.isLive(i))
                continue;
            ChainSample s;
            s.beat = chainHistoryBeat_;
            s.closedSeq = net_.closedSeq(i);
            s.validSeq = net_.validSeq(i);
            s.closedHash = net_.closedHash(i);
            s.validHash =
                s.validSeq == 0 ? uint256{} : net_.ledgerHash(i, s.validSeq);
            s.mode = net_[i].app().getOPs().getOperatingMode();
            chainHistory_[i].push_back(s);
        }
        ++chainHistoryBeat_;
    }

    [[nodiscard]] std::function<void()>
    chainHistoryHook()
    {
        if (!chainHistoryEnabled_)
            return {};
        return [this]() { sampleChainHistory(); };
    }

    [[nodiscard]] bool
    closedDescends(
        std::uint32_t node,
        ChainSample const& from,
        ChainSample const& to)
    {
        if (from.closedHash == uint256{} || to.closedHash == uint256{})
            return true;
        if (from.closedHash == to.closedHash)
            return true;
        if (to.closedSeq < from.closedSeq)
            return false;

        auto l =
            net_[node].app().getLedgerMaster().getLedgerBySeq(to.closedSeq);
        if (!l || l->info().hash != to.closedHash)
            return false;
        while (l && l->info().seq > from.closedSeq)
            l = net_[node].app().getLedgerMaster().getLedgerByHash(
                l->info().parentHash);
        return l && l->info().hash == from.closedHash;
    }

    static OverlayFactory
    simOverlayFactory()
    {
        return [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
    }

    void
    requireNode(std::uint32_t node, char const* what) const
    {
        if (node >= net_.size())
            throw std::logic_error(
                std::string("SteppingNetwork::") + what +
                ": node index out of range");
        if (!net_.isLive(node))
            throw std::logic_error(
                std::string("SteppingNetwork::") + what + ": node is not live");
    }

    void
    requireSlot(std::uint32_t node, char const* what) const
    {
        if (node >= net_.size())
            throw std::logic_error(
                std::string("SteppingNetwork::") + what +
                ": node index out of range");
    }

public:
    explicit SteppingNetwork(
        beast::unit_test::suite& suite,
        HarnessScheduler::duration linkDelay = std::chrono::milliseconds{5})
        : suite_(suite)
        , net_(suite, /*virtualClock=*/true, /*stepping=*/true)
        , linkDelay_(linkDelay)
    {
        if (linkDelay_ <= HarnessScheduler::duration::zero())
            throw std::logic_error(
                "SteppingNetwork: linkDelay must be strictly positive");
    }

    SteppingNetwork(SteppingNetwork const&) = delete;
    SteppingNetwork&
    operator=(SteppingNetwork const&) = delete;

    [[nodiscard]] MultiNode&
    raw()
    {
        return net_;
    }

    [[nodiscard]] MultiNode const&
    raw() const
    {
        return net_;
    }

    [[nodiscard]] MultiNode&
    multiNode()
    {
        return net_;
    }

    [[nodiscard]] MultiNode const&
    multiNode() const
    {
        return net_;
    }

    [[nodiscard]] SteppingController&
    controller()
    {
        return net_.controller();
    }

    [[nodiscard]] NodeBundle&
    node(std::uint32_t i)
    {
        requireNode(i, "node");
        return net_[i];
    }

    [[nodiscard]] std::vector<Link> const&
    linkHistory() const
    {
        return links_;
    }

    // Returns the newest retained SimWire for this pair that was not explicitly
    // severed by the harness. This is a topology handle, not a live-peer proof:
    // PeerImp can still close for other reasons without flipping
    // SimWire::severed_.
    [[nodiscard]] std::shared_ptr<SimWire>
    latestUnseveredWire(std::uint32_t a, std::uint32_t b) const
    {
        for (auto it = links_.rbegin(); it != links_.rend(); ++it)
            if (it->connects(a, b) && it->wire && !it->wire->severed())
                return it->wire;
        return {};
    }

    // Set the base seed for the per-node deterministic PRNGs (node i draws
    // from an engine seeded base + i). Chain BEFORE validators(): same seed →
    // same random choices (acquire sampling, relay selection) → same run;
    // vary it to sweep a scenario across random streams, or pin it to replay
    // a failure a sweep found.
    SteppingNetwork&
    seedPrng(std::uint64_t base)
    {
        net_.setPrngSeedBase(base);
        return *this;
    }

    // Configure each Application before construction, including observers and
    // later restarts. Install before spawning any node; never mutate live
    // config.
    SteppingNetwork&
    configureNodes(std::function<void(std::uint32_t, Config&)> configure)
    {
        if (net_.size() != 0)
            throw std::logic_error(
                "SteppingNetwork::configureNodes: nodes already exist");
        configureNode_ = std::move(configure);
        return *this;
    }

    // §5.8: enable forensics recording — the executed-event trace plus the
    // send/validation logs the debugging ladder (design-notes §7) diffs when
    // a replay assertion fails. Call any time (typically right after
    // construction — expectReplays does); nodes created afterwards record
    // automatically, and nodes already live are retrofitted.
    SteppingNetwork&
    recordForensics()
    {
        if (forensics_->enabled)
            return *this;
        forensics_->enabled = true;
        controller().scheduler().setRecordTrace(true);
        for (std::uint32_t i = 0; i < net_.size(); ++i)
            if (net_.isLive(i))
                installForensics(i);
        return *this;
    }

    [[nodiscard]] Forensics const&
    forensics() const
    {
        return *forensics_;
    }

    SteppingNetwork&
    recordChainHistory()
    {
        if (chainHistoryEnabled_)
            return *this;
        chainHistoryEnabled_ = true;
        chainHistoryBeat_ = 0;
        chainHistory_.clear();
        sampleChainHistory();
        return *this;
    }

    [[nodiscard]] std::vector<ChainSample> const&
    chainHistory(std::uint32_t node) const
    {
        requireSlot(node, "chainHistory");
        static std::vector<ChainSample> const empty;
        return node < chainHistory_.size() ? chainHistory_[node] : empty;
    }

    [[nodiscard]] std::vector<ClosedJump>
    closedJumps(std::uint32_t node)
    {
        requireNode(node, "closedJumps");
        std::vector<ClosedJump> jumps;
        auto const& h = chainHistory(node);
        for (std::size_t i = 1; i < h.size(); ++i)
            if (!closedDescends(node, h[i - 1], h[i]))
                jumps.push_back(ClosedJump{h[i - 1], h[i]});
        return jumps;
    }

    bool
    expectAbandonedClosed(std::uint32_t node, uint256 const& hash)
    {
        requireSlot(node, "expectAbandonedClosed");
        std::optional<ChainSample> closed;
        bool superseded = false;
        for (auto const& sample : chainHistory(node))
        {
            if (!closed && sample.closedHash == hash)
                closed = sample;
            else if (
                closed && sample.closedSeq >= closed->closedSeq &&
                sample.closedHash != hash)
            {
                superseded = true;
            }
        }

        bool validated = false;
        for (std::uint32_t i = 0; i < net_.size(); ++i)
        {
            if (!net_.isLive(i))
                continue;
            for (std::uint32_t seq = 2; seq <= validSeq(i); ++seq)
                if (ledgerHash(i, seq) == hash)
                    validated = true;
        }

        bool ok = suite_.expect(
            closed.has_value(),
            "expectAbandonedClosed: hash was never observed closed");
        ok &= suite_.expect(
            !validated, "expectAbandonedClosed: hash was fully validated");
        ok &= suite_.expect(
            superseded,
            "expectAbandonedClosed: closed hash was not superseded");
        return ok;
    }

    // §5.8 (north-star unweld, design-notes §8): create the validator
    // IDENTITIES — keys + the shared UNL — without bringing any node up.
    // Formation over TIME is then expressible: spawn() subsets at chosen
    // moments, connect() inside at()/in() closures, and the trust graph is
    // fixed from the start even for nodes that don't exist yet.
    SteppingNetwork&
    identities(
        std::size_t count,
        std::string const& seedPrefix = "step-validator-")
    {
        if (net_.size() != 0)
            throw std::logic_error(
                "SteppingNetwork::identities: nodes already exist");
        validators_.clear();
        unl_.clear();
        trustIds_.clear();
        validators_.reserve(count);
        unl_.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            validators_.push_back(
                ValidatorKey::fromPassphrase(seedPrefix + std::to_string(i)));
            unl_.push_back(validators_.back().pubKey);
        }
        trustIds_.resize(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            trustIds_[i].reserve(count);
            for (std::uint32_t j = 0; j < count; ++j)
                trustIds_[i].push_back(j);
        }
        return *this;
    }

    // Override identity `id`'s UNL before spawn. Defaults remain the full
    // identity set; this only changes the selected identity's Config-time
    // validator list. Argument order is not semantic: the stored UNL is
    // canonicalized into identity order so scenario fingerprints cannot depend
    // on brace-list spelling.
    SteppingNetwork&
    trust(std::uint32_t id, std::vector<std::uint32_t> ids)
    {
        if (id >= validators_.size())
            throw std::logic_error(
                "SteppingNetwork::trust: identity index out of range");
        if (id < net_.size())
            throw std::logic_error(
                "SteppingNetwork::trust: identity already spawned");

        std::vector<bool> seen(validators_.size());
        for (auto const trusted : ids)
        {
            if (trusted >= validators_.size())
                throw std::logic_error(
                    "SteppingNetwork::trust: trusted identity index out of "
                    "range");
            if (seen[trusted])
                throw std::logic_error(
                    "SteppingNetwork::trust: duplicate trusted identity");
            seen[trusted] = true;
        }

        std::vector<std::uint32_t> canonical;
        canonical.reserve(ids.size());
        for (std::uint32_t trusted = 0; trusted < validators_.size(); ++trusted)
            if (seen[trusted])
                canonical.push_back(trusted);
        trustIds_[id] = std::move(canonical);
        return *this;
    }

    // Bring identity indexes up as live validator nodes. Node id == identity
    // index, and MultiNode slots are append-only, so spawning must proceed
    // in identity order (each call continues where the last stopped) —
    // enforced, because a gap would silently misalign node ids and UNLs.
    SteppingNetwork&
    spawn(std::initializer_list<std::uint32_t> ids)
    {
        auto factory = simOverlayFactory();
        for (auto const id : ids)
        {
            if (id >= validators_.size())
                throw std::logic_error(
                    "SteppingNetwork::spawn: no such identity (call "
                    "identities(n) first)");
            if (id != net_.size())
                throw std::logic_error(
                    "SteppingNetwork::spawn: ids must continue the spawn "
                    "sequence (node id == identity index)");
            std::vector<std::string> unl;
            unl.reserve(trustIds_[id].size());
            for (auto const trusted : trustIds_[id])
                unl.push_back(validators_[trusted].pubKey);
            net_.add(
                TrustConfig{validators_[id].seed, std::move(unl)},
                factory,
                {},
                true,
                configForNode(id));
            if (forensics_->enabled && net_.isLive(id))
                installForensics(id);
        }
        return *this;
    }

    SteppingNetwork&
    spawnAll()
    {
        for (auto id = static_cast<std::uint32_t>(net_.size());
             id < validators_.size();
             ++id)
            spawn({id});
        return *this;
    }

    // Sugar: identities(count) + spawn all of them.
    SteppingNetwork&
    validators(
        std::size_t count,
        std::string const& seedPrefix = "step-validator-")
    {
        identities(count, seedPrefix);
        for (std::size_t i = 0; i < count; ++i)
            spawn({static_cast<std::uint32_t>(i)});
        return *this;
    }

    // §5.6: append a non-validator OBSERVER node — it trusts the validator set
    // (same UNL) but has no signing identity, so it tracks and fully-validates
    // the network's chain without ever proposing or validating: the shape of a
    // client-facing node. Call after validators() and before mesh() (mesh()
    // wires every node added so far). Returns the new node's id.
    std::uint32_t
    observer()
    {
        if (unl_.empty())
            throw std::logic_error(
                "SteppingNetwork::observer: create validators() first");
        auto const id = static_cast<std::uint32_t>(net_.size());
        net_.add(
            TrustConfig{/*validationSeed=*/{}, unl_},
            simOverlayFactory(),
            {},
            true,
            configForNode(id));
        if (forensics_->enabled && net_.isLive(id))
            installForensics(id);
        return id;
    }

    SteppingNetwork&
    mesh()
    {
        for (std::uint32_t i = 0; i < net_.size(); ++i)
            for (std::uint32_t j = i + 1; j < net_.size(); ++j)
                if (!connect(i, j))
                    throw std::logic_error(
                        "SteppingNetwork::mesh: simConnect failed");
        return *this;
    }

    std::shared_ptr<SimWire>
    connect(std::uint32_t a, std::uint32_t b)
    {
        requireNode(a, "connect");
        requireNode(b, "connect");
        auto wire = simConnect(
            net_[a].app(),
            net_[b].app(),
            SimSteppingLink{&net_.controller(), a, b, linkDelay_});
        if (wire)
            links_.push_back(Link{a, b, wire});
        return wire;
    }

    // §5.7: install a message-fault injector on the live wire carrying
    // traffic FROM `from` TO `to` (direction matters; fault the reverse
    // direction with a second call). Empty injector clears. Install at a
    // stepping boundary — single-threaded stepping makes live install
    // race-free. See simfaults:: for canned injectors.
    SteppingNetwork&
    faultLink(
        std::uint32_t from,
        std::uint32_t to,
        SimPipe::WriteFault injector)
    {
        requireSlot(from, "faultLink");
        requireSlot(to, "faultLink");
        for (auto it = links_.rbegin(); it != links_.rend(); ++it)
        {
            if (it->connects(from, to) && it->wire && !it->wire->severed())
            {
                it->wire->setFault(
                    /*aToB=*/it->a == from, std::move(injector));
                return *this;
            }
        }
        throw std::logic_error(
            "SteppingNetwork::faultLink: no live wire between nodes");
    }

    // Content-aware whole-frame faults, e.g. lose only share-bearing proposals.
    SteppingNetwork&
    faultFrames(
        std::uint32_t from,
        std::uint32_t to,
        SimPipe::FrameFault injector)
    {
        requireSlot(from, "faultFrames");
        requireSlot(to, "faultFrames");
        for (auto it = links_.rbegin(); it != links_.rend(); ++it)
            if (it->connects(from, to) && it->wire && !it->wire->severed())
            {
                it->wire->setFrameFault(it->a == from, std::move(injector));
                return *this;
            }
        throw std::logic_error(
            "SteppingNetwork::faultFrames: no live wire between nodes");
    }

    SteppingNetwork&
    linkDelays(latency::Profile profile)
    {
        for (auto const& link : links_)
        {
            if (!link.wire || link.wire->severed())
                continue;
            auto const apply = [&](std::uint32_t from, std::uint32_t to) {
                auto const delay = profile(from, to);
                // A missing profile entry leaves that directed link untouched;
                // use faultLink(from, to, {}) when a test wants to clear it.
                if (!delay)
                    return;
                if (*delay < HarnessScheduler::duration::zero())
                    throw std::logic_error(
                        "SteppingNetwork::linkDelays: delay must be "
                        "non-negative");
                link.wire->setFault(
                    /*aToB=*/link.a == from, simfaults::delayAll(*delay));
            };
            apply(link.a, link.b);
            apply(link.b, link.a);
        }
        return *this;
    }

    // ── Axis B: position-altitude misbehavior (the byzantine validator) ──
    // What a node SIGNS, not what the wire carries: `from` emits a
    // validation signed with its REAL keys but with caller-chosen content
    // (a fabricated ledger hash / seq / tx-set), through the REAL relay
    // path. This is the position-altitude primitive faultLink is at wire
    // altitude — a signed-but-wrong message no attacker WITHOUT keys could
    // forge, which is exactly the class wire faults cannot express.
    //
    // CONTAINED BY CONSTRUCTION (Nicholas's constraint): ZERO production
    // change. It reuses the production STValidation ctor + Overlay::
    // broadcast() exactly as RCLConsensus::Adaptor::validate does — the
    // harness just supplies the keys (from the slot's validator seed) and
    // the lie. The node's OWN consensus stays honest (we do not process
    // the validation locally); only its peers receive the lie, so this is
    // the two-faced byzantine shape and it EQUIVOCATES naturally: the
    // node's real validate() still broadcasts the honest validation, so a
    // peer sees two validations from one signer at one seq.
    //
    // Deterministic (replayable): secp256k1 validator signing is RFC6979
    // (fixed nonce), the sign time is the node's virtual close time, and
    // the cookie is pinned — so the crafted bytes are identical every run.
    struct ByzantineValidation
    {
        std::uint32_t seq = 0;
        uint256 ledgerHash;     // the (fabricated) ledger it attests to
        uint256 consensusHash;  // the tx-set id it claims (default: zero)
        bool fullValidation = true;
        std::chrono::seconds signTimeSkew{0};  // vs the node's close time
    };

    // ── Pleasant vocabulary (the common case) ──────────────────────────
    // "node `from` LIES that ledger `hash` is validated at `seq`" — the
    // 90% case, no struct, no zero consensus-hash noise. `injectValidation`
    // below is the full-control form. Matches the harness's established
    // voice ("lie about time" is what clockOffset does).

    // Lie to every peer identically.
    SteppingNetwork&
    lieValidation(std::uint32_t from, std::uint32_t seq, uint256 const& hash)
    {
        ByzantineValidation spec;
        spec.seq = seq;
        spec.ledgerHash = hash;
        return injectValidation(from, spec);
    }

    // Lie to ONE peer (pair two calls with different hashes to equivocate).
    SteppingNetwork&
    lieValidationTo(
        std::uint32_t from,
        std::uint32_t to,
        std::uint32_t seq,
        uint256 const& hash)
    {
        ByzantineValidation spec;
        spec.seq = seq;
        spec.ledgerHash = hash;
        return injectValidationTo(from, to, spec);
    }

    // Lie to a whole CAMP with one story (the equivocation-camp idiom):
    // `net.lieValidationTo(byz, {0,1}, seq, hashA)`.
    SteppingNetwork&
    lieValidationTo(
        std::uint32_t from,
        std::vector<std::uint32_t> const& camp,
        std::uint32_t seq,
        uint256 const& hash)
    {
        for (auto const to : camp)
            lieValidationTo(from, to, seq, hash);
        return *this;
    }

    // ── Full-control forms ─────────────────────────────────────────────
    // Lie to EVERY peer identically (one broadcast).
    SteppingNetwork&
    injectValidation(std::uint32_t from, ByzantineValidation const& spec)
    {
        requireNode(from, "injectValidation");
        auto& app = net_[from].app();
        auto const serialized = buildByzantineValidation(from, spec, app);
        protocol::TMValidation val;
        val.set_validation(serialized.data(), serialized.size());
        app.overlay().broadcast(val);
        return *this;
    }

    // Lie to ONE peer only (true equivocation: pair with a different lie —
    // or the honest vote — to another peer, and one signer has told two
    // peers two stories at one seq). Delivers through the byzantine node's
    // real PeerImp send to the specific peer that carries node `to`.
    SteppingNetwork&
    injectValidationTo(
        std::uint32_t from,
        std::uint32_t to,
        ByzantineValidation const& spec)
    {
        requireNode(from, "injectValidationTo");
        requireNode(to, "injectValidationTo");
        auto& app = net_[from].app();
        auto const serialized = buildByzantineValidation(from, spec, app);
        protocol::TMValidation val;
        val.set_validation(serialized.data(), serialized.size());
        auto const msg = std::make_shared<Message>(val, protocol::mtVALIDATION);
        auto const targetPub = net_[to].app().nodeIdentity().first;
        for (auto const& peer : app.overlay().getActivePeers())
            if (peer->getNodePublic() == targetPub)
            {
                peer->send(msg);
                return *this;
            }
        throw std::logic_error(
            "SteppingNetwork::injectValidationTo: node " +
            std::to_string(from) + " has no active peer to node " +
            std::to_string(to));
    }

private:
    // Build + sign the byzantine validation with node `from`'s real keys and
    // caller-chosen content; register the self-suppression exactly as
    // validate() does. Shared by the broadcast and per-peer injectors.
    [[nodiscard]] Blob
    buildByzantineValidation(
        std::uint32_t from,
        ByzantineValidation const& spec,
        Application& app)
    {
        if (from >= validators_.size())
            throw std::logic_error(
                "SteppingNetwork::injectValidation: node has no validator "
                "identity (call validators() first)");
        auto const seed = parseBase58<Seed>(validators_[from].seed);
        if (!seed)
            throw std::logic_error(
                "SteppingNetwork::injectValidation: unparsable validator "
                "seed");
        auto const sk = generateSecretKey(KeyType::secp256k1, *seed);
        auto const pk = derivePublicKey(KeyType::secp256k1, sk);
        auto const signTime = app.timeKeeper().closeTime() + spec.signTimeSkew;

        auto v = std::make_shared<STValidation>(
            signTime, pk, sk, calcNodeID(pk), [&spec](STValidation& v) {
                v.setFieldH256(sfLedgerHash, spec.ledgerHash);
                v.setFieldH256(sfConsensusHash, spec.consensusHash);
                v.setFieldU32(sfLedgerSequence, spec.seq);
                if (spec.fullValidation)
                    v.setFlag(vfFullValidation);
                // Pinned cookie: the one field validate() draws from a
                // per-session random. Fixing it keeps the bytes — and thus
                // the replay — deterministic.
                v.setFieldU64(sfCookie, 0xB47B47B47B47B47Full);
            });

        auto const serialized = v->getSerialized();
        // Suppress our own echo, exactly as validate() does, so relay
        // dedup treats it like a real self-originated validation.
        app.getHashRouter().addSuppression(sha512Half(makeSlice(serialized)));
        return serialized;
    }

public:
    // Axis A ("lie about time", explorer-takeoff.md §2): set what time
    // `node` THINKS it is — a whole-second offset applied to its NetClock
    // (TimeKeeper) at every clock sync. Epoch disagreement only: close
    // times, validation sign times, freshness windows; the shared steady
    // clock (round timing) stays common. Takes effect at the next
    // scheduler event; on a live node that is the deterministic clock-STEP
    // (NTP-jump) scenario. Constraint: the real handshake rejects >20s
    // relative skew between CONNECTING peers — set offsets after the mesh
    // forms, or keep pairwise spread inside the window.
    SteppingNetwork&
    clockOffset(std::uint32_t node, std::chrono::seconds offset)
    {
        requireSlot(node, "clockOffset");
        net_.setClockOffset(node, offset);
        return *this;
    }

    SteppingNetwork&
    cutLinksTo(std::uint32_t node)
    {
        requireSlot(node, "cutLinksTo");
        for (auto const& l : links_)
            if (l.touches(node))
                l.sever();
        return *this;
    }

    SteppingNetwork&
    isolateNodeAndFlush(std::uint32_t node)
    {
        requireNode(node, "isolateNodeAndFlush");
        cutLinksTo(node);
        settle();
        pumpIo();
        return *this;
    }

    SteppingNetwork&
    reconnectNode(std::uint32_t node)
    {
        requireNode(node, "reconnectNode");
        std::size_t connected = 0;
        for (std::uint32_t other = 0; other < net_.size(); ++other)
        {
            if (other == node || !net_.isLive(other))
                continue;
            if (latestUnseveredWire(node, other))
                throw std::logic_error(
                    "SteppingNetwork::reconnectNode: unsevered link already "
                    "retained; cut links before reconnecting");
            if (!connect(node, other))
                throw std::logic_error(
                    "SteppingNetwork::reconnectNode: simConnect failed");
            ++connected;
        }
        if (connected == 0)
            throw std::logic_error(
                "SteppingNetwork::reconnectNode: no live peers to reconnect");
        return *this;
    }

    // Horizon-bounded settle: run the cascade already in flight (sever EOFs,
    // residual deliveries) up to now()+window, and NOTHING beyond — a settle
    // must never execute a far-future injection or a later beat's events.
    // (Replaces the unbounded drain(), which owned the whole future; the
    // whole-queue drop remains teardown-only via dropPending.)
    SteppingNetwork&
    settle(
        HarnessScheduler::duration window = std::chrono::seconds{1},
        std::size_t maxSteps = 100000)
    {
        net_.controller().stepUntilTime(
            net_.controller().now() + window, maxSteps);
        return *this;
    }

    SteppingNetwork&
    pumpIo(std::size_t maxRounds = 100)
    {
        net_.pumpIo(maxRounds);
        return *this;
    }

    SteppingNetwork&
    stopNode(std::uint32_t node)
    {
        requireNode(node, "stopNode");
        for (std::uint32_t other = 0; other < net_.size(); ++other)
        {
            if (other != node && latestUnseveredWire(node, other))
                throw std::logic_error(
                    "SteppingNetwork::stopNode: unsevered link retained; cut "
                    "links before stopping node");
        }
        net_.stopNode(node);
        return *this;
    }

    NodeBundle&
    restartNode(std::uint32_t node)
    {
        requireSlot(node, "restartNode");
        return net_.restartNode(node);
    }

    SteppingNetwork&
    advanceTime(std::chrono::milliseconds dt)
    {
        net_.advanceTime(dt);
        return *this;
    }

    [[nodiscard]] bool
    allUp() const
    {
        return net_.allUp();
    }

    [[nodiscard]] bool
    isLive(std::uint32_t node) const
    {
        return net_.isLive(node);
    }

    [[nodiscard]] bool
    peerCountIs(std::size_t expectedPeers)
    {
        for (std::uint32_t i = 0; i < net_.size(); ++i)
        {
            if (!net_.isLive(i))
                continue;
            if (net_[i].app().overlay().size() != expectedPeers)
                return false;
        }
        return true;
    }

    [[nodiscard]] std::size_t
    liveCount() const
    {
        std::size_t count = 0;
        for (std::uint32_t i = 0; i < net_.size(); ++i)
            if (net_.isLive(i))
                ++count;
        return count;
    }

    [[nodiscard]] bool
    ready(
        std::size_t expectedPeers,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{100})
    {
        return waitUntil([&]() { return peerCountIs(expectedPeers); }, timeout);
    }

    [[nodiscard]] bool
    meshReady(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{100})
    {
        auto const n = liveCount();
        if (n == 0)
            return true;
        return ready(n - 1, timeout);
    }

    std::size_t
    runTo(std::uint32_t target, RunBudget budget = {}, Cadence cadence = {})
    {
        return net_.runStepping(
            target,
            budget.heartbeats,
            budget.steps,
            cadence.dt,
            cadence.skew,
            chainHistoryHook());
    }

    KProfiledRunStats
    runProfiledTo(
        std::uint32_t target,
        KProfiledOptions options,
        RunBudget budget = {},
        Cadence cadence = {},
        std::function<void()> const& afterBeat = {})
    {
        auto const history = chainHistoryHook();
        auto const hook = [history, &afterBeat]() {
            if (history)
                history();
            if (afterBeat)
                afterBeat();
        };
        return net_.runSteppingProfiled(
            target,
            budget.heartbeats,
            budget.steps,
            options,
            cadence.dt,
            cadence.skew,
            hook);
    }

    // Profile continuously until `pred` holds at a quiescent beat boundary.
    // Unlike repeatedly calling runProfiledTo() for one heartbeat, this keeps
    // one ProfiledStepStats alive for the whole run, so per-node lag carries
    // across beats. The predicate is never evaluated between scheduler events.
    KProfiledRunStats
    runProfiledUntil(
        std::function<bool()> const& pred,
        KProfiledOptions options,
        RunBudget budget = {},
        Cadence cadence = {},
        std::function<void()> const& afterBeat = {})
    {
        auto const history = chainHistoryHook();
        auto const hook = [history, &afterBeat]() {
            if (history)
                history();
            if (afterBeat)
                afterBeat();
        };
        return net_.runSteppingProfiled(
            std::numeric_limits<std::uint32_t>::max(),
            budget.heartbeats,
            budget.steps,
            options,
            cadence.dt,
            cadence.skew,
            hook,
            pred);
    }

    // §5.5: ONE imperative beat of network time — a single heartbeat round
    // (per-node, optionally phase-skewed) scheduled dt ahead, then stepped to
    // full quiescence. This is the stepping-mode counterpart of the Stage-2
    // virtual tick(): drive time step-by-step and INSPECT between beats
    //   net.tick(); BEAST_EXPECT(net.closedSeq(0) == ...); net.tick(); ...
    // Each tick re-anchors at current time; runTo(target) holds one fixed grid
    // across its loop. Returns scheduler events stepped this beat.
    std::size_t
    tick(
        std::chrono::milliseconds dt = std::chrono::seconds{1},
        std::chrono::milliseconds skew = std::chrono::milliseconds{0})
    {
        return net_.runStepping(
            std::numeric_limits<std::uint32_t>::max(),
            /*maxHeartbeats=*/1,
            /*maxSteps=*/1'000'000,
            dt,
            skew,
            chainHistoryHook());
    }

    // Queue #4's poll driver: drive network time beat by beat until `pred`
    // holds, bounded by `budget`. Returns whether the predicate held, so a
    // scenario phase reads as a CLAIM with a timeout, not a guessed budget:
    //   BEAST_EXPECT(net.runUntil([&]{ return cohortValidated(seq); }));
    // The predicate is evaluated between beats — at full quiescence — so it
    // always reads a consistent network snapshot. budget.heartbeats bounds
    // the number of beats TRIED (a ceiling on effort, never a definition of
    // success); budget.steps bounds each beat's quiescence drain, as in
    // tick(). runTo(target) remains the ledger-target special case (and
    // stops early inside a beat); use whichever reads better.
    [[nodiscard]] bool
    runUntil(
        std::function<bool()> const& pred,
        RunBudget budget = {},
        Cadence cadence = {})
    {
        if (pred())
            return true;
        for (std::size_t beat = 0; beat < budget.heartbeats; ++beat)
        {
            net_.runStepping(
                std::numeric_limits<std::uint32_t>::max(),
                /*maxHeartbeats=*/1,
                budget.steps,
                cadence.dt,
                cadence.skew,
                chainHistoryHook());
            if (pred())
                return true;
        }
        return false;
    }

    std::size_t
    runOnly(
        std::initializer_list<std::uint32_t> nodes,
        std::uint32_t target = std::numeric_limits<std::uint32_t>::max(),
        RunBudget budget = {},
        Cadence cadence = {})
    {
        for (auto const i : nodes)
            requireSlot(i, "runOnly");
        return net_.runSteppingForNodes(
            std::vector<std::uint32_t>(nodes),
            target,
            budget.heartbeats,
            budget.steps,
            cadence.dt,
            cadence.skew,
            chainHistoryHook());
    }

    std::size_t
    runOnly(std::initializer_list<std::uint32_t> nodes, RunBudget budget)
    {
        return runOnly(
            nodes, std::numeric_limits<std::uint32_t>::max(), budget);
    }

    SteppingNetwork&
    canonicalJobs()
    {
        net_.controller().setJobPolicy(
            SteppingController::JobPolicy::canonicalAllJobs);
        return *this;
    }

    SteppingNetwork&
    lagAccept(std::uint32_t node, HarnessScheduler::duration lag)
    {
        requireSlot(node, "lagAccept");
        net_.controller().setJobLag(
            node, SteppingController::Tier::accept, lag);
        return *this;
    }

    SteppingNetwork&
    clearLag(std::uint32_t node)
    {
        requireSlot(node, "clearLag");
        net_.controller().clearJobLag(node);
        return *this;
    }

    // §5.1: submit a transaction on `node` through the real local submission
    // entry (autofill Fee/Sequence from that node's open ledger, sign as
    // `signer`, NetworkOPs::processTransaction bLocal=true). Stepping-thread
    // only; the open-ledger apply runs inline, the relay lands as scheduler
    // deliveries. See MultiNode::submit.
    std::shared_ptr<Transaction>
    submit(std::uint32_t node, Json::Value tx, jtx::Account const& signer)
    {
        requireNode(node, "submit");
        return net_.submit(node, std::move(tx), signer);
    }

    // §5.3: fund accounts from the genesis master (jtx::Account::master is the
    // "masterpassphrase" account the genesis ledger endows), submitted on
    // `node`, then run the network until the payments validate. FAIL-LOUD:
    // throws if a payment does not apply tesSUCCESS or if any live node's
    // newest validated ledger lacks an account root afterwards — a funding
    // scenario that silently half-works would poison everything built on it.
    SteppingNetwork&
    fund(
        std::uint32_t node,
        jtx::PrettyAmount const& amount,
        std::vector<jtx::Account> const& accounts,
        RunBudget budget = {})
    {
        requireNode(node, "fund");
        for (auto const& account : accounts)
        {
            auto const txn = submit(
                node,
                jtx::pay(jtx::Account::master, account, amount),
                jtx::Account::master);
            if (txn->getResult() != tesSUCCESS)
                throw std::logic_error(
                    "SteppingNetwork::fund: pay(" + account.name() +
                    ") not applied: " + transToken(txn->getResult()));
        }
        runTo(minValidatedSeq() + 3, budget);
        auto const seq = minValidatedSeq();
        for (std::uint32_t i = 0; i < net_.size(); ++i)
        {
            if (!net_.isLive(i))
                continue;
            auto const l = ledger(i, seq);
            for (auto const& account : accounts)
                if (!l || !l->exists(keylet::account(account.id())))
                    throw std::logic_error(
                        "SteppingNetwork::fund: " + account.name() +
                        " not validated on node " + std::to_string(i));
        }
        return *this;
    }

    // §5.5: the GENERIC deterministic-injection combinator. Schedule ANY
    // harness action — a closure over submit()/rpc()/cutLinksTo()/whatever —
    // at an absolute virtual instant, owned by `node` for tie-break purposes
    // and, in per-node K horizon mode, for owner-only NetClock sync. A per-node
    // profiled injection should therefore touch the owner node's app state (or
    // perform its own explicit boundary sync) rather than reading some other
    // node under a stale observed clock.
    // This is deliberately one primitive instead of a scheduleSubmit /
    // scheduleSever / ... family: actions compose as closures, and anything
    // the fn captures is evaluated WHEN THE EVENT FIRES (e.g. a submit inside
    // the closure autofills Sequence/Fee from the open ledger at that moment).
    //
    // Default Tier::process places the action, at its instant, AFTER message
    // deliveries and BEFORE that node's heartbeat — so an injection at a close
    // boundary is visible to the close (the earliest-writer position). Pass
    // another tier to model a different phase relationship. `when` must be in
    // the future (the scheduler rejects the past, structurally).
    SteppingNetwork&
    at(SteppingController::time_point when,
       std::uint32_t node,
       std::function<void()> fn,
       SteppingController::Tier tier = SteppingController::Tier::process)
    {
        requireSlot(node, "at");
        controller().scheduleAt(
            when, tier, node, std::move(fn), HarnessScheduler::Kind::inject);
        return *this;
    }

    // Relative-time sugar for at(): schedule `fn` `delay` after the scheduler's
    // current virtual now(). Tests think in "2.5s into the run".
    SteppingNetwork&
    in(HarnessScheduler::duration delay,
       std::uint32_t node,
       std::function<void()> fn,
       SteppingController::Tier tier = SteppingController::Tier::process)
    {
        return at(controller().now() + delay, node, std::move(fn), tier);
    }

    // §5.4: drive a node's RPC handler table directly — no socket, no server.
    // Builds a JsonContext with the given `role` (default Role::ADMIN — the
    // in-process equivalent of a local admin client; pass Role::USER etc. to
    // probe permissioning) and dispatches through the REAL rpc::doCommand. The
    // call runs INLINE on the stepping thread: READ commands (account_info,
    // ledger, tx, fee, server_info, ...) are stepping-safe by construction —
    // any job a handler posts is claimed by the closed-world hook and an
    // unmodeled one fail-louds. WRITE commands (submit, ...) share §5.1's job
    // model.
    [[nodiscard]] Json::Value
    rpc(std::uint32_t node,
        std::string const& method,
        Json::Value params = Json::objectValue,
        Role role = Role::ADMIN)
    {
        requireNode(node, "rpc");
        auto& app = net_[node].app();
        params[jss::command] = method;
        Resource::Charge loadType = Resource::feeReferenceRPC;
        auto consumer =
            app.getResourceManager().newUnlimitedEndpoint(beast::IP::Endpoint(
                boost::asio::ip::make_address(getEnvLocalhostAddr()), 0));
        RPC::JsonContext context{
            {app.journal("SteppingRPC"),
             app,
             loadType,
             app.getOPs(),
             app.getLedgerMaster(),
             consumer,
             role,
             {},
             {},
             RPC::apiMaximumSupportedVersion},
            std::move(params),
            {}};
        Json::Value result;
        RPC::doCommand(context, result);
        return result;
    }

    [[nodiscard]] std::uint32_t
    minValidatedSeq()
    {
        return net_.minValidated();
    }

    [[nodiscard]] std::uint32_t
    validSeq(std::uint32_t node)
    {
        return net_.validSeq(node);
    }

    // §5.6: a node's network operating mode (DISCONNECTED → CONNECTED →
    // SYNCING → TRACKING → FULL) — the state machine a node walks while
    // joining/forming a network. Pure reader; pair with tick() to watch
    // formation happen beat by beat.
    [[nodiscard]] OperatingMode
    mode(std::uint32_t node)
    {
        requireNode(node, "mode");
        return net_[node].app().getOPs().getOperatingMode();
    }

    [[nodiscard]] std::uint32_t
    closedSeq(std::uint32_t node)
    {
        return net_.closedSeq(node);
    }

    [[nodiscard]] uint256
    closedHash(std::uint32_t node)
    {
        return net_.closedHash(node);
    }

    [[nodiscard]] uint256
    ledgerHash(std::uint32_t node, std::uint32_t seq)
    {
        return net_.ledgerHash(node, seq);
    }

    [[nodiscard]] std::optional<NetClock::time_point>
    ledgerCloseTime(std::uint32_t node, std::uint32_t seq)
    {
        auto const l = ledger(node, seq);
        if (!l)
            return std::nullopt;
        return l->info().closeTime;
    }

    // The SAFETY oracle (promoted from SteppingFaults so hand-written
    // suites and the explorer's edge mode share one definition): no two
    // live nodes ever FULLY VALIDATE different ledgers at the same seq.
    // Closed-ledger divergence is allowed and expected under heavy loss —
    // isolated-by-loss nodes close alone; safety is about validated
    // ledgers only. A node can validate past a seq it never built (quorum
    // arrived at a later seq first), so missing ledgers are skipped, not
    // treated as disagreement.
    // Does every node in `nodes` agree on the ledger at `seq`? True iff each
    // has VALIDATED that far and they share the hash. The subset companion
    // to validatedForkFree() (which is the whole-network safety oracle) and
    // to ledgersAgree() (all nodes): byzantine and partition scenarios
    // constantly need "the HONEST subset converged", not "everyone did".
    //   if (!BEAST_EXPECT(net.validatedAgree({0,1,2,3}, target))) ...
    [[nodiscard]] bool
    validatedAgree(std::vector<std::uint32_t> const& nodes, std::uint32_t seq)
    {
        std::optional<uint256> h;
        for (auto const i : nodes)
        {
            if (!net_.isLive(i) || validSeq(i) < seq)
                return false;
            auto const hash = ledgerHash(i, seq);
            if (!h)
                h = hash;
            else if (*h != hash)
                return false;
        }
        return true;
    }

    [[nodiscard]] bool
    validatedForkFree()
    {
        std::uint32_t hi = 0;
        for (std::uint32_t i = 0; i < net_.size(); ++i)
            if (net_.isLive(i))
                hi = std::max(hi, validSeq(i));
        for (std::uint32_t seq = 2; seq <= hi; ++seq)
        {
            std::optional<uint256> h;
            for (std::uint32_t i = 0; i < net_.size(); ++i)
            {
                if (!net_.isLive(i) || validSeq(i) < seq)
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

    // How many sequences validatedForkFree walks: 2 through the highest live
    // validated sequence, inclusive. That is max-1 when max >= 2. Not a count
    // of pairwise hash comparisons.
    [[nodiscard]] std::uint32_t
    forkCheckedSeqs()
    {
        std::uint32_t hi = 0;
        for (std::uint32_t i = 0; i < net_.size(); ++i)
            if (net_.isLive(i))
                hi = std::max(hi, validSeq(i));
        return hi >= 2 ? hi - 1 : 0;
    }

    [[nodiscard]] std::shared_ptr<Ledger const>
    ledger(std::uint32_t node, std::uint32_t seq)
    {
        return net_.ledger(node, seq);
    }

    // §5.2: one entry per transaction in node's ledger at `seq`, in APPLY order
    // (TxMeta's TransactionIndex — the canonical-set order the ledger was built
    // with, which every honest node must reproduce bit-for-bit).
    using AppliedTx = MultiNode::AppliedTx;

    // Read back what a built ledger actually did: which transactions, in which
    // apply order, with which final TER. Empty if the node lacks the ledger.
    [[nodiscard]] std::vector<AppliedTx>
    appliedTxs(std::uint32_t node, std::uint32_t seq)
    {
        return net_.appliedTxs(node, seq);
    }

    [[nodiscard]] std::shared_ptr<Ledger const>
    closedLedger(std::uint32_t node)
    {
        if (!net_.isLive(node))
            return {};
        return net_[node].app().getLedgerMaster().getClosedLedger();
    }

    [[nodiscard]] bool
    ledgersAgree(std::uint32_t seq)
    {
        return net_.ledgersAgree(seq);
    }

    // ── §5.8 expect* vocabulary: assertions that read as intent ──────────
    // Each reports through the owning suite (so failures count and locate)
    // and returns the verdict so scenarios can bail early.

    // The standard convergence postamble: reached `target`, every node has
    // the identical ledger there, and the structural determinism invariants
    // held. Logs job diagnostics on a convergence miss.
    bool
    expectConverged(std::uint32_t target)
    {
        bool const reached = minValidatedSeq() >= target;
        suite_.expect(
            reached,
            "expectConverged: minValidatedSeq " +
                std::to_string(minValidatedSeq()) + " < target " +
                std::to_string(target));
        if (!reached)
            suite_.log << "  diagnostics: " << jobDiagnostics() << std::endl;
        bool ok = reached;
        ok &= suite_.expect(
            ledgersAgree(target), "expectConverged: ledgers disagree");
        ok &= suite_.expect(
            offThreadJobs() == 0, "expectConverged: off-thread jobs");
        ok &= suite_.expect(failedJobs() == 0, "expectConverged: failed jobs");
        return ok;
    }

    // The transaction landed in node's ledger at `seq` with the expected TER.
    bool
    expectApplied(
        std::uint32_t node,
        std::uint32_t seq,
        uint256 const& txid,
        TER expected = TER{tesSUCCESS})
    {
        for (auto const& tx : appliedTxs(node, seq))
        {
            if (tx.txid == txid)
                return suite_.expect(
                    tx.result == expected,
                    "expectApplied: unexpected TER " + transToken(tx.result));
        }
        return suite_.expect(false, "expectApplied: tx not in ledger");
    }

    // Both transactions landed in node's ledger at `seq`, `first` at a
    // smaller apply index than `second`.
    bool
    expectAppliedBefore(
        std::uint32_t node,
        std::uint32_t seq,
        uint256 const& first,
        uint256 const& second)
    {
        std::optional<std::uint32_t> ia, ib;
        for (auto const& tx : appliedTxs(node, seq))
        {
            if (tx.txid == first)
                ia = tx.index;
            if (tx.txid == second)
                ib = tx.index;
        }
        return suite_.expect(
            ia && ib && *ia < *ib, "expectAppliedBefore: order not satisfied");
    }

    [[nodiscard]] std::size_t
    offThreadJobs()
    {
        return net_.controller().offThreadJobs();
    }

    // Executed-order fingerprint of the whole run so far (scheduler-level).
    // Compares the coarse (time, tier, node, kind) stream; labels and closure
    // identity are not included. Pair it with the scenario's semantic checks.
    [[nodiscard]] std::uint64_t
    traceFingerprint()
    {
        return net_.controller().traceFingerprint();
    }

    [[nodiscard]] std::uint64_t
    traceCount()
    {
        return net_.controller().traceCount();
    }

    [[nodiscard]] std::size_t
    failedJobs()
    {
        return net_.controller().failedJobs();
    }

    [[nodiscard]] std::string
    jobDiagnostics()
    {
        return net_.controller().jobDiagnostics();
    }
};

}  // namespace ripple::test
