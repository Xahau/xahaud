//------------------------------------------------------------------------------
/*
    This file is part of Beast: https://github.com/vinniefalco/Beast
    Copyright 2014, Vinnie Falco <vinnie.falco@gmail.com>

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

#ifndef BEAST_RANDOM_XOR_SHIFT_ENGINE_H_INCLUDED
#define BEAST_RANDOM_XOR_SHIFT_ENGINE_H_INCLUDED

#include <array>
#include <cstdint>
#include <limits>

namespace beast {

/** MurmurHash3 64-bit finalizer (fmix64).

    A bijective mixing function with strong avalanche behavior: every input
    bit affects every output bit with probability ~0.5.

    Commonly used for:
    - Integer hashing
    - Seed expansion for PRNGs
    - Converting sequential values to uncorrelated values

    @param x The input value to mix.
    @return The mixed output value.
*/
[[nodiscard]] constexpr std::uint64_t
fmix64(std::uint64_t x) noexcept
{
    x = 0xff51afd7ed558ccd * (x ^ (x >> 33));
    x = 0xc4ceb9fe1a85ec53 * (x ^ (x >> 33));
    return x ^ (x >> 33);
}

/** XOR-shift Generator.

    Meets the requirements of UniformRandomNumberGenerator.

    Simple and fast RNG based on:
    http://xorshift.di.unimi.it/xorshift128plus.c
*/
class xor_shift_engine
{
public:
    using result_type = std::uint64_t;

    xor_shift_engine(xor_shift_engine const&) = default;
    xor_shift_engine&
    operator=(xor_shift_engine const&) = default;

    explicit constexpr xor_shift_engine(result_type val = 1977u) noexcept
        : s_{(val * val) + 1, fmix64(val)}
    {
    }

    [[nodiscard]] constexpr result_type
    operator()() noexcept
    {
        result_type s1 = s_[0];
        result_type const s0 = s_[1];
        s_[0] = s0;
        s1 ^= s1 << 23;
        return (s_[1] = (s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26))) + s0;
    }

    static constexpr result_type
    min() noexcept
    {
        return std::numeric_limits<result_type>::min();
    }

    static constexpr result_type
    max() noexcept
    {
        return std::numeric_limits<result_type>::max();
    }

private:
    std::array<result_type, 2> s_;
};

}  // namespace beast

#endif
