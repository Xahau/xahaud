//------------------------------------------------------------------------------
// SteppingExplore — the seed-sweep EXPLORER (explorer-takeoff §4): the harness
// as a scenario GENERATOR. One instance seed derives a full scenario plan
// (protocol-PRNG base, per-node clock offsets, seeded link faults, cadence
// skew, submit schedule, and — schema 2 — an optional one-node stop/restart
// cycle) through ScenarioSeed's derivation contract; the
// driver runs instances against the UNIVERSAL oracles and turns any failure
// into a self-contained one-command reproducer.
//
// MANUAL suite (excluded from --unittest, like SteppingGrind/compression):
//
//   xrpld --unittest=SteppingExplore                    # sweep 25 seeds from the pinned base
//   xrpld --unittest=SteppingExplore --unittest-arg=seeds=N[,start=0x...]
//   xrpld --unittest=SteppingExplore --unittest-arg=seed=0x...[,schema=N]
//                                                       # repro ONE instance via
//                                                       # expectReplays (ladder armed)
//   ...any of the above + ",mode=edge"                  # EDGE mode: loss at/past
//                                                       # absorption on every link;
//                                                       # stalling is legal, forking
//                                                       # never, heal must recover
//
// Layering rule (explorer-api-design §1): NOTHING here touches SteppingNetwork
// surface — the generator is a pure consumer of the existing per-axis knobs
// (seedPrng, clockOffset, faultLink, in, submit, cadence), the way
// expectReplays is a consumer of the whole net.
//
// Oracle discipline (the rung-2b lesson): the sweep asserts only UNIVERSAL
// properties of the absorbed envelope — convergence, no off-thread/failed
// jobs, every submitted payment validated, budget respected. Shape-dependent
// facts (closeAgree, feedback offsets) are LOGGED and folded into the payload
// (so confirm/repro runs must agree on them) but never asserted: a legitimate
// draw may produce a no-consensus-time ledger.
//
// On a sweep failure: print the EXPLORE-REPRO token + full parameter dump,
// then CONFIRM-at-K inline (rerun that instance 3x; identical failure =
// real find, divergent = instrument bug — the §7.11a discrimination), and
// CONTINUE the sweep. A sweep survives bad seeds; the suite still fails.
//------------------------------------------------------------------------------
#include <test/jtx/ScenarioSeed.h>
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpld/core/TimeKeeper.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerHeader.h>

#include <ripple.pb.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace ripple::test {

class SteppingExplore_test : public beast::unit_test::suite
{
    using Payload = std::optional<std::vector<uint256>>;

    static constexpr int kDefaultSeeds = 25;
    static constexpr int kConfirmRuns = 3;
    // Fixed default base: the no-arg sweep is itself reproducible end to end.
    static constexpr std::uint64_t kDefaultBase = 0xFAB1E0005EED0001ull;

    // ── Envelope: the modeled-world bounds, BY CONSTRUCTION ────────────────
    // (explorer-takeoff §4.1.) Every draw below is bounded so an instance
    // stays inside the ABSORBED regime — the oracle contract is full
    // convergence. Edge-of-envelope exploration is a future generator MODE
    // with a different oracle contract (expect liveness cost, assert no
    // fork), not a wider draw here.
    struct Envelope
    {
        std::uint32_t validators = 5;
        // Clock offsets: rung-1 absorbed regime (SteppingClockOffset) —
        // whole seconds inside the 10s genesis close-time bin.
        int maxAbsClockOffsetSecs = 2;
        // Cadence contract: skew*(N-1) < dt (=1s default).
        std::chrono::milliseconds maxCadenceSkew{200};
        int maxFaultedLinks = 2;
        // 10% loss is the grind-proven absorbed rate (SteppingGrind).
        int maxLossPercent = 10;
        // Delay < dt/4 keeps reordering sub-round.
        std::chrono::milliseconds minDelay{50}, maxDelay{250};
        int maxSubmits = 3;
        // Lifecycle axis (schema 2): at most ONE stop/restart cycle per
        // instance — quorum(5)=4 keeps the cohort validating with one
        // node down, so the absorbed-regime oracle contract still holds.
        // The restarted node re-acquires through the real catch-up path
        // (tip jump + history backfill — the 002/004 payoff region).
        bool allowRestart = true;
        // Byzantine axis (schema 3): at most ONE lie-only equivocating
        // validator, honest INTERNALLY, so honest quorum(4 of 5) always
        // holds and the absorbed converge-oracle stays valid.
        bool allowByzantine = true;
    };

