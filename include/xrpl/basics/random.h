//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_BASICS_RANDOM_H_INCLUDED
#define RIPPLE_BASICS_RANDOM_H_INCLUDED

#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/beast/xor_shift_engine.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <type_traits>

namespace ripple {

#ifndef __INTELLISENSE__
static_assert(
    std::is_integral<beast::xor_shift_engine::result_type>::value &&
        std::is_unsigned<beast::xor_shift_engine::result_type>::value,
    "The Ripple default PRNG engine must return an unsigned integral type.");

static_assert(
    std::numeric_limits<beast::xor_shift_engine::result_type>::max() >=
        std::numeric_limits<std::uint64_t>::max(),
    "The Ripple default PRNG engine return must be at least 64 bits wide.");
#endif

namespace detail {

// Determines if a type can be called like an Engine
template <class Engine, class Result = typename Engine::result_type>
using is_engine = std::is_invocable_r<Result, Engine>;

// 64 bits from the engine. Width comes from max()-min(), not from the
// storage type: a 32-bit engine may use a 64-bit result_type.
template <class Engine>
std::uint64_t
randomU64(Engine& engine)
{
    static_assert(std::is_unsigned_v<typename Engine::result_type>);
    static_assert(
        std::numeric_limits<typename Engine::result_type>::digits <= 64);
    static_assert(Engine::min() < Engine::max());
    auto const draw = [&engine]() -> std::uint64_t {
        return static_cast<std::uint64_t>(engine() - Engine::min());
    };

    // A wrapped cardinality of 0 means 2^64 values: one full-range draw.
    constexpr auto span =
        static_cast<std::uint64_t>(Engine::max() - Engine::min());
    constexpr std::uint64_t range = span + 1u;
    constexpr bool full = range == 0;
    constexpr bool powerOfTwo = full || (range & (range - 1u)) == 0;

    if constexpr (powerOfTwo)
    {
        constexpr int width = full ? 64 : std::bit_width(range) - 1;
        if constexpr (width >= 64)
            return draw();

        std::uint64_t value = 0;
        int filled = 0;
        while (filled < 64)
        {
            auto const take = width < (64 - filled) ? width : (64 - filled);
            auto const mask = (std::uint64_t{1} << take) - 1;
            value |= (draw() & mask) << filled;
            filled += take;
        }
        return value;
    }
    else
    {
        // [rand.adapt.ibits] for w = 64. R is not a power of two.
        constexpr int m = std::bit_width(range) - 1;
        constexpr int nCeil = (64 + m - 1) / m;
        constexpr int w0Try = 64 / nCeil;
        constexpr auto y0Try = (std::uint64_t{1} << w0Try) * (range >> w0Try);
        constexpr bool bump =
            (range - y0Try) > (y0Try / static_cast<std::uint64_t>(nCeil));
        constexpr int n = bump ? nCeil + 1 : nCeil;
        constexpr int w0 = 64 / n;
        constexpr int n0 = n - (64 % n);
        constexpr auto y0 = (std::uint64_t{1} << w0) * (range >> w0);
        constexpr auto y1 =
            (std::uint64_t{1} << (w0 + 1)) * (range >> (w0 + 1));
        // R == 3 gives n == 65 and w0 == 0. The first group still
        // consumes its draw even though it contributes no output bits.
        static_assert(w0 >= 0 && w0 < 63);

        std::uint64_t word = 0;
        for (int k = 0; k != n0; ++k)
        {
            std::uint64_t u;
            do
            {
                u = draw();
            } while (u >= y0);
            word = (word << w0) + (u & ((std::uint64_t{1} << w0) - 1));
        }
        for (int k = n0; k != n; ++k)
        {
            std::uint64_t u;
            do
            {
                u = draw();
            } while (u >= y1);
            constexpr int bits = w0 + 1;
            word = (word << bits) + (u & ((std::uint64_t{1} << bits) - 1));
        }
        return word;
    }
}
}  // namespace detail

/** Return the default random engine.

    This engine is guaranteed to be deterministic, but by
    default will be randomly seeded. It is NOT cryptographically
    secure and MUST NOT be used to generate randomness that
    will be used for keys, secure cookies, IVs, padding, etc.

    Each thread gets its own instance of the engine which
    will be randomly seeded.
*/
inline beast::xor_shift_engine&
default_prng()
{
    // This is used to seed the thread-specific PRNGs on demand
    static beast::xor_shift_engine seeder = [] {
        std::random_device rng;
        std::uniform_int_distribution<std::uint64_t> distribution{1};
        return beast::xor_shift_engine(distribution(rng));
    }();

    // This protects the seeder
    static std::mutex m;

    // The thread-specific PRNGs:
    thread_local beast::xor_shift_engine engine = [] {
        std::uint64_t seed;
        {
            std::lock_guard lk(m);
            std::uniform_int_distribution<std::uint64_t> distribution{1};
            seed = distribution(seeder);
        }
        return beast::xor_shift_engine{seed};
    }();

    return engine;
}

/** Return a uniformly distributed random integer.

    @param min The smallest value to return. If not specified
               the value defaults to 0.
    @param max The largest value to return. If not specified
               the value defaults to the largest value that
               can be represented.

    The randomness is generated by the specified engine (or
    the default engine if one is not specified). The result
    is cryptographically secure only when the engine passed
    into the function is cryptographically secure.

    @note The range is always a closed interval, so calling
          rand_int(-5, 15) can return any integer in the
          closed interval [-5, 15]; similarly, calling
          rand_int(7) can return any integer in the closed
          interval [0, 7].
*/
/** @{ */
template <class Engine, class Integral>
std::enable_if_t<
    std::is_integral<Integral>::value && detail::is_engine<Engine>::value,
    Integral>
rand_int(Engine& engine, Integral min, Integral max)
{
    XRPL_ASSERT(max > min, "ripple::rand_int : max over min inputs");

    // Closed interval. Rejection sampling keeps the result uniform and the
    // same on libc++ and libstdc++ for a given engine sequence.
    using U = std::make_unsigned_t<Integral>;
    auto const span = static_cast<U>(static_cast<U>(max) - static_cast<U>(min));
    auto const count = static_cast<std::uint64_t>(span) + 1u;
    if (count == 0)
        return static_cast<Integral>(detail::randomU64(engine));

    // Values below this threshold are the leftover that would bias x % count.
    auto const slack = static_cast<std::uint64_t>(-count) % count;
    std::uint64_t draw;
    do
    {
        draw = detail::randomU64(engine);
    } while (draw < slack);

    auto const offset = static_cast<U>(draw % count);
    return static_cast<Integral>(static_cast<U>(static_cast<U>(min) + offset));
}

template <class Integral>
std::enable_if_t<std::is_integral<Integral>::value, Integral>
rand_int(Integral min, Integral max)
{
    return rand_int(default_prng(), min, max);
}

template <class Engine, class Integral>
std::enable_if_t<
    std::is_integral<Integral>::value && detail::is_engine<Engine>::value,
    Integral>
rand_int(Engine& engine, Integral max)
{
    return rand_int(engine, Integral(0), max);
}

template <class Integral>
std::enable_if_t<std::is_integral<Integral>::value, Integral>
rand_int(Integral max)
{
    return rand_int(default_prng(), max);
}

template <class Integral, class Engine>
std::enable_if_t<
    std::is_integral<Integral>::value && detail::is_engine<Engine>::value,
    Integral>
rand_int(Engine& engine)
{
    return rand_int(engine, std::numeric_limits<Integral>::max());
}

template <class Integral = int>
std::enable_if_t<std::is_integral<Integral>::value, Integral>
rand_int()
{
    return rand_int(default_prng(), std::numeric_limits<Integral>::max());
}
/** @} */

/** Return a random byte */
/** @{ */
template <class Byte, class Engine>
std::enable_if_t<
    (std::is_same<Byte, unsigned char>::value ||
     std::is_same<Byte, std::uint8_t>::value) &&
        detail::is_engine<Engine>::value,
    Byte>
rand_byte(Engine& engine)
{
    return static_cast<Byte>(rand_int<Engine, std::uint32_t>(
        engine,
        std::numeric_limits<Byte>::min(),
        std::numeric_limits<Byte>::max()));
}

template <class Byte = std::uint8_t>
std::enable_if_t<
    (std::is_same<Byte, unsigned char>::value ||
     std::is_same<Byte, std::uint8_t>::value),
    Byte>
rand_byte()
{
    return rand_byte<Byte>(default_prng());
}
/** @} */

/** Return a random boolean value */
/** @{ */
template <class Engine>
inline bool
rand_bool(Engine& engine)
{
    return rand_int(engine, 1) == 1;
}

inline bool
rand_bool()
{
    return rand_bool(default_prng());
}
/** @} */

}  // namespace ripple

#endif  // RIPPLE_BASICS_RANDOM_H_INCLUDED
