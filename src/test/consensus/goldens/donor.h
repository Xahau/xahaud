//------------------------------------------------------------------------------
/*
    Per-lineage golden manifest for the donor stepping suites.

    lineage:            donor / #814
    seed base:          0xFAB1E5EED0000000 (harness default)
    traffic seed:       0x5452414646494331
    topology:           slow-minority and dispute are 5 validators, mesh.
                        The hub pin is 5 validators, each linked only to one
                        non-validator hub. Traffic is 3 validators, mesh.
                        Trust fork cells are N=10 with the overlap in the row.
    pacing:             profiled dispute K in {0,1,2,3,4}, unitCost 5ms,
                        heartbeat budget 160. This lineage has one K=3 row;
                        it resolves. There is no second K=3 seed.
    RNG mapping:        c527e0a721 (engine-range randomU64 plus the zero-bit
                        group fix)
    captured at:        c527e0a721. These are the pins recaptured at
                        58ff1777fb after the portable rand_int mapping
                        and unchanged since; extracted from the test
                        bodies on top of f5963e38d2.
    platforms:          macOS libc++ holds these literals.

    Outcome-bearing (the assertion is the whole row, not only the hash):
      kProfiledDispute, kProfiledFork.
    Content-bearing (ledger hashes plus transaction ids and sequences,
      not the scheduler event order):
      kTrafficPayloadFingerprint.
    Pure event-order pins (a hash or a count, no outcome table):
      kSlowMinorityFingerprint, kHubNetworkFingerprint,
      kDisputeFingerprint, kTrafficFingerprint, kTrafficEvents.

    Regeneration: --unittest-arg=goldens=print with one of
    SteppingCsf, SteppingTrust, or SteppingTraffic. Each prints GOLDEN
    lines from the observed values. Review that diff; do not treat it as
    automatic. A pinned 2000 is the highest virtual time one beat
    consumed, not a fixed per-beat budget.
*/
//==============================================================================

#ifndef XRPL_TEST_CONSENSUS_GOLDENS_DONOR_H_INCLUDED
#define XRPL_TEST_CONSENSUS_GOLDENS_DONOR_H_INCLUDED

#include <xrpl/beast/unit_test/suite.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <string>