    // ── ScenarioPlan: derive → inspect → apply, in that order ──────────────
    // Derivation is a PURE function of (instance seed, envelope); describe()
    // dumps the full parameter set so a failure report is readable — and
    // hand-editable for bisection — without re-deriving anything.
    struct ScenarioPlan
    {
        enum class FaultKind { loss, delay, duplicate };
        struct Fault
        {
            std::uint32_t from = 0, to = 0;
            FaultKind kind = FaultKind::loss;
            int lossPercent = 0;
            std::chrono::milliseconds delay{0};
            std::uint16_t dupType = 0;
            std::uint64_t engineSeed = 0;
        };
        struct Submit
        {
            std::uint32_t node = 0;
            std::chrono::milliseconds at{0};  // after the arm boundary
            int payer = 0;                    // distinct per submit: no seq races
            int amountXrp = 0;
        };
        struct Lifecycle
        {
            bool restart = false;
            std::uint32_t node = 0;
            int stopAfterRounds = 0;  // beats after the arm boundary
            int downRounds = 0;       // beats while stopped (cohort-only)
        };
        struct Byzantine
        {
            // BOUNDED to one lie-only equivocator (schema 3): the node
            // stays honest INTERNALLY (lieValidation never processes
            // locally), so the honest quorum(5)=4 always holds and the
            // absorbed converge-oracle stays valid — the byzantine node
            // itself converges too; its lies are just noise its peers
            // process and discard. This exercises the byzantine
            // validation-processing + phantom-acquire path across seeds
            // without ever threatening liveness.
            bool active = false;
            std::uint32_t node = 0;
            int rounds = 0;  // how many ledgers it equivocates on
        };

        // Generator MODE (takeoff §4.1's envelope discipline): absorbed
        // instances must fully converge under their faults; EDGE instances
        // draw loss at and past the absorption boundary on every link, so
        // losing liveness is a legitimate outcome and the oracle contract
        // changes — never fork, structure intact, heal recovers everything.
        // The mode is part of the repro token (mode=edge), NOT of the seed.
        enum class Mode { absorbed, edge };

        Mode mode = Mode::absorbed;
        std::uint64_t instanceSeed = 0;
        std::uint64_t prngBase = 0;
        std::vector<int> clockOffsetSecs;
        std::chrono::milliseconds cadenceSkew{0};
        std::vector<Fault> faults;
        std::vector<Submit> submits;
        Lifecycle lifecycle;
        Byzantine byzantine;
        // Draws dropped by the directed-link dedupe (codex round 8): the
        // dump stays truthful about how many raw draws the plan absorbed
        // without listing installs that faultLink would have shadowed.
        int shadowedFaults = 0;

        [[nodiscard]] std::string
        describe() const
        {
            std::ostringstream os;
            os << "seed=0x" << std::hex << instanceSeed << std::dec
               << " schema=" << ScenarioSeed::kSchemaVersion
               << (mode == Mode::edge ? " mode=edge" : "") << " prng=0x"
               << std::hex << prngBase << std::dec << " offsets=[";
            for (std::size_t i = 0; i < clockOffsetSecs.size(); ++i)
                os << (i ? "," : "") << clockOffsetSecs[i];
            os << "]s skew=" << cadenceSkew.count() << "ms faults=[";
            for (std::size_t i = 0; i < faults.size(); ++i)
            {
                auto const& f = faults[i];
                os << (i ? "," : "");
                switch (f.kind)
                {
                    case FaultKind::loss:
                        os << "loss(" << f.from << "->" << f.to << ","
                           << f.lossPercent << "%,eng=0x" << std::hex
                           << f.engineSeed << std::dec << ")";
                        break;
                    case FaultKind::delay:
                        os << "delay(" << f.from << "->" << f.to << ","
                           << f.delay.count() << "ms)";
                        break;
                    case FaultKind::duplicate:
                        os << "dup(" << f.from << "->" << f.to << ","
                           << (f.dupType ==
                                       static_cast<std::uint16_t>(
                                           protocol::mtPROPOSE_LEDGER)
                                   ? "mtPROPOSE_LEDGER"
                                   : "mtVALIDATION")
                           << ")";
                        break;
                }
            }
            os << "]";
            if (shadowedFaults)
                os << " shadowed=" << shadowedFaults;
            if (lifecycle.restart)
                os << " restart(n" << lifecycle.node << ",stop@+"
                   << lifecycle.stopAfterRounds << "r,down="
                   << lifecycle.downRounds << "r)";
            if (byzantine.active)
                os << " byzantine(n" << byzantine.node << ","
                   << byzantine.rounds << "r)";
            os << " submits=[";
            for (std::size_t i = 0; i < submits.size(); ++i)
                os << (i ? "," : "") << "p" << submits[i].payer << "@n"
                   << submits[i].node << "+" << submits[i].at.count()
                   << "ms XRP(" << submits[i].amountXrp << ")";
            os << "]";
            return os.str();
        }
    };

