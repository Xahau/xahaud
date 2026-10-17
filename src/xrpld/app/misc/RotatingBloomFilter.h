//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_APP_MISC_ROTATINGBLOOMFILTER_H_INCLUDED
#define RIPPLE_APP_MISC_ROTATINGBLOOMFILTER_H_INCLUDED

#include <xrpl/basics/hardened_hash.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace ripple {

/** Approximate membership of everything inserted recently, in fixed space.

    Time is cut into epochs of a fixed interval. Each of the Generations
    filters holds the insertions of one epoch: an insertion goes to the filter
    for the current epoch, and a query consults every filter whose epoch is
    one of the last Generations. An element is therefore reported for at least
    (Generations - 1) and at most Generations intervals after it was last
    inserted, and then forgotten.

    A filter is cleared lazily, when its slot is first wanted for a newer
    epoch. Nothing has to tick: an idle filter costs nothing, and a clock that
    jumps forward simply finds every slot expired.

    Within the window there are no false negatives. False positives occur at
    the usual bloom rate for one epoch's insertions, once per live generation.
    The footprint is Generations * Bits / 8 bytes however much is inserted:
    saturating the filter raises the false positive rate and never grows it.

    Bit positions come from a hash seeded randomly per instance, so nobody
    without the seed can choose elements that set a particular set of bits.

    @par Thread Safety
    Safe to call concurrently. The internal mutex is never held while calling
    out, so this is a leaf in any lock order.
*/
template <std::size_t Bits, std::size_t Hashes, std::size_t Generations>
class RotatingBloomFilter
{
    static_assert(
        Bits >= 64 && (Bits & (Bits - 1)) == 0,
        "Bits must be a power of two, at least 64");
    static_assert(
        Bits <= (std::size_t{1} << 32),
        "bit positions are computed in 32 bits");
    static_assert(Hashes >= 1 && Hashes <= 16);
    static_assert(
        Generations >= 2,
        "a single generation would forget everything at each rotation");

public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using duration = clock_type::duration;

    /** Size of the filter state, in bytes. */
    static constexpr std::size_t bytes = Generations * Bits / 8;

    explicit RotatingBloomFilter(duration interval)
        : interval_(interval > duration::zero() ? interval : duration{1})
    {
        epochs_.fill(none);
    }

    RotatingBloomFilter(RotatingBloomFilter const&) = delete;
    RotatingBloomFilter&
    operator=(RotatingBloomFilter const&) = delete;

    /** Length of one epoch. */
    duration
    interval() const
    {
        return interval_;
    }

    template <class T>
    void
    insert(T const& item, time_point now)
    {
        auto const positions = probe(item);
        auto const epoch = epochOf(now);
        auto const slot = slotOf(epoch);

        std::lock_guard lock{mutex_};

        // The slot last held an earlier epoch, now out of the window or about
        // to be: it is reused for this one. A slot already ahead of us (the
        // caller read the clock before another thread rotated) keeps its bits,
        // and ours are added to them: that can only remember the element a
        // little longer than asked, never forget it early.
        if (epochs_[slot] < epoch)
        {
            bits_[slot].fill(0);
            epochs_[slot] = epoch;
        }

        for (auto const p : positions)
            bits_[slot][p / 64] |= std::uint64_t{1} << (p % 64);
    }

    template <class T>
    bool
    contains(T const& item, time_point now) const
    {
        auto const positions = probe(item);
        auto const oldest =
            epochOf(now) - static_cast<std::int64_t>(Generations);

        std::lock_guard lock{mutex_};

        for (std::size_t g = 0; g < Generations; ++g)
        {
            if (epochs_[g] == none || epochs_[g] <= oldest)
                continue;

            bool all = true;
            for (auto const p : positions)
            {
                if ((bits_[g][p / 64] & (std::uint64_t{1} << (p % 64))) == 0)
                {
                    all = false;
                    break;
                }
            }

            if (all)
                return true;
        }

        return false;
    }

private:
    static constexpr std::size_t words = Bits / 64;
    static constexpr std::int64_t none =
        std::numeric_limits<std::int64_t>::min();

    std::int64_t
    epochOf(time_point now) const
    {
        // Floor division, so a clock reading before its own epoch still maps
        // to consecutive epochs.
        auto const t = now.time_since_epoch().count();
        auto const i = interval_.count();
        return t >= 0 ? t / i : -((-t + i - 1) / i);
    }

    static std::size_t
    slotOf(std::int64_t epoch)
    {
        auto const g = static_cast<std::int64_t>(Generations);
        return static_cast<std::size_t>(((epoch % g) + g) % g);
    }

    /** Bit positions for an element: Kirsch-Mitzenmacher double hashing.

        Hashes positions from a single 64 bit hash, as h1 + i * h2. Forcing h2
        odd makes it invertible modulo the power of two table size, so the
        positions for one element are all distinct.
    */
    template <class T>
    std::array<std::uint32_t, Hashes>
    probe(T const& item) const
    {
        std::uint64_t const h = hash_(item);
        auto const h1 = static_cast<std::uint32_t>(h);
        auto const h2 = static_cast<std::uint32_t>(h >> 32) | 1u;
        constexpr auto mask = static_cast<std::uint32_t>(Bits - 1);

        std::array<std::uint32_t, Hashes> positions;
        for (std::size_t i = 0; i < Hashes; ++i)
            positions[i] = (h1 + static_cast<std::uint32_t>(i) * h2) & mask;
        return positions;
    }

    hardened_hash<> const hash_;
    duration const interval_;
    std::mutex mutable mutex_;
    std::array<std::int64_t, Generations> epochs_;
    std::array<std::array<std::uint64_t, words>, Generations> bits_{};
};

}  // namespace ripple

#endif
