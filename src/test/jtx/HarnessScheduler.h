#pragma once
//------------------------------------------------------------------------------
// HarnessScheduler — a deterministic virtual-time event scheduler for the
// in-process multi-node harness (Stage 3, spec §7.1-7.2). It is the structural
// fix for the 3-node convergence flake: instead of letting each node's run()
// thread service io while the virtual clock advances per tick — and *guessing*
// (a wall-clock settle) when the message ricochet has quiesced — every message
// delivery and every resulting bit of processing becomes an EVENT on ONE
// virtual clock, run on ONE stepping thread in a TOTALLY-ORDERED sequence.
// Quiescence is then implicit (the queue is empty), not drained-for.
//
// Modeled on csf::Scheduler (src/test/csf/Scheduler.h) but with the explicit
// same-time tie-break csf lacks: events are totally ordered by the composite
// key
//
//     (when, tier, nodeId, seq)
//
//   when   — the virtual time the event fires (primary order);
//   tier   — priority class AT THE SAME INSTANT (e.g. a message is delivered
//            before the heartbeat that consumes it; the deferred accept runs
//            after the heartbeat returns). A stable, explicit tie-break so a
//            same-time cascade is reproducible regardless of *who* scheduled
//            it;
//   nodeId — the owning node, so two nodes acting at the same instant have a
//            fixed cross-node order;
//   seq    — a monotonic insertion counter; FIFO among otherwise-equal events,
//            and (being unique) it makes the key a TOTAL order so std::set
//            never collapses two distinct events.
//
// Single-threaded by construction: all scheduling AND stepping happen on the
// one stepping thread (handlers run inline from stepOne()), so insertion is
// race-free and the resulting order is fully determined by the key — not by
// thread timing. This is TEST-ONLY infrastructure; it touches no production
// code.
//------------------------------------------------------------------------------
#include <xrpl/basics/contract.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ripple::test {

class HarnessScheduler
{
public:
    // Matches ManualSteadyClock (test/jtx/MultiNode.h): the harness's shared
    // virtual steady clock is a
    // beast::abstract_clock<std::chrono::steady_clock>, so the scheduler
    // advances time in the same units it will later drive.
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::steady_clock::duration;
    // Fingerprints fold duration::count() as-is. This clock is nanoseconds;
    // a coarser period would move every pinned fingerprint.
    static_assert(
        std::ratio_equal_v<duration::period, std::nano>,
        "trace fingerprints assume a nanosecond steady_clock");

    // Priority class among events at the SAME virtual instant (lower runs
    // first). Spaced by 10 so intermediate tiers can be slotted in without
    // renumbering. The values encode the harness's intended same-instant
    // pipeline:
    //   deliver → process → heartbeat → accept → advance → timer.
    //
    // SEMANTICS (pinned for the integration): a tier is a STABLE SORT KEY, NOT
    // a hard phase barrier. stepOne() always runs the single globally-earliest
    // (when,tier,nodeId,seq) event — so an event added at the CURRENT instant
    // in a LOWER tier than the one running becomes the next to run (it is not
    // deferred to "next instant"). The harness therefore observes one causality
    // convention:
    //   - cross-node effects (a delivered message → the reply it triggers) are
    //     scheduled in the FUTURE (now()+linkDelay), so they never collide with
    //     the instant that produced them;
    //   - intra-node sequencing at the SAME instant flows FORWARD in tier —
    //   e.g.
    //     a heartbeat (Tier::heartbeat) defers its ledger build to Tier::accept
    //     at now(), so accept runs right after the heartbeat returns (locks
    //     released), never before it.
    // Code must not rely on a tier being "exhausted" before a later tier
    // starts; it relies only on this forward-causality discipline at scheduling
    // time.
    enum class Tier : int {
        deliver = 0,   // bytes arrive on a node's rx pipe (link delay elapsed)
        process = 10,  // onMessage → the consensus job that message triggers
        heartbeat = 20,  // consensus heartbeat / timerEntry
        accept = 30,  // deferred accept / ledger-build, AFTER heartbeat returns
        advance = 35,  // advance the validated ledger, AFTER accept builds it
        timer = 40,    // acquire / timeout timers
    };