    // Draws use engine() % k: integer arithmetic, platform-stable (the
    // dropWithProbability precedent); modulo bias at these ranges is ~1e-17.
    [[nodiscard]] static ScenarioPlan
    derivePlan(
        std::uint64_t instanceSeed,
        Envelope const& env,
        ScenarioPlan::Mode mode = ScenarioPlan::Mode::absorbed)
    {
        using namespace std::chrono;
        ScenarioSeed const s{instanceSeed};
        ScenarioPlan plan;
        plan.mode = mode;
        plan.instanceSeed = instanceSeed;
        plan.prngBase = s.sub("prng");

        // Lifecycle draws FIRST (own stream): the fault and submit blocks
        // below draw node indexes from the SURVIVOR pool, so a generated
        // scenario never faults a link that the restart will destroy and
        // never schedules a submit onto the node that will be down —
        // absorbed-regime discipline by construction, not by rejection.
        // EDGE mode runs restart-free (first cut): heavy network loss and
        // a node outage compose into scenarios whose legitimate outcomes
        // the edge oracles can't yet discriminate from findings.
        {
            beast::xor_shift_engine e(s.sub("lifecycle"));
            if (mode == ScenarioPlan::Mode::absorbed && env.allowRestart &&
                (e() % 2) == 1)
            {
                plan.lifecycle.restart = true;
                plan.lifecycle.node =
                    static_cast<std::uint32_t>(e() % env.validators);
                plan.lifecycle.stopAfterRounds = 1 + static_cast<int>(e() % 2);
                plan.lifecycle.downRounds = 1 + static_cast<int>(e() % 2);
            }
        }
        // Survivor mapping: identity when no restart is planned — which is
        // what keeps schema-2 derivation byte-identical to schema 1 for
        // every no-restart seed.
        auto const pool =
            env.validators - (plan.lifecycle.restart ? 1u : 0u);
        auto const survivor = [&plan](std::uint32_t idx) {
            return plan.lifecycle.restart && idx >= plan.lifecycle.node
                ? idx + 1
                : idx;
        };

        {
            beast::xor_shift_engine e(s.sub("clock-offsets"));
            // EDGE widens the skew into the bin-straddling regime: ±10s is
            // the rung-2-proven spread (close-time vote splits, feedback
            // engages, occasionally sLCF_NoConsensusTime — all LEGAL and
            // already logged + folded into the payload as observations).
            // Absorbed keeps the rung-1 ±2s regime.
            auto const maxAbs = mode == ScenarioPlan::Mode::edge
                ? 10
                : env.maxAbsClockOffsetSecs;
            auto const span = static_cast<std::uint64_t>(2 * maxAbs + 1);
            for (std::uint32_t i = 0; i < env.validators; ++i)
                plan.clockOffsetSecs.push_back(
                    static_cast<int>(e() % span) - maxAbs);
        }

        if (mode == ScenarioPlan::Mode::edge)
        {
            // EDGE faults: seeded loss AT AND PAST the absorption boundary
            // on EVERY directed link (the loss-envelope test's shape,
            // generated). Per-link rates draw from the "faults" stream in
            // link order; per-link engines from the indexed sub-stream, so
            // the arming order can never couple streams. 55% sits just
            // above the grind-proven absorbed regime; 100% is the
            // guaranteed-stall backstop — BOTH outcomes are legal here,
            // which is exactly what makes this a different oracle regime.
            beast::xor_shift_engine e(s.sub("faults"));
            std::uint64_t link = 0;
            for (std::uint32_t from = 0; from < env.validators; ++from)
                for (std::uint32_t to = 0; to < env.validators; ++to)
                {
                    if (from == to)
                        continue;
                    ScenarioPlan::Fault f;
                    f.from = from;
                    f.to = to;
                    f.kind = ScenarioPlan::FaultKind::loss;
                    f.lossPercent = static_cast<int>(55 + e() % 46);
                    f.engineSeed = s.sub("faults-engine", link++);
                    plan.faults.push_back(f);
                }
        }
        else
        {
            beast::xor_shift_engine e(s.sub("faults"));
            auto const n =
                e() % static_cast<std::uint64_t>(env.maxFaultedLinks + 1);
            for (std::uint64_t i = 0; i < n; ++i)
            {
                ScenarioPlan::Fault f;
                // Draw endpoint INDEXES in the survivor pool, then map.
                auto const fromIdx = static_cast<std::uint32_t>(e() % pool);
                auto toIdx = static_cast<std::uint32_t>(e() % (pool - 1));
                if (toIdx >= fromIdx)  // distinct endpoints, still uniform
                    ++toIdx;
                f.from = survivor(fromIdx);
                f.to = survivor(toIdx);
                f.kind = static_cast<ScenarioPlan::FaultKind>(e() % 3);
                f.lossPercent = static_cast<int>(
                    2 + e() % static_cast<std::uint64_t>(
                                  env.maxLossPercent - 2 + 1));
                f.delay = milliseconds{
                    env.minDelay.count() +
                    static_cast<std::int64_t>(
                        e() % static_cast<std::uint64_t>(
                                  env.maxDelay.count() -
                                  env.minDelay.count() + 1))};
                f.dupType = (e() % 2) == 0
                    ? static_cast<std::uint16_t>(protocol::mtPROPOSE_LEDGER)
                    : static_cast<std::uint16_t>(protocol::mtVALIDATION);
                // The loss engine's seed comes from an INDEXED sub-stream —
                // arming order can never couple one fault's stream to
                // another's (or to this shape engine).
                f.engineSeed = s.sub("faults-engine", i);
                plan.faults.push_back(f);
            }

            // faultLink keeps ONE injector per directed link — a later
            // install overwrites the earlier — so a plan listing two
            // faults on one link would lie about the effective scenario
            // (codex round 8). Dedupe AFTER all draws (stream consumption
            // unchanged: every other axis and seed derives identically)
            // keeping the LAST draw per link — the one faultLink would
            // have kept — so effective scenarios, fingerprints, and old
            // repro tokens are all preserved bit-for-bit.
            std::vector<ScenarioPlan::Fault> kept;
            kept.reserve(plan.faults.size());
            for (std::size_t i = 0; i < plan.faults.size(); ++i)
            {
                bool lastForLink = true;
                for (std::size_t j = i + 1; j < plan.faults.size(); ++j)
                    if (plan.faults[j].from == plan.faults[i].from &&
                        plan.faults[j].to == plan.faults[i].to)
                    {
                        lastForLink = false;
                        break;
                    }
                if (lastForLink)
                    kept.push_back(plan.faults[i]);
            }
            plan.shadowedFaults =
                static_cast<int>(plan.faults.size() - kept.size());
            plan.faults = std::move(kept);
        }

        // One "timing" stream, fixed draw order: skew first, then submits.
        {
            beast::xor_shift_engine e(s.sub("timing"));
            plan.cadenceSkew = milliseconds{static_cast<std::int64_t>(
                e() % static_cast<std::uint64_t>(
                          env.maxCadenceSkew.count() + 1))};
            auto const n =
                e() % static_cast<std::uint64_t>(env.maxSubmits + 1);
            for (std::uint64_t i = 0; i < n; ++i)
            {
                ScenarioPlan::Submit sub;
                sub.node =
                    survivor(static_cast<std::uint32_t>(e() % pool));
                sub.at = milliseconds{
                    500 + static_cast<std::int64_t>(e() % 2001)};
                sub.payer = static_cast<int>(i);
                sub.amountXrp = static_cast<int>(100 + e() % 900);
                plan.submits.push_back(sub);
            }
        }

        // Byzantine axis LAST (own stream): a seed drawing "inactive" leaves
        // every prior axis byte-identical to schema 2. A drawn liar picks a
        // SURVIVOR node (never the one a restart takes down — it must be live
        // to lie) and a small number of equivocation rounds. Bounded to one
        // node so honest quorum(4 of 5) is never at risk.
        {
            beast::xor_shift_engine e(s.sub("byzantine"));
            if (env.allowByzantine && (e() % 2) == 1)
            {
                plan.byzantine.active = true;
                plan.byzantine.node =
                    survivor(static_cast<std::uint32_t>(e() % pool));
                plan.byzantine.rounds = 1 + static_cast<int>(e() % 3);
            }
        }
        return plan;
    }