namespace ripple::test::goldens::donor {

inline constexpr std::uint64_t kSlowMinorityFingerprint = 0x4aea2f46ca499175ull;
inline constexpr std::uint64_t kHubNetworkFingerprint = 0x75653559986b05c9ull;
inline constexpr std::uint64_t kDisputeFingerprint = 0x03773da2e07e273cull;
inline constexpr std::uint64_t kTrafficSeed = 0x5452414646494331ull;
inline constexpr std::uint64_t kTrafficFingerprint = 0x4b1c6cb7b9297b94ull;
inline constexpr std::uint64_t kTrafficEvents = 1413;
inline constexpr std::uint64_t kTrafficPayloadFingerprint =
    0xd77bfa4d445420e3ull;

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
    // Highest virtual time one beat consumed. A pinned 2000 is that
    // observed maximum, not a fixed per-beat budget.
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
        return k == o.k && fingerprint == o.fingerprint && events == o.events &&
            weightedEvents == o.weightedEvents && steps == o.steps &&
            beats == o.beats && minValidated == o.minValidated &&
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

inline constexpr std::array<KProfiledDisputeSample, 5> kProfiledDispute = {{
    {0,    0xc7b9f47f247a148aull,
     2606, 0,
     1132, 0,
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
    {1,     0x8a2443e7d07ed17bull,
     2630,  2567,
     1156,  15,
     11,    11,
     10,    11,
     8,     8,
     0,     9,
     12835, 12760,
     1865,  66820,
     75,    717,
     332,   30,
     2,     true,
     true,  true,
     true,  true,
     true,  true},
    {2,     0x182d5979de1d171full,
     2708,  2844,
     1234,  27,
     11,    11,
     10,    11,
     9,     9,
     0,     26,
     28440, 27960,
     2000,  78980,
     135,   542,
     513,   42,
     2,     true,
     true,  true,
     true,  true,
     true,  true},
    {3,      0xcfc0f9405b29cef7ull,
     4661,   7645,
     3187,   112,
     11,     12,
     11,     11,
     10,     0,
     10,     111,
     114675, 112885,
     2000,   163905,
     545,    722,
     1818,   100,
     3,      true,
     true,   true,
     true,   true,
     true,   false},
    {4,      0x668228fd72846e90ull,
     4980,   8331,
     3506,   160,
     8,      8,
     7,      11,
     0,      0,
     0,      160,
     166620, 161000,
     2000,   212020,
     725,    603,
     2046,   130,
     2,      true,
     true,   true,
     false,  false,
     false,  false},
}};

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

inline constexpr std::array<KProfiledForkCell, 9> kProfiledFork = {{
    {0,     0,     0xc37088e4d4e3963bull,
     3439,  0,     2020,
     0,     8,     8,
     7,     8,     5,
     0,     5,     5,
     0,     0,     0,
     0,     54020, 0,
     0,     0,     0,
     0,     true,  true,
     false, true,  false,
     false, false, 5},
    {0,     1,     0xe24a6cdd8143ee3bull,
     3435,  4492,  2016,
     12,    8,     8,
     7,     8,     5,
     0,     5,     5,
     0,     4492,  4492,
     787,   54345, 120,
     1312,  582,   0,
     0,     true,  true,
     false, true,  false,
     false, false, 1},
    {0,     1,     0xc364543a2b0ed316ull,
     3809,  5335,  2390,
     26,    8,     8,
     7,     8,     6,
     0,     6,     6,
     25,    26675, 26455,
     2000,  68465, 250,
     1279,  807,   52,
     2,     true,  true,
     false, true,  false,
     false, true,  5},
    {4,     0,     0xdd2d1949c1e5e96full,
     6104,  0,     4178,
     0,     8,     8,
     7,     8,     0,
     5,     5,     0,
     0,     0,     0,
     0,     55020, 0,
     0,     0,     0,
     0,     true,  true,
     true,  false, true,
     false, false, 5},
    {4,     1,     0x272a08d874ce16f9ull,
     6191,  9605,  4265,
     14,    8,     8,
     7,     8,     0,
     5,     5,     0,
     4,     9605,  9598,
     1640,  56548, 140,
     2884,  1217,  22,
     3,     true,  true,
     true,  false, true,
     false, true,  1},
    {4,     1,      0xcd8cee7af2732ffcull,
     14875, 32503,  12949,
     160,   5,      5,
     4,     8,      0,
     0,     0,      0,
     160,   162515, 161000,
     2000,  203010, 1520,
     2855,  8127,   445,
     3,     true,   true,
     true,  false,  false,
     true,  true,   5},
    {6,     0,     0x7bb058844fa2eae9ull,
     6098,  0,     4068,
     0,     8,     8,
     7,     8,     0,
     5,     5,     0,
     0,     0,     0,
     0,     54020, 0,
     0,     0,     0,
     0,     true,  true,
     true,  false, true,
     false, false, 5},
    {6,     1,     0x4d2f69d8884dca4dull,
     6212,  9314,  4182,
     13,    8,     8,
     7,     8,     0,
     5,     5,     0,
     7,     9314,  9299,
     1632,  55612, 130,
     2952,  1082,  16,
     3,     true,  true,
     true,  false, true,
     false, true,  1},
    {6,     1,      0x525af9afc81f4861ull,
     15382, 32495,  13352,
     160,   5,      12,
     11,    8,      0,
     0,     6,      0,
     160,   162475, 161000,
     2000,  203010, 1550,
     4008,  7343,   449,
     2,     true,   true,
     true,  false,  false,
     true,  true,   5},
}};

[[nodiscard]] inline bool
goldensPrint(beast::unit_test::suite const& s)
{
    return s.arg().find("goldens=print") != std::string::npos;
}

inline void
printDispute(beast::unit_test::suite& s, KProfiledDisputeSample const& r)
{
    s.log << "GOLDEN-DISPUTE"
          << " k=" << r.k << " fp=0x" << std::hex << r.fingerprint << std::dec
          << " events=" << r.events << " weightedEvents=" << r.weightedEvents
          << " steps=" << r.steps << " beats=" << r.beats
          << " minValidated=" << r.minValidated
          << " maxValidated=" << r.maxValidated
          << " forkCheckedSeqs=" << r.forkCheckedSeqs << " target=" << r.target
          << " acceptedSeq=" << r.acceptedSeq << " txASeq=" << r.txASeq
          << " txBSeq=" << r.txBSeq << " clampHits=" << r.clampHits
          << " requestedMs=" << r.requestedMs << " consumedMs=" << r.consumedMs
          << " maxBeatMs=" << r.maxConsumedBeatMs
          << " schedulerMs=" << r.schedulerMs
          << " heartbeat=" << r.heartbeatEvents
          << " deliver=" << r.deliverEvents << " job=" << r.jobEvents
          << " timer=" << r.timerEvents
          << " firstClampWeight=" << r.firstClampWeight
          << " submitted=" << r.submittedA << "/" << r.submittedB
          << " forkFree=" << r.forkFree << " converged=" << r.converged
          << " exactlyOne=" << r.exactlyOneAccepted
          << " acceptedSet=" << r.acceptedSetVerified
          << " historyReady=" << r.historyReadyAtSnapshot << std::endl;
}

inline void
printFork(beast::unit_test::suite& s, KProfiledForkCell const& r)
{
    s.log << "GOLDEN-FORK"
          << " overlap=" << r.overlap << " k=" << r.k
          << " unitCostMs=" << r.unitCostMs << " fp=0x" << std::hex
          << r.fingerprint << std::dec << " events=" << r.events
          << " weightedEvents=" << r.weightedEvents << " steps=" << r.steps
          << " beats=" << r.beats << " minValidated=" << r.minValidated
          << " maxValidated=" << r.maxValidated
          << " forkCheckedSeqs=" << r.forkCheckedSeqs << " target=" << r.target
          << " divergentSeq=" << r.divergentSeq << " agreedSeq=" << r.agreedSeq
          << " txASeq=" << r.txASeq << " txBSeq=" << r.txBSeq
          << " clampHits=" << r.clampHits << " requestedMs=" << r.requestedMs
          << " consumedMs=" << r.consumedMs
          << " maxBeatMs=" << r.maxConsumedBeatMs
          << " schedulerMs=" << r.schedulerMs
          << " heartbeat=" << r.heartbeatEvents
          << " deliver=" << r.deliverEvents << " job=" << r.jobEvents
          << " timer=" << r.timerEvents
          << " firstClampWeight=" << r.firstClampWeight
          << " submitted=" << r.submittedA << "/" << r.submittedB
          << " forkFree=" << r.forkFree << " forked=" << r.forked
          << " safeResolved=" << r.safeResolved
          << " unresolved=" << r.unresolved << " saturated=" << r.saturated
          << std::endl;
}

inline void
printScalar(
    beast::unit_test::suite& s,
    char const* name,
    std::uint64_t observed)
{
    auto const decimal = std::string(name).find("Events") != std::string::npos;
    s.log << "GOLDEN-SCALAR " << name << "=";
    if (decimal)
        s.log << observed;
    else
        s.log << "0x" << std::hex << observed << std::dec;
    s.log << std::endl;
}

}  // namespace ripple::test::goldens::donor

#endif