    // PROVENANCE, not ordering: what CREATED an event. Orthogonal to Tier (its
    // same-instant priority). The queue orders by (when, tier, nodeId, seq),
    // so Kind is not a sort key. Profiled pacing does consult it: event cost
    // scales by the kind weight.
    enum class Kind {
        other = 0,
        heartbeat,  // a driver's per-node heartbeat trigger
        deliver,    // a transport delivery completion
        job,        // a JobQueue closure claimed by the dispatch hook
        inject,     // a harness-injected action (SteppingNetwork::at/in)
        timer,      // a quiet-gap boundary / stale-deadline event / an
                    // acquire-retry expiry (the TimeoutCounter seam)
    };
    static constexpr std::size_t kKindCount = 6;

    [[nodiscard]] static constexpr std::size_t
    kindIndex(Kind k)
    {
        return static_cast<std::size_t>(k);
    }

    // One executed event's ordering identity (see traceLog_ / foldTrace).
    // `label` is diagnostic provenance (job name) — NOT part of equality or
    // the fingerprint; it names the divergence once the identity finds it.
    struct TraceEvent
    {
        std::int64_t when = 0;  // time_since_epoch().count()
        int tier = 0;
        std::uint32_t nodeId = 0;
        Kind kind = Kind::other;
        std::string label;

        bool
        operator==(TraceEvent const& o) const
        {
            return when == o.when && tier == o.tier && nodeId == o.nodeId &&
                kind == o.kind;
        }
    };

    // K-profiled pacing is an EXPERIMENTAL test-harness pressure model:
    // each executed scheduler event costs a deterministic duration scaled by
    // one global K. The default table is intentionally small: it distinguishes
    // event provenance kind, not wall CPU time or per-call-site job labels.
    struct ProfiledPacer
    {
        struct KindWeights
        {
            std::array<std::uint32_t, kKindCount> values{};

            [[nodiscard]] static KindWeights
            flat()
            {
                KindWeights out;
                out.values.fill(1);
                return out;
            }

            [[nodiscard]] static KindWeights
            eventTypeV1()
            {
                auto out = flat();
                out.values[kindIndex(Kind::deliver)] = 2;
                out.values[kindIndex(Kind::job)] = 3;
                out.values[kindIndex(Kind::timer)] = 2;
                return out;
            }

            [[nodiscard]] std::uint32_t
            weight(Kind k) const
            {
                return values[kindIndex(k)];
            }
        };

        struct NodeMultipliers
        {
            std::vector<std::uint32_t> values;
            std::uint32_t defaultValue = 1;

            [[nodiscard]] static NodeMultipliers
            uniform(std::uint32_t value)
            {
                NodeMultipliers out;
                out.defaultValue = value;
                return out;
            }

            [[nodiscard]] static NodeMultipliers
            single(
                std::uint32_t nodeId,
                std::uint32_t value,
                std::uint32_t fallback = 1)
            {
                NodeMultipliers out;
                out.defaultValue = fallback;
                out.values.resize(
                    static_cast<std::size_t>(nodeId) + 1, fallback);
                out.values[nodeId] = value;
                return out;
            }

            [[nodiscard]] std::uint32_t
            multiplier(std::uint32_t nodeId) const
            {
                if (nodeId < values.size())
                    return values[nodeId];
                return defaultValue;
            }
        };

        enum class HorizonMode { global, perNode };

        std::uint32_t k = 0;
        duration unitCost{};
        KindWeights weights = KindWeights::eventTypeV1();
        NodeMultipliers nodeMultipliers;
        HorizonMode horizonMode = HorizonMode::global;

        [[nodiscard]] bool
        enabled() const
        {
            return k != 0 && unitCost > duration::zero();
        }