    // ── The instance runner (shared by sweep / confirm / repro) ────────────
    Payload
    runInstance(SteppingNetwork& net, ScenarioPlan const& plan)
    {
        using namespace jtx;
        using namespace std::chrono;

        std::vector<Account> const payers{
            Account{"explorer-a"}, Account{"explorer-b"},
            Account{"explorer-c"}};
        Account const dest{"explorer-dest"};

        // seedPrng BEFORE validators() (its contract); prologue and funding
        // run on the default cadence — the drawn skew shapes the
        // exploration window, not the setup.
        net.seedPrng(plan.prngBase);
        net.validators(
               static_cast<std::size_t>(plan.clockOffsetSecs.size()))
            .mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return Payload{};
        // A byzantine lie about a phantom ledger triggers a REAL acquire
        // (GetConsL2 → InboundLedger → virtual retries) on the peers that
        // receive it — exercised, not modeled, so the closed world must run
        // it as a job. Only byzantine instances pay this; others stay in
        // strict mode, byte-identical to schema 2.
        if (plan.byzantine.active)
            net.canonicalJobs();
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return Payload{};
        net.fund(0, XRP(100000), {payers[0], payers[1], payers[2], dest});

        // The arm boundary: offsets + faults land at one stepping instant.
        for (std::uint32_t i = 0; i < plan.clockOffsetSecs.size(); ++i)
            net.clockOffset(i, seconds{plan.clockOffsetSecs[i]});
        for (auto const& f : plan.faults)
        {
            switch (f.kind)
            {
                //@@start injector-owns-engine
                case ScenarioPlan::FaultKind::loss: {
                    // The injector OWNS its engine (shared_ptr capture):
                    // wires can outlive any scope here — teardown traffic
                    // still routes through the fault — so engine lifetime
                    // must ride the closure, not the stack (the pattern
                    // grindSeededLoss solves with an external accumulator).
                    auto eng = std::make_shared<beast::xor_shift_engine>(
                        f.engineSeed);
                    auto inner = simfaults::dropWithProbability(
                        *eng, f.lossPercent / 100.0);
                    net.faultLink(
                        f.from,
                        f.to,
                        [eng, inner](std::uint16_t t, std::size_t sz) {
                            return inner(t, sz);
                        });
                    break;
                }
                //@@end injector-owns-engine
                case ScenarioPlan::FaultKind::delay: {
                    // Per-MESSAGE seeded jitter in [1ms, f.delay] (the drawn
                    // value is the max), so a generated delay fault models
                    // variable packet times, not one constant offset — the
                    // delay analog of the loss fault's dropWithProbability.
                    // The per-link engine (own indexed seed) is the replay
                    // handle; same shared_ptr ownership as the loss case.
                    auto eng = std::make_shared<beast::xor_shift_engine>(
                        f.engineSeed);
                    auto inner = simfaults::delayJittered(
                        *eng, std::chrono::milliseconds{1}, f.delay);
                    net.faultLink(
                        f.from,
                        f.to,
                        [eng, inner](std::uint16_t t, std::size_t sz) {
                            return inner(t, sz);
                        });
                    break;
                }
                case ScenarioPlan::FaultKind::duplicate:
                    net.faultLink(
                        f.from, f.to, simfaults::duplicateType(f.dupType, 1));
                    break;
            }
        }

        // The submit schedule rides the faulted, skewed network. Distinct
        // payers per submit: independent sequences, no autofill races.
        for (auto const& sub : plan.submits)
        {
            auto const payer = payers[static_cast<std::size_t>(sub.payer)];
            auto const amount = sub.amountXrp;
            net.in(sub.at, sub.node, [&net, sub, payer, dest, amount]() {
                net.submit(
                    sub.node, jtx::pay(payer, dest, XRP(amount)), payer);
            });
        }

        SteppingNetwork::Cadence const cadence{seconds{1}, plan.cadenceSkew};

        // ── Lifecycle phase (schema 2): the drawn stop/restart cycle ──
        // The canonical choreography (SteppingCombined): isolate + flush,
        // stop, keep the quorum(5)=4 cohort moving, restart from the same
        // slot (disk persists, memory clears), reconnect fresh wires. The
        // faults/submits never touch this node by construction (survivor
        // pool), so the fault epoch survives the restart untouched.
        if (plan.lifecycle.restart)
        {
            for (int r = 0; r < plan.lifecycle.stopAfterRounds; ++r)
                net.tick(seconds{1}, plan.cadenceSkew);
            net.isolateNodeAndFlush(plan.lifecycle.node);
            net.stopNode(plan.lifecycle.node);
            for (int r = 0; r < plan.lifecycle.downRounds; ++r)
                net.tick(seconds{1}, plan.cadenceSkew);
            net.restartNode(plan.lifecycle.node);
            if (!BEAST_EXPECT(net.isLive(plan.lifecycle.node)))
                return Payload{};
            net.reconnectNode(plan.lifecycle.node);
        }

        // ── Edge chaos window (mode=edge only) ──
        // Five beats under at-and-past-absorption loss. Liveness is NOT
        // asserted (stalling is a legal outcome — record it); safety and
        // structure are asserted CONTINUOUSLY: no validated fork, no
        // off-thread work, no failed jobs. Then heal every link and fall
        // through to the shared recovery/convergence path below — after a
        // heal the absorbed-mode oracles apply in full.
        std::uint32_t chaosProgress = 0;
        if (plan.mode == ScenarioPlan::Mode::edge)
        {
            auto const before = net.minValidatedSeq();
            for (int r = 0; r < 5; ++r)
            {
                net.tick(seconds{1}, plan.cadenceSkew);
                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return Payload{};
            }
            chaosProgress = net.minValidatedSeq() - before;
            BEAST_EXPECT(net.offThreadJobs() == 0);
            BEAST_EXPECT(net.failedJobs() == 0);
            for (auto const& f : plan.faults)
                net.faultLink(f.from, f.to, {});
        }
        auto const edgeMode = plan.mode == ScenarioPlan::Mode::edge;

        // ── Byzantine phase (schema 3): the drawn equivocator ──
        // A lie-only validator: at each of `rounds` ledgers in the window it
        // broadcasts a fabricated validation for the NEXT seq (a phantom
        // ledger no node can serve) alongside its honest vote. Honest
        // INTERNALLY — lieValidation never processes locally — so honest
        // quorum(4 of 5) holds and everyone, the liar included, still
        // converges on the real chain; the lies are noise peers process and
        // discard. The fabricated hash is a deterministic function of (node,
        // seq) so the byzantine run replays bit-for-bit like any other.
        if (plan.byzantine.active)
        {
            auto const bz = plan.byzantine.node;
            for (int r = 0; r < plan.byzantine.rounds; ++r)
                net.in(
                    milliseconds{300 + r * 1000}, bz, [&net, bz]() {
                        auto const seq = net.minValidatedSeq() + 1;
                        uint256 fake;
                        fake.data()[0] = 0xBA;
                        fake.data()[1] = 0xD5;
                        fake.data()[30] = static_cast<std::uint8_t>(bz);
                        fake.data()[31] = static_cast<std::uint8_t>(seq);
                        net.lieValidation(bz, seq, fake);
                    });
        }

        // Target measured AFTER the lifecycle phase: +5 from wherever the
        // network now stands, floored by the restarted node's catch-up.
        auto const target = net.minValidatedSeq() + 5;
        net.runTo(target, SteppingNetwork::RunBudget{}, cadence);
        // Serving is asynchronous after a catch-up (the retry-cadence
        // lesson): a restarted node jumps to the tip and backfills the gap
        // BEHIND it, so byte-agreement at target is POLLED, not sampled.
        // Zero extra beats for no-restart seeds (already agreeing).
        if (!BEAST_EXPECT(net.runUntil(
                [&net, target]() { return net.ledgersAgree(target); },
                SteppingNetwork::RunBudget{/*heartbeats=*/10},
                cadence)))
            return Payload{};

        // ── Universal oracles (envelope properties — always asserted) ──
        if (!net.expectConverged(target))
            return Payload{};
        // Post-heal fork-free symmetry (codex round 13): the chaos window
        // asserted it per beat; assert it once more over the RECOVERED
        // history so the heal path itself can never smuggle a fork in.
        if (edgeMode && !BEAST_EXPECT(net.validatedForkFree()))
            return Payload{};
        // Every submitted payment validated: dest received exactly the
        // planned sum (receivers pay no fees, nothing else touches dest).
        std::uint64_t expectedDrops = 100000ull * 1'000'000ull;
        for (auto const& sub : plan.submits)
            expectedDrops +=
                static_cast<std::uint64_t>(sub.amountXrp) * 1'000'000ull;
        auto const l = net.ledger(0, target);
        if (!BEAST_EXPECT(l != nullptr))
            return Payload{};
        auto const sle = l->read(keylet::account(dest.id()));
        if (!BEAST_EXPECT(sle != nullptr))
            return Payload{};
        auto const destDrops = static_cast<std::uint64_t>(
            sle->getFieldAmount(sfBalance).xrp().drops());
        if (!BEAST_EXPECT(destDrops == expectedDrops))
            return Payload{};

        // ── Shape-dependent observations (logged + folded, NOT asserted) ──
        bool anyNoConsensusTime = false;
        for (std::uint32_t seq = 4; seq <= target; ++seq)
        {
            auto const lg = net.ledger(0, seq);
            if (!BEAST_EXPECT(lg != nullptr))
                return Payload{};
            if (!getCloseAgree(lg->info()))
                anyNoConsensusTime = true;
        }

        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            chain.push_back(net.ledgerHash(0, seq));
        chain.push_back(uint256{anyNoConsensusTime ? 2u : 1u});
        chain.push_back(uint256{destDrops});
        for (std::uint32_t i = 0; i < plan.clockOffsetSecs.size(); ++i)
            chain.push_back(uint256{static_cast<std::uint64_t>(
                1000 +
                net.node(i).app().timeKeeper().closeOffset().count())});
        // Edge observability, folded LAST so the sweep can read it back as
        // payload->back(): how much validated progress survived the chaos
        // window (5000 = fully stalled). Logged and FOLDED, never asserted
        // — both regimes are legal draws; a replay that shifts the number
        // is a divergence.
        if (plan.mode == ScenarioPlan::Mode::edge)
        {
            log << "  edge: validated progress under chaos = "
                << chaosProgress << std::endl;
            chain.push_back(uint256{5000u + chaosProgress});
        }
        return Payload{std::move(chain)};
    }

