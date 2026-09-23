#pragma once
//------------------------------------------------------------------------------
// SteppingReplay — the determinism self-oracle as a first-class combinator
// (plan §5.8), extracted from the suite-local scaffolding that found the
// first-run nodestore oracle (design-notes §7).
//
// expectReplays(suite, label, scenario) runs ONE scenario K times in the
// same process and compares the scenario's payload (e.g. a hash chain), event
// count, and coarse (time, tier, node, kind) fingerprint with the first run.
// Distinct closures can share that tuple; semantic payload checks remain
// necessary, and equality is not proof of identical event-by-event execution.
// There is deliberately NO persisted golden (nothing to bless, nothing to
// rot): the expectation is regenerated live every run. On mismatch it
// prints the debugging LADDER instead of a bare failure: first divergent
// event (with job labels), then the first send-flow/target divergence and
// validation-lifecycle divergence.
//
// WHY K runs in one process: consecutive in-process runs are the
// cross-run-leak detector. Run 1 vs run 2 is cold-vs-warm — exactly what
// exposed the MemoryFactory static-nodestore oracle; run 2 vs run 3
// distinguishes first-run-only leakage from progressive drift. Default
// K=3; grind deeper with
//     --unittest-arg=replays=N
// (the larger of the suite's default and the arg wins).
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ripple::test {

// Everything captured from one run of a scenario, for cross-run comparison
// and (on mismatch) ladder diffs.
struct ReplaySnapshot
{
    std::vector<uint256> payload;  // the scenario's comparable outcome
    std::uint64_t fingerprint = 0;
    std::uint64_t traceCount = 0;
    std::vector<HarnessScheduler::TraceEvent> traceLog;
    SteppingNetwork::Forensics forensics;
    std::string diagnostics;
};

// Parse `replays=N` (or a bare integer) out of --unittest-arg. Zero if absent.
[[nodiscard]] inline int
replayArg(beast::unit_test::suite& s)
{
    auto const& a = s.arg();
    std::string v;
    if (auto const pos = a.find("replays="); pos != std::string::npos)
        v = a.substr(pos + 8);
    else if (
        !a.empty() && a.find_first_not_of("0123456789") == std::string::npos)
        v = a;
    int n = 0;
    for (char const c : v)
    {
        if (c < '0' || c > '9')
            break;
        n = n * 10 + (c - '0');
    }
    return n;
}

namespace detail {

inline void
logFirstTraceDivergence(
    beast::unit_test::suite& s,
    std::vector<HarnessScheduler::TraceEvent> const& a,
    std::vector<HarnessScheduler::TraceEvent> const& b)
{
    auto const n = std::min(a.size(), b.size());
    std::size_t i = 0;
    while (i < n && a[i] == b[i])
        ++i;
    s.log << "  trace sizes: run1=" << a.size() << " runK=" << b.size()
          << ", first divergence at index " << i << std::endl;
    auto const dump = [&s](
                          char const* label,
                          std::vector<HarnessScheduler::TraceEvent> const& t,
                          std::size_t at) {
        auto const lo = at >= 6 ? at - 6 : 0;
        for (auto j = lo; j < std::min(t.size(), at + 10); ++j)
            s.log << "    " << label << "[" << j << "] when=" << t[j].when
                  << " tier=" << HarnessScheduler::tierName(t[j].tier) << "("
                  << t[j].tier << ") node=" << t[j].nodeId
                  << " kind=" << HarnessScheduler::kindName(t[j].kind)
                  << (t[j].label.empty() ? "" : " '" + t[j].label + "'")
                  << (j == at ? "   <-- FIRST DIVERGENCE" : "") << std::endl;
    };
    dump("run1", a, i);
    dump("runK", b, i);
}

// Diff send logs with run-varying remote ports normalized by first-seen
// order (connect order is deterministic, so "k-th distinct port" names the
// same wire in both runs).
inline void
logFirstSendDivergence(
    beast::unit_test::suite& s,
    SteppingNetwork::Forensics const& a,
    SteppingNetwork::Forensics const& b)
{
    auto const normalize = [](SteppingNetwork::Forensics const& f) {
        std::map<std::uint16_t, int> seen;
        std::vector<std::string> out;
        out.reserve(f.sendLog.size());
        for (auto const& [when, sender, name, port] : f.sendLog)
        {
            auto const [it, inserted] =
                seen.emplace(port, static_cast<int>(seen.size()));
            out.push_back(
                "when=" + std::to_string(when) + " n" + std::to_string(sender) +
                " '" + name + "' -> w" + std::to_string(it->second));
        }
        return out;
    };
    auto const na = normalize(a);
    auto const nb = normalize(b);
    auto const n = std::min(na.size(), nb.size());
    std::size_t i = 0;
    while (i < n && na[i] == nb[i])
        ++i;
    if (i == na.size() && i == nb.size())
    {
        s.log << "  send logs identical" << std::endl;
        return;
    }
    s.log << "  send logs: run1=" << na.size() << " runK=" << nb.size()
          << ", first divergence at " << i << std::endl;
    auto const lo = i >= 3 ? i - 3 : 0;
    for (auto j = lo; j < std::min(na.size(), i + 6); ++j)
        s.log << "    run1[" << j << "] " << na[j] << std::endl;
    for (auto j = lo; j < std::min(nb.size(), i + 6); ++j)
        s.log << "    runK[" << j << "] " << nb[j] << std::endl;
}

inline void
logFirstValDivergence(
    beast::unit_test::suite& s,
    SteppingNetwork::Forensics const& a,
    SteppingNetwork::Forensics const& b)
{
    auto const n = std::min(a.valEvents.size(), b.valEvents.size());
    std::size_t i = 0;
    while (i < n && a.valEvents[i] == b.valEvents[i])
        ++i;
    if (i == a.valEvents.size() && i == b.valEvents.size())
        return;
    s.log << "  val-event streams: run1=" << a.valEvents.size()
          << " runK=" << b.valEvents.size() << ", first divergence at " << i
          << std::endl;
    auto const lo = i >= 3 ? i - 3 : 0;
    for (auto j = lo; j < std::min(a.valEvents.size(), i + 6); ++j)
        s.log << "    run1[" << j << "] " << a.valEvents[j] << std::endl;
    for (auto j = lo; j < std::min(b.valEvents.size(), i + 6); ++j)
        s.log << "    runK[" << j << "] " << b.valEvents[j] << std::endl;
}

}  // namespace detail

inline std::function<void(SteppingNetwork&, std::uint32_t)>&
steppingBusyProbe()
{
    static std::function<void(SteppingNetwork&, std::uint32_t)> probe;
    return probe;
}

// Run `scenario` K times (K = max(minRuns, --unittest-arg replays=N)) and
// assert every run reproduces run 1 exactly — payload and executed-order
// fingerprint. The scenario receives a fresh SteppingNetwork with forensics
// already recording; it builds the topology, drives, keeps its own internal
// BEAST_EXPECTs, and returns its comparable payload (nullopt aborts the
// grind — the scenario itself failed). Returns true if all runs matched.
inline bool
expectReplays(
    beast::unit_test::suite& s,
    char const* label,
    std::function<std::optional<std::vector<uint256>>(SteppingNetwork&)> const&
        scenario,
    int minRuns = 3)
{
    auto const runs = std::max(minRuns, replayArg(s));
    std::optional<ReplaySnapshot> first;
    bool ok = true;
    for (int k = 0; k < runs; ++k)
    {
        SteppingNetwork net(s);
        net.recordForensics();
        if (steppingBusyProbe())
        {
            net.controller().setAlwaysBeforeJob(
                [&net](std::uint32_t id, JobType, std::string const&) {
                    steppingBusyProbe()(net, id);
                });
        }
        auto const payload = scenario(net);
        if (!s.expect(
                payload.has_value(),
                std::string(label) + ": scenario failed on run " +
                    std::to_string(k + 1)))
            return false;

        ReplaySnapshot snap;
        snap.payload = *payload;
        snap.fingerprint = net.traceFingerprint();
        snap.traceCount = net.traceCount();
        snap.traceLog = net.controller().scheduler().traceLog();
        snap.forensics = net.forensics();
        snap.diagnostics = net.jobDiagnostics();

        if (!first)
        {
            first = std::move(snap);
            continue;
        }
        //@@start self-oracle-core
        bool const payloadEq = snap.payload == first->payload;
        bool const traceEq = snap.fingerprint == first->fingerprint;
        bool const traceCountEq = snap.traceCount == first->traceCount;
        s.expect(
            payloadEq,
            std::string(label) + ": payload diverged on run " +
                std::to_string(k + 1));
        s.expect(
            traceEq,
            std::string(label) + ": trace fingerprint diverged on run " +
                std::to_string(k + 1));
        s.expect(
            traceCountEq,
            std::string(label) + ": trace count diverged on run " +
                std::to_string(k + 1));
        if (!payloadEq || !traceEq || !traceCountEq)
        {
            ok = false;
            s.log << "  " << label << ": run " << (k + 1)
                  << " diverged from run 1 — ladder:" << std::endl;
            detail::logFirstTraceDivergence(s, first->traceLog, snap.traceLog);
            detail::logFirstSendDivergence(s, first->forensics, snap.forensics);
            detail::logFirstValDivergence(s, first->forensics, snap.forensics);
        }
        //@@end self-oracle-core
    }
    if (ok)
        s.log << "  " << label << ": " << runs
              << " runs identical (fingerprint 0x" << std::hex
              << first->fingerprint << std::dec << ", " << first->traceCount
              << " events)" << std::endl;
    return ok;
}

}  // namespace ripple::test
