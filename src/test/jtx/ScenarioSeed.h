#pragma once
//------------------------------------------------------------------------------
// ScenarioSeed — the explorer's derivation convention (explorer-takeoff §4.1):
// one master seed fans into named, indexed sub-streams; each sub-seed seeds
// one engine; each axis draws its parameters from its own engine in a fixed,
// documented order. Indexed streams (sub(axis, i)) exist so per-item draws
// (per-link fault engines, per-instance seeds) never share a stream with —
// and can never shift — another axis's draws.
//
// REPRODUCIBILITY CONTRACT: a repro token is (schema version, instance seed).
// Axis names, draw order, distributions, and envelope defaults are all part
// of the schema — change ANY of them and every previously minted token now
// derives a different scenario, so bump kSchemaVersion in the same commit.
// The version is printed in every token and checked by the repro entrypoint:
// a stale token must fail loud, never silently "reproduce" the wrong world.
//------------------------------------------------------------------------------
#include <cstdint>
#include <string_view>

namespace ripple::test {

namespace scenario_detail {

// The 64-bit "golden gamma": 2^64/φ (φ = the golden ratio), rounded to odd.
// Because φ is the "most irrational" real, successive multiples of this
// constant mod 2^64 land maximally spread apart (a low-discrepancy Weyl
// sequence) — which is why splitmix64 uses it as its state increment and
// why it makes a good decorrelating multiplier for stream indexes below.
constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ull;

// splitmix64 — the standard seed expander: no zero fixpoint, no correlated
// low bits between adjacent inputs (both defects of raw xorshift mixing).
// The two multiplier constants are Stafford's MurmurHash3-finalizer
// variant (mix13), the published splitmix64 reference values.
constexpr std::uint64_t
splitmix64(std::uint64_t x)
{
    x += kGoldenGamma;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr std::uint64_t
fnv1a64(std::string_view s)
{
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (char const c : s)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

}  // namespace scenario_detail

struct ScenarioSeed
{
    // v2 (2026-07-04): the LIFECYCLE axis (one optional stop/restart cycle)
    // joins the derivation. Fault and submit node draws map into the
    // survivor pool when a restart is planned; for seeds whose lifecycle
    // stream draws no-restart, every axis derives byte-identically to
    // schema 1 (the pool is the full validator set and the mapping is the
    // identity), so schema-1 fingerprints remain valid cross-checks for
    // exactly those seeds.
    // v3 (2026-07-04): the BYZANTINE axis (one optional lie-only
    // equivocating validator) joins LAST, drawing from its own sub-stream
    // after everything else — so a seed whose byzantine stream draws
    // "inactive" derives byte-identically to schema 2, keeping schema-2
    // fingerprints as cross-checks for those seeds.
    // v4 (2026-07-04): a delay fault now applies PER-MESSAGE seeded jitter
    // in [1ms, drawn] (delayJittered) instead of a constant offset
    // (delayAll) — same derivation, different APPLICATION semantics, so
    // delay-bearing instances get new fingerprints. Loss/duplicate/
    // no-fault instances are unaffected.
    static constexpr std::uint64_t kSchemaVersion = 4;

    std::uint64_t master;

    // The named (and optionally indexed) sub-stream seed. Never returns 0:
    // beast::xor_shift_engine cannot be seeded with 0 (degenerate xorshift
    // orbit), and splitmix64 emits 0 for exactly one input — map that one
    // landmine to the golden gamma instead (any fixed nonzero value works;
    // this one is already in the file).
    [[nodiscard]] constexpr std::uint64_t
    sub(std::string_view axis, std::uint64_t index = 0) const
    {
        auto const v = scenario_detail::splitmix64(
            master ^ scenario_detail::fnv1a64(axis) ^
            (index * scenario_detail::kGoldenGamma));
        return v ? v : scenario_detail::kGoldenGamma;
    }
};

}  // namespace ripple::test