    // ── Modes ───────────────────────────────────────────────────────────────

    // Pin the derivation itself (canary-style: update the literal WITH
    // INTENT alongside a kSchemaVersion bump). Runs in every mode — a
    // repro session discovers mixer drift before trusting a token.
    void
    testDerivationPinned()
    {
        testcase("explore: derivation schema pin");
        auto const observed = ScenarioSeed{kDefaultBase}.sub("instance", 0);
        log << "  schema=" << ScenarioSeed::kSchemaVersion
            << " pin observed: 0x" << std::hex << observed << std::dec
            << std::endl;
        BEAST_EXPECT(observed == 0x08DECA8F7424331Full);
    }

    // The dedupe invariant, derivation-only (no networks — cheap enough
    // to run in every mode): no plan may list two faults on one directed
    // link, and the scan must actually WITNESS the dedupe firing (a
    // non-vacuity gate in the rung-2b tradition: if a draw-range change
    // makes duplicates impossible, this stops proving anything — fail it).
    void
    testDerivationLinkUnique()
    {
        testcase(
            "explore: derived plans keep their invariants (unique links, "
            "survivor-pool discipline)");
        Envelope const env{};
        ScenarioSeed const master{kDefaultBase};
        int shadowedTotal = 0;
        int restarts = 0;
        int byzantines = 0;
        constexpr int kScan = 512;
        for (int i = 0; i < kScan; ++i)
        {
            auto const plan = derivePlan(
                master.sub("instance", static_cast<std::uint64_t>(i)), env);
            byzantines += plan.byzantine.active ? 1 : 0;
            std::set<std::pair<std::uint32_t, std::uint32_t>> links;
            for (auto const& f : plan.faults)
                if (!links.insert({f.from, f.to}).second)
                {
                    fail(
                        "duplicate directed link survived dedupe: " +
                        plan.describe());
                    return;
                }
            if (plan.lifecycle.restart)
            {
                ++restarts;
                // Survivor-pool discipline: nothing in the plan may touch
                // the node the restart takes down — a fault on a wire the
                // isolate destroys or a submit scheduled onto a dead node
                // would silently narrow (or crash) the scenario.
                for (auto const& f : plan.faults)
                    if (f.from == plan.lifecycle.node ||
                        f.to == plan.lifecycle.node)
                    {
                        fail(
                            "fault touches the restart node: " +
                            plan.describe());
                        return;
                    }
                for (auto const& sub : plan.submits)
                    if (sub.node == plan.lifecycle.node)
                    {
                        fail(
                            "submit targets the restart node: " +
                            plan.describe());
                        return;
                    }
            }
            // The byzantine equivocator must never be the node a restart
            // takes down — it has to be live to lie (survivor-pool draw).
            if (plan.byzantine.active && plan.lifecycle.restart)
                if (plan.byzantine.node == plan.lifecycle.node)
                {
                    fail(
                        "byzantine node is the restart node: " +
                        plan.describe());
                    return;
                }
            shadowedTotal += plan.shadowedFaults;
        }
        pass();  // the scan itself
        log << "  " << kScan << " plans scanned, " << shadowedTotal
            << " shadowed draws deduped, " << restarts << " restarts, "
            << byzantines << " byzantine drawn" << std::endl;
        expect(
            shadowedTotal > 0,
            "dedupe never fired across the scan — non-vacuity lost");
        // Lifecycle non-vacuity: ~half the scan should draw a restart; zero
        // means the axis silently died (a draw-range or envelope change).
        expect(
            restarts > 0,
            "no restart drawn across the scan — lifecycle axis vacuous");
        expect(
            byzantines > 0,
            "no byzantine drawn across the scan — byzantine axis vacuous");
    }