        [[nodiscard]] std::uint32_t
        eventWeight(std::uint32_t nodeId, Kind kind) const
        {
            auto const product =
                static_cast<std::uint64_t>(weights.weight(kind)) *
                static_cast<std::uint64_t>(nodeMultipliers.multiplier(nodeId));
            if (product > std::numeric_limits<std::uint32_t>::max())
                Throw<std::overflow_error>(
                    "HarnessScheduler::ProfiledPacer event weight overflow");
            return static_cast<std::uint32_t>(product);
        }

        [[nodiscard]] duration
        eventCost(std::uint32_t nodeId, Kind kind) const
        {
            auto const scale = static_cast<std::uint64_t>(k) *
                static_cast<std::uint64_t>(eventWeight(nodeId, kind));
            if (unitCost.count() < 0)
                Throw<std::overflow_error>(
                    "HarnessScheduler::ProfiledPacer event cost overflow");
            auto const unit = static_cast<std::uint64_t>(unitCost.count());
            if (unit != 0 &&
                scale > static_cast<std::uint64_t>(
                            std::numeric_limits<duration::rep>::max()) /
                        unit)
                Throw<std::overflow_error>(
                    "HarnessScheduler::ProfiledPacer event cost overflow");
            return unitCost * static_cast<duration::rep>(scale);
        }

        [[nodiscard]] duration
        eventCost(Kind kind) const
        {
            return eventCost(/*nodeId=*/0, kind);
        }
    };

    struct ProfiledStepStats
    {
        std::uint64_t events = 0;
        std::uint64_t clampHits = 0;
        duration requestedAdvance{};
        duration consumedAdvance{};
        bool saturated = false;
        TraceEvent firstClampEvent;
        duration firstClampRequested{};
        duration firstClampBudget{};
        std::uint32_t firstClampWeight = 0;
        std::uint32_t firstClampNodeMultiplier = 0;
        std::uint64_t weightedEvents = 0;
        std::array<std::uint64_t, kKindCount> eventsByKind{};
        std::array<std::uint64_t, kKindCount> weightedEventsByKind{};
        std::vector<duration> nodeLag;
        std::vector<std::optional<time_point>> nodeBeatHorizon;
        std::vector<duration> nodeConsumedThisBeat;

        void
        record(Kind kind, std::uint32_t weight)
        {
            ++eventsByKind[kindIndex(kind)];
            weightedEventsByKind[kindIndex(kind)] += weight;
            weightedEvents += weight;
        }

        void
        ensureNode(std::uint32_t nodeId)
        {
            auto const n = static_cast<std::size_t>(nodeId) + 1;
            if (nodeLag.size() < n)
            {
                nodeLag.resize(n);
                nodeBeatHorizon.resize(n);
                nodeConsumedThisBeat.resize(n);
            }
        }

        [[nodiscard]] duration
        lag(std::uint32_t nodeId) const
        {
            return nodeId < nodeLag.size() ? nodeLag[nodeId] : duration::zero();
        }
    };

    [[nodiscard]] static char const*
    kindName(Kind k)
    {
        switch (k)
        {
            case Kind::heartbeat:
                return "heartbeat";
            case Kind::deliver:
                return "deliver";
            case Kind::job:
                return "job";
            case Kind::inject:
                return "inject";
            case Kind::timer:
                return "timer";
            case Kind::other:
            default:
                return "other";
        }
    }

    // Diagnostic name for a Tier value (TraceEvent stores the raw int).
    [[nodiscard]] static char const*
    tierName(int t)
    {
        switch (static_cast<Tier>(t))
        {
            case Tier::deliver:
                return "deliver";
            case Tier::process:
                return "process";
            case Tier::heartbeat:
                return "heartbeat";
            case Tier::accept:
                return "accept";
            case Tier::advance:
                return "advance";
            case Tier::timer:
                return "timer";
            default:
                return "?";
        }
    }

private:
    struct Event
    {
        time_point when;
        int tier;
        std::uint32_t nodeId;
        std::uint64_t seq;
        Kind kind;          // provenance; ordering ignores this
        std::string label;  // diagnostic provenance; ordering ignores this
        std::function<void()> fn;  // ordering ignores this; operator() is const

