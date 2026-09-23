//------------------------------------------------------------------------------
/*
    Per-lineage golden manifest for the dsf stepping suites.

    lineage:            dsf
    seed base:          0xFAB1E5EED0000000 (harness default)
    resolving K=3 seed: 0x1000000000000001
    traffic seed:       0x5452414646494331
    topology:           slow-minority and dispute are 5 validators, mesh.
                        The hub pin is 5 validators, each linked only to one
                        non-validator hub. Traffic is 3 validators, mesh.
                        Trust fork cells are N=10 with the overlap in the row.
    pacing:             profiled dispute K in {0,1,2,3,3,4}, unitCost 5ms,
                        heartbeat budget 160. The second K=3 row uses the
                        resolving seed. Trust fork cells vary overlap, K and
                        unitCostMs (5 or 1) as stored in each row.
    RNG mapping:        c527e0a721 (engine-range randomU64 plus the zero-bit
                        group fix)
    captured at:        3f27fbcd5b
                        The numbers are unchanged since 020db70424.
    platforms:          macOS libc++ captured the rows. Linux libstdc++
                        reconfirmed the donor suites, including the K=3
                        rows, at 3f27fbcd5b.

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
    automatic. The pinned 2000 values are the highest virtual time one
    beat consumed, not a fixed per-beat budget.
*/
//==============================================================================

#ifndef XRPL_TEST_CONSENSUS_GOLDENS_DSF_H_INCLUDED
#define XRPL_TEST_CONSENSUS_GOLDENS_DSF_H_INCLUDED

#include <xrpl/beast/unit_test/suite.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ripple::test::goldens::dsf {

inline constexpr std::uint64_t kSeedBase = 0xFAB1E5EED0000000ull;
inline constexpr std::uint64_t kResolvingSeed = 0x1000000000000001ull;
inline constexpr char kRngMapping[] = "c527e0a721";
inline constexpr char kCapturedAt[] = "3f27fbcd5b";

inline constexpr std::uint64_t kSlowMinorityFingerprint = 0x25ec64d383356379ull;
inline constexpr std::uint64_t kHubNetworkFingerprint = 0x182601635f5b0308ull;
inline constexpr std::uint64_t kDisputeFingerprint = 0x297cf7d89bfd08f7ull;

inline constexpr std::uint64_t kTrafficSeed = 0x5452414646494331ull;
inline constexpr std::uint64_t kTrafficFingerprint = 0xc0dad1d188726d9dull;
inline constexpr std::uint64_t kTrafficEvents = 1448;
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
    // Beats of K=0 continuation that reached the target. 0 when recovery
    // was not needed, or when it ran and still missed the target.
    std::uint32_t recoveryBeats = 0;
    // PRNG base passed to seedPrng before the nodes exist.
    static constexpr std::uint64_t kDefaultSeed = kSeedBase;
    std::uint64_t seed = kDefaultSeed;
    // Not part of operator==. True only when the profiled loop stopped
    // because the heartbeat ceiling was reached.
    bool heartbeatBudgetStop = false;

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
            historyReadyAtSnapshot == o.historyReadyAtSnapshot &&
            recoveryBeats == o.recoveryBeats && seed == o.seed;
    }
};

inline constexpr std::array<KProfiledDisputeSample, 6> kProfiledDispute = {{
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
     true,   true,
     6},
    {3,     0x555c80e47cc1cc21ull,
     3130,  3706,
     1637,  54,
     11,    11,
     10,    11,
     10,    0,
     10,    53,
     55590, 54615,
     2000,  105635,
     265,   630,
     699,   41,
     3,     true,
     true,  true,
     true,  true,
     true,  true,
     0,     0x1000000000000001ull},
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
    {0,     0,     0x56ce6480b4972d74ull,
     3488,  0,     2060,
     0,     8,     8,
     7,     8,     5,
     0,     5,     5,
     0,     0,     0,
     0,     54020, 0,
     0,     0,     0,
     0,     true,  true,
     false, true,  false,
     false, false, 5},
    {0,     1,     0x5bcaa0a57392b44bull,
     3475,  4585,  2047,
     12,    8,     8,
     7,     8,     5,
     0,     5,     5,
     0,     4585,  4585,
     790,   54345, 120,
     1312,  613,   0,
     0,     true,  true,
     false, true,  false,
     false, false, 1},
    {0,     1,     0x66349c6e0ba11e57ull,
     3776,  5322,  2348,
     26,    8,     8,
     7,     8,     6,
     0,     6,     6,
     25,    26610, 26385,
     2000,  68395, 250,
     1161,  878,   57,
     2,     true,  true,
     false, true,  false,
     false, true,  5},
    {4,     0,     0xdc05a22afd9269d0ull,
     6153,  0,     4218,
     0,     8,     8,
     7,     8,     0,
     5,     5,     0,
     0,     0,     0,
     0,     55020, 0,
     0,     0,     0,
     0,     true,  true,
     true,  false, true,
     false, false, 5},
    {4,     1,     0x7872f20cdb7ec1c9ull,
     6231,  9698,  4296,
     14,    8,     8,
     7,     8,     0,
     5,     5,     0,
     6,     9698,  9687,
     1643,  56548, 140,
     2884,  1248,  22,
     3,     true,  true,
     true,  false, true,
     false, true,  1},
    {4,     1,      0x7797f687638895d9ull,
     14855, 32488,  12920,
     160,   5,      5,
     4,     8,      0,
     0,     0,      0,
     160,   162440, 161000,
     2000,  203010, 1490,
     2839,  8140,   449,
     2,     true,   true,
     true,  false,  false,
     true,  true,   5},
    {6,     0,     0xc54816e93aff2fd2ull,
     6147,  0,     4108,
     0,     8,     8,
     7,     8,     0,
     5,     5,     0,
     0,     0,     0,
     0,     54020, 0,
     0,     0,     0,
     0,     true,  true,
     true,  false, true,
     false, false, 5},
    {6,     1,     0x63e402b833368e31ull,
     6292,  9487,  4253,
     15,    8,     8,
     7,     8,     0,
     5,     5,     0,
     7,     9487,  9472,
     1635,  57612, 150,
     2952,  1133,  16,
     3,     true,  true,
     true,  false, true,
     false, true,  1},
    {6,     1,     0xf49350dbcf9e3c9cull,
     6477,  10070, 4438,
     49,    8,     8,
     7,     8,     0,
     6,     6,     0,
     48,    50350, 49935,
     2000,  91945, 480,
     2156,  1676,  124,
     2,     true,  true,
     true,  false, true,
     false, true,  5},
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
          << " k=" << r.k << " seed=0x" << std::hex << r.seed << std::dec
          << " fp=0x" << std::hex << r.fingerprint << std::dec
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
          << " exactlyOneAccepted=" << r.exactlyOneAccepted
          << " acceptedSetVerified=" << r.acceptedSetVerified
          << " historyReady=" << r.historyReadyAtSnapshot
          << " recoveryBeats=" << r.recoveryBeats << std::endl;
}

inline void
printFork(beast::unit_test::suite& s, KProfiledForkCell const& r)
{
    s.log << "GOLDEN-FORK"
          << " overlap=" << r.overlap << " k=" << r.k
          << " unitMs=" << r.unitCostMs << " fp=0x" << std::hex << r.fingerprint
          << std::dec << " events=" << r.events
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

}  // namespace ripple::test::goldens::dsf

#endif