    void
    sweep(
        std::uint64_t base,
        int count,
        ScenarioPlan::Mode mode = ScenarioPlan::Mode::absorbed)
    {
        testcase(
            mode == ScenarioPlan::Mode::edge ? "explore: seed sweep (edge)"
                                             : "explore: seed sweep");
        log << "  sweep: " << count << " instances from master 0x" << std::hex
            << base << std::dec << " (schema "
            << ScenarioSeed::kSchemaVersion
            << (mode == ScenarioPlan::Mode::edge ? ", mode=edge" : "") << ")"
            << std::endl;
        ScenarioSeed const master{base};
        Envelope const env{};
        int failures = 0;
        int restarts = 0;
        int byzantines = 0;
        int stalls = 0;
        std::set<std::string> seenLabels;
        for (int i = 0; i < count; ++i)
        {
            auto const instanceSeed =
                master.sub("instance", static_cast<std::uint64_t>(i));
            auto const plan = derivePlan(instanceSeed, env, mode);
            restarts += plan.lifecycle.restart ? 1 : 0;
            byzantines += plan.byzantine.active ? 1 : 0;
            log << "  [" << i << "] " << plan.describe() << std::endl;

            SteppingNetwork net(*this);
            net.recordForensics();
            auto const payload = runInstance(net, plan);

            // Novelty census (take-off §4.3's cheap proxy): an instance
            // that executes a never-before-seen labeled event is
            // interesting by definition. Log it, don't build around it.
            std::vector<std::string> novel;
            for (auto const& ev : net.controller().scheduler().traceLog())
                if (!ev.label.empty() && seenLabels.insert(ev.label).second)
                    novel.push_back(ev.label);
            if (i == 0)
                log << "    label baseline: " << seenLabels.size()
                    << " distinct" << std::endl;
            else if (!novel.empty())
            {
                log << "    NOVEL labels:";
                for (auto const& n : novel)
                    log << " '" << n << "'";
                log << std::endl;
            }

            if (payload)
            {
                if (plan.mode == ScenarioPlan::Mode::edge &&
                    !payload->empty() && payload->back() == uint256{5000u})
                    ++stalls;
                log << "    ok fingerprint=0x" << std::hex
                    << net.traceFingerprint() << std::dec << " ("
                    << net.traceCount() << " events)" << std::endl;
                continue;
            }
            //@@start repro-token
            ++failures;
            char const* const modeArg =
                plan.mode == ScenarioPlan::Mode::edge ? ",mode=edge" : "";
            log << "  EXPLORE-REPRO schema=" << ScenarioSeed::kSchemaVersion
                << " seed=0x" << std::hex << instanceSeed << std::dec
                << modeArg << std::endl;
            log << "    repro: --unittest=SteppingExplore --unittest-arg="
                << "seed=0x" << std::hex << instanceSeed << ",schema="
                << std::dec << ScenarioSeed::kSchemaVersion << modeArg
                << std::endl;
            confirmFailure(plan);
            //@@end repro-token
        }
        log << "  sweep done: " << count << " instances (" << restarts
            << " with a restart, " << byzantines << " byzantine"
            << (mode == ScenarioPlan::Mode::edge
                    ? ", " + std::to_string(stalls) + " fully stalled"
                    : std::string{})
            << "), " << failures << " failing" << std::endl;
        expect(
            failures == 0,
            "explore sweep: failing seeds above (EXPLORE-REPRO tokens)");
        // Edge-mode non-vacuity (the rung-2b discipline, sweep-level): a
        // sweep big enough to be representative must have WITNESSED a full
        // stall — an edge envelope that always converges has silently
        // demoted to the absorbed regime. Small/targeted sweeps skip the
        // gate (a 2-seed repro session can't demand a stall).
        if (mode == ScenarioPlan::Mode::edge && count >= 10)
            expect(
                stalls > 0,
                "edge sweep: no instance fully stalled — envelope no "
                "longer reaches past absorption");
    }