        bool
        operator<(Event const& o) const
        {
            return std::tie(when, tier, nodeId, seq) <
                std::tie(o.when, o.tier, o.nodeId, o.seq);
        }
    };

    using Queue = std::set<Event>;

    time_point now_{};  // virtual now; advances to each stepped event's when
    std::uint64_t nextSeq_{0};
    Queue queue_;
    // EXECUTED-ORDER trace fingerprint: every event stepOne() runs folds its
    // (when, tier, nodeId, kind) into a running FNV-1a-style hash. Two
    // runs of the same scenario must produce the same fingerprint — a far
    // stronger claim than matching ledger chains, which a different execution
    // order can still converge to. Cancelled/dropped events are NOT folded
    // (they never executed); their seq consumption is itself deterministic.
    std::uint64_t traceHash_{0xcbf29ce484222325ull};
    std::uint64_t traceCount_{0};
    // Optional full trace RECORDING (diagnosis, not regression): when
    // enabled, every executed event's identity is appended to traceLog_ so a
    // suite can diff two runs and print the FIRST divergent event instead of
    // just knowing the fingerprints differ. Off by default (memory).
    bool recordTrace_ = false;
    std::vector<TraceEvent> traceLog_;
    // TIME-OWNERSHIP fence (design-notes §2): when set, stepping an event with
    // when > fence_ THROWS — the symmetric guard to at()'s no-past rule. A
    // driver plants the fence at its beat horizon so a bug that would drain
    // someone else's future fail-louds on first contact instead of silently
    // executing it. Scheduling beyond the fence stays legal (that's the whole
    // point: future events WAIT); only EXECUTING beyond it is the violation.
    std::optional<time_point> fence_;

public:
    HarnessScheduler() = default;
    HarnessScheduler(HarnessScheduler const&) = delete;
    HarnessScheduler&
    operator=(HarnessScheduler const&) = delete;

    // A handle to a scheduled-but-not-yet-fired event, for cancel(). Valid
    // until the event fires (stepOne erases it) or it is cancelled. Mirrors
    // csf's token.
    struct CancelToken
    {
    private:
        friend class HarnessScheduler;
        Queue::iterator iter_;
        explicit CancelToken(Queue::iterator iter) : iter_(iter)
        {
        }
    };

    [[nodiscard]] time_point
    now() const
    {
        return now_;
    }

    [[nodiscard]] bool
    empty() const
    {
        return queue_.empty();
    }

    // Drop all pending events without running them (destroys their handlers).
    // Used by the harness at teardown to release any counted closures held in
    // events before the owning JobQueue shuts down. Does not move now().
    void
    clear()
    {
        queue_.clear();
    }