    // Confirm-at-K: a reported failure must itself replay identically —
    // same fingerprint, still failing — before it's a FIND; divergence
    // means the INSTRUMENT is broken (worse, and differently urgent).
    void
    confirmFailure(ScenarioPlan const& plan)
    {
        std::optional<std::uint64_t> firstPrint;
        bool stillFails = true, printsAgree = true;
        for (int k = 0; k < kConfirmRuns; ++k)
        {
            SteppingNetwork net(*this);
            net.recordForensics();
            auto const payload = runInstance(net, plan);
            stillFails = stillFails && !payload.has_value();
            if (!firstPrint)
                firstPrint = net.traceFingerprint();
            else
                printsAgree =
                    printsAgree && (*firstPrint == net.traceFingerprint());
        }
        if (stillFails && printsAgree)
            log << "    CONFIRMED: fails identically x" << kConfirmRuns
                << " (fingerprint 0x" << std::hex << *firstPrint << std::dec
                << ") — real find" << std::endl;
        else
            log << "    INSTRUMENT SUSPECT: "
                << (stillFails ? "fingerprints diverged across reruns"
                               : "failure did not reproduce")
                << " — harness bug, not a find" << std::endl;
    }

    void
    reproInstance(
        std::uint64_t instanceSeed,
        ScenarioPlan::Mode mode = ScenarioPlan::Mode::absorbed)
    {
        testcase("explore: repro single instance");
        Envelope const env{};
        auto const plan = derivePlan(instanceSeed, env, mode);
        log << "  " << plan.describe() << std::endl;
        expectReplays(
            *this, "explore repro", [this, &plan](SteppingNetwork& net) {
                return runInstance(net, plan);
            });
    }

    // ── Arg parsing (the replayArg pattern: key=value out of s.arg()) ──────
    [[nodiscard]] static std::optional<std::uint64_t>
    hexArg(std::string const& a, std::string const& key)
    {
        auto const pos = a.find(key);
        if (pos == std::string::npos)
            return std::nullopt;
        auto v = a.substr(pos + key.size());
        if (v.starts_with("0x") || v.starts_with("0X"))
            v = v.substr(2);
        std::uint64_t n = 0;
        bool any = false;
        for (char const c : v)
        {
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = 10 + (c - 'a');
            else if (c >= 'A' && c <= 'F')
                d = 10 + (c - 'A');
            else
                break;
            n = (n << 4) | static_cast<std::uint64_t>(d);
            any = true;
        }
        return any ? std::optional{n} : std::nullopt;
    }

    [[nodiscard]] static std::optional<int>
    decArg(std::string const& a, std::string const& key)
    {
        auto const pos = a.find(key);
        if (pos == std::string::npos)
            return std::nullopt;
        int n = 0;
        bool any = false;
        for (char const c : a.substr(pos + key.size()))
        {
            if (c < '0' || c > '9')
                break;
            n = n * 10 + (c - '0');
            any = true;
        }
        return any ? std::optional{n} : std::nullopt;
    }

public:
    void
    run() override
    {
        testDerivationPinned();
        testDerivationLinkUnique();
        auto const& a = arg();
        if (auto const schema = decArg(a, "schema="))
        {
            // A token minted under another schema derives a DIFFERENT
            // scenario — refuse loudly rather than reproduce the wrong one.
            if (!expect(
                    static_cast<std::uint64_t>(*schema) ==
                        ScenarioSeed::kSchemaVersion,
                    "repro token schema mismatch: token=" +
                        std::to_string(*schema) + " current=" +
                        std::to_string(ScenarioSeed::kSchemaVersion)))
                return;
        }
        auto const mode = a.find("mode=edge") != std::string::npos
            ? ScenarioPlan::Mode::edge
            : ScenarioPlan::Mode::absorbed;
        if (auto const seed = hexArg(a, "seed="))
        {
            reproInstance(*seed, mode);
            return;
        }
        sweep(
            hexArg(a, "start=").value_or(kDefaultBase),
            decArg(a, "seeds=").value_or(kDefaultSeeds),
            mode);
    }
};

BEAST_DEFINE_TESTSUITE_MANUAL(SteppingExplore, consensus, ripple);

}  // namespace ripple::test