    // Drop only pending events owned by one node. Used by lifecycle tests when
    // a node is stopped while the rest of the stepping network remains alive.
    std::size_t
    clearNode(std::uint32_t nodeId)
    {
        std::size_t removed = 0;
        for (auto it = queue_.begin(); it != queue_.end();)
        {
            if (it->nodeId == nodeId)
            {
                it = queue_.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
        return removed;
    }

    [[nodiscard]] std::size_t
    size() const
    {
        return queue_.size();
    }

    // The virtual instant of the earliest pending event (the queue must be
    // non-empty). Lets a driver bound its stepping to a time HORIZON — "settle
    // everything up to my next beat, touch nothing beyond it" — so far-future
    // events (e.g. harness-injected actions) are left for the beats that own
    // them instead of being drained through out of cadence.
    [[nodiscard]] time_point
    nextWhen() const
    {
        if (queue_.empty())
            Throw<std::logic_error>("HarnessScheduler::nextWhen: queue empty");
        return queue_.begin()->when;
    }

    // Plant / clear the time-ownership fence (see fence_). Drivers plant it at
    // their beat horizon and clear it when the beat ends; while planted, any
    // stepOne() that would EXECUTE an event beyond it throws.
    void
    setFence(time_point fence)
    {
        fence_ = fence;
    }

    void
    clearFence()
    {
        fence_.reset();
    }

    [[nodiscard]] std::optional<time_point>
    fence() const
    {
        return fence_;
    }

    // Schedule fn to run at virtual time `when` (in priority class `tier`,
    // owned by `nodeId`). PRECONDITION: `when >= now()`. Scheduling into the
    // past is a causality bug (e.g. a delivered message replying "before" it
    // arrived), so it THROWS — in EVERY build, not silently clamped — to
    // surface exactly the transport-scheduling mistakes this harness exists to
    // make deterministic. (A plain runtime throw, not XRPL_ASSERT, because the
    // latter is stripped in Release — and the determinism proofs run in
    // Release.) For a deliberately- stale event (a timer whose virtual deadline
    // already elapsed) use atOrNow().
    template <class Fn>
    CancelToken
    at(time_point when,
       Tier tier,
       std::uint32_t nodeId,
       Fn&& fn,
       Kind kind = Kind::other,
       std::string label = {})
    {
        if (when < now_)
            Throw<std::logic_error>(
                "HarnessScheduler::at: when is in the past "
                "(use atOrNow() for an intentionally-stale event)");
        return insert(
            when, tier, nodeId, std::forward<Fn>(fn), kind, std::move(label));
    }

    // Like at(), but CLAMPS a past `when` up to now() instead of asserting —
    // for intentionally-stale events only (a timer whose deadline already
    // passed).
    template <class Fn>
    CancelToken
    atOrNow(
        time_point when,
        Tier tier,
        std::uint32_t nodeId,
        Fn&& fn,
        Kind kind = Kind::other,
        std::string label = {})
    {
        return insert(
            when < now_ ? now_ : when,
            tier,
            nodeId,
            std::forward<Fn>(fn),
            kind,
            std::move(label));
    }

    // Schedule fn to run `delay` from now (delay >= 0).
    template <class Fn>
    CancelToken
    in(duration delay,
       Tier tier,
       std::uint32_t nodeId,
       Fn&& fn,
       Kind kind = Kind::other,
       std::string label = {})
    {
        return at(
            now_ + delay,
            tier,
            nodeId,
            std::forward<Fn>(fn),
            kind,
            std::move(label));
    }

    // Cancel a not-yet-fired event. Precondition: the event has neither fired
    // nor already been cancelled (same contract as csf::Scheduler::cancel).
    void
    cancel(CancelToken const& token)
    {
        queue_.erase(token.iter_);
    }

    // Run the earliest event at max(now(), its deadline). Profiled execution
    // may have left nominally overdue events queued; consuming them cannot
    // rewind time when a caller switches back to ordinary stepping.
    // Returns false (without advancing) when the queue is empty. THROWS if a
    // fence is planted and the event lies beyond it — executing someone else's
    // future is a driver bug, never a scheduling choice.
    bool
    stepOne()
    {
        if (queue_.empty())
            return false;
        auto const it = queue_.begin();
        auto const executedWhen = now_ < it->when ? it->when : now_;
        if (fence_ && executedWhen > *fence_)
            Throw<std::logic_error>(
                std::string("HarnessScheduler::stepOne: next event (kind=") +
                kindName(it->kind) + ", node " + std::to_string(it->nodeId) +
                ") lies beyond the planted fence — a driver tried to execute "
                "a future it does not own");
        now_ = executedWhen;
        // Fold the event's identity into the executed-order fingerprint
        // BEFORE running it (a throwing handler was still executed).
        foldTraceAt(*it, executedWhen);
        // Copy the handler out and erase BEFORE running so the handler may
        // safely (re)schedule, and an event can't observe itself still queued.
        auto fn = it->fn;
        queue_.erase(it);
        fn();
        return true;
    }

    // Run the single earliest event under K-profiled pacing.
    //
    // HorizonMode::global is the original model: the event is CHARGED BEFORE
    // its handler runs, so the handler observes the charged completion time via
    // scheduler_.now() / the clock-sync wrapper. If the requested charge would
    // pass the beat horizon, the event still executes at the horizon, the clamp
    // is recorded, and the caller should stop the beat.
    //
    // HorizonMode::perNode is the scheduler/controller checkpoint for modeled
    // slow-node pressure: global ordering time does not advance by the charge.
    // Consumed charge burns that node's beat budget, and over-budget charge
    // accumulates as node-local observed-time lag for later owner sync.
    //
    // This path is intentionally separate from stepOne(): K=0 determinism gates
    // continue to exercise the original zero-duration scheduler byte-for-byte.
    bool
    stepOneProfiled(
        time_point horizon,
        ProfiledPacer const& pacer,
        ProfiledStepStats& stats,
        std::function<void(std::uint32_t, time_point)> const& beforeEvent = {})
    {
        if (!pacer.enabled())
        {
            bool const ran = stepOne();
            if (ran)
                ++stats.events;
            return ran;
        }
        if (queue_.empty())
            return false;
        auto const it = queue_.begin();
        auto const start = now_ < it->when ? it->when : now_;
        if (fence_ && start > *fence_)
            Throw<std::logic_error>(
                std::string("HarnessScheduler::stepOneProfiled: next event "
                            "(kind=") +
                kindName(it->kind) + ", node " + std::to_string(it->nodeId) +
                ") lies beyond the planted fence — a driver tried to execute "
                "a future it does not own");

        auto const kindWeight = pacer.weights.weight(it->kind);
        auto const weight = pacer.eventWeight(it->nodeId, it->kind);
        auto const requested = pacer.eventCost(it->nodeId, it->kind);
        auto const nodeMultiplier =
            pacer.nodeMultipliers.multiplier(it->nodeId);
        stats.ensureNode(it->nodeId);
        auto& nodeLag = stats.nodeLag[it->nodeId];
        auto& nodeHorizon = stats.nodeBeatHorizon[it->nodeId];
        auto& nodeConsumed = stats.nodeConsumedThisBeat[it->nodeId];
        if (!nodeHorizon || *nodeHorizon != horizon)
        {
            nodeHorizon = horizon;
            nodeConsumed = duration::zero();
        }

        auto budget = start < horizon ? horizon - start : duration::zero();
        if (pacer.horizonMode == ProfiledPacer::HorizonMode::perNode)
            budget = nodeConsumed < budget ? budget - nodeConsumed
                                           : duration::zero();
        auto const consumed = requested <= budget ? requested : budget;
        nodeConsumed += consumed;
        auto const executedWhen =
            pacer.horizonMode == ProfiledPacer::HorizonMode::global
            ? start + consumed
            : start;
        bool const clamped = consumed != requested;
        if (pacer.horizonMode == ProfiledPacer::HorizonMode::perNode && clamped)
            nodeLag += requested - consumed;
        auto const observedWhen =
            pacer.horizonMode == ProfiledPacer::HorizonMode::perNode
            ? start - nodeLag
            : executedWhen;

        now_ = executedWhen;
        stats.record(it->kind, weight);
        stats.requestedAdvance += requested;
        stats.consumedAdvance += consumed;
        if (clamped)
        {
            ++stats.clampHits;
            if (!stats.saturated)
            {
                stats.saturated = true;
                stats.firstClampEvent = traceEventAt(*it, executedWhen);
                stats.firstClampRequested = requested;
                stats.firstClampBudget = budget;
                stats.firstClampWeight = kindWeight;
                stats.firstClampNodeMultiplier = nodeMultiplier;
            }
        }

        if (beforeEvent)
            beforeEvent(it->nodeId, observedWhen);
        foldTraceAt(*it, executedWhen);
        auto fn = it->fn;
        queue_.erase(it);
        ++stats.events;
        fn();
        return true;
    }

    // Fingerprint of the executed (time, tier, node, kind) tuple stream.
    // Labels and closure identity are excluded, so matching fingerprints and
    // counts are a coarse replay check, not proof of identical work.
    [[nodiscard]] std::uint64_t
    traceFingerprint() const
    {
        return traceHash_;
    }

    [[nodiscard]] std::uint64_t
    traceCount() const
    {
        return traceCount_;
    }

    // Monotonic count of every event ever INSERTED (scheduled), executed or
    // not. Lets a boundary (MultiNode::pumpIo) prove an io poll enqueued
    // nothing — the issue-005 guard: a wall-armed timer completion that
    // schedules work during a poll is nondeterminism entering the timeline.
    [[nodiscard]] std::uint64_t
    insertionCount() const
    {
        return nextSeq_;
    }

    // Enable full trace recording (diagnosis): call BEFORE any events run so
    // two runs' logs align from index 0.
    void
    setRecordTrace(bool on)
    {
        recordTrace_ = on;
    }

    [[nodiscard]] std::vector<TraceEvent> const&
    traceLog() const
    {
        return traceLog_;
    }

    // Run events until the queue is empty (including events the handlers add).
    // Returns true if at least one event ran.
    bool
    step()
    {
        bool ran = false;
        while (stepOne())
            ran = true;
        return ran;
    }

    // Run all events with when <= `until`, then advance now() to `until`.
    // Events scheduled by handlers that still fall within [now, until] are also
    // run. Returns true if any events remain after `until`.
    bool
    stepUntil(time_point until)
    {
        while (!queue_.empty() && queue_.begin()->when <= until)
            stepOne();
        if (now_ < until)
            now_ = until;
        return !queue_.empty();
    }

    // Run events while pred() holds, checked before each step. Returns true if
    // at least one event ran.
    template <class Pred>
    bool
    stepWhile(Pred&& pred)
    {
        bool ran = false;
        while (pred() && stepOne())
            ran = true;
        return ran;
    }

private:
    // FNV-1a-style 64-bit mix of one executed event's ordering identity.
    // Not cryptographic — a regression fingerprint.
    //
    // Deliberately EXCLUDES e.seq: seq is an insertion LABEL, not semantics.
    // Send-side loops (e.g. OverlayImpl::relay) iterate unordered peer
    // containers whose bucket order shifts with the process-global peer-id
    // range, so the same delivery earns a different seq NUMBER in two
    // otherwise-identical runs — while the executed order is already fully
    // determined by (when, tier, nodeId) plus the deterministic sequencing
    // of same-key insertions. Folding seq made the fingerprint flag label
    // noise as divergence (found by SteppingLargeNet's first run).
    [[nodiscard]] TraceEvent
    traceEventAt(Event const& e, time_point when) const
    {
        return TraceEvent{
            when.time_since_epoch().count(), e.tier, e.nodeId, e.kind, e.label};
    }

    void
    foldTraceAt(Event const& e, time_point when)
    {
        auto const mix = [this](std::uint64_t v) {
            traceHash_ ^= v;
            traceHash_ *= 0x100000001b3ull;
        };
        mix(static_cast<std::uint64_t>(when.time_since_epoch().count()));
        mix(static_cast<std::uint64_t>(e.tier));
        mix(e.nodeId);
        mix(static_cast<std::uint64_t>(e.kind));
        ++traceCount_;
        if (recordTrace_)
            traceLog_.push_back(traceEventAt(e, when));
    }

    // Shared insert for at()/atOrNow(): stamps the monotonic seq (total order +
    // FIFO tie-break among equal (when,tier,nodeId)) and returns a cancel
    // handle. `when` is taken as already validated (at) or clamped (atOrNow) by
    // the caller.
    template <class Fn>
    CancelToken
    insert(
        time_point when,
        Tier tier,
        std::uint32_t nodeId,
        Fn&& fn,
        Kind kind,
        std::string label = {})
    {
        auto const [it, inserted] = queue_.insert(Event{
            when,
            static_cast<int>(tier),
            nodeId,
            nextSeq_++,
            kind,
            std::move(label),
            std::function<void()>(std::forward<Fn>(fn))});
        (void)inserted;  // seq is unique → always inserted
        return CancelToken{it};
    }
};

}  // namespace ripple::test
