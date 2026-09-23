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

#include <xrpl/basics/random.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/beast/xor_shift_engine.h>

#include <array>
#include <cstdint>
#include <random>

namespace ripple {
namespace test {
namespace {

// 32-bit output stored in a 32-bit result.
struct Bits32
{
    using result_type = std::uint32_t;
    result_type n = 0;
    static constexpr result_type
    min()
    {
        return 0;
    }
    static constexpr result_type
    max()
    {
        return 0xffffffffu;
    }
    result_type
    operator()()
    {
        return n++;
    }
};

// 32-bit output stored in a 64-bit result. This is the Linux mt19937 shape.
struct Bits32In64
{
    using result_type = std::uint64_t;
    result_type n = 0;
    static constexpr result_type
    min()
    {
        return 0;
    }
    static constexpr result_type
    max()
    {
        return 0xffffffffu;
    }
    result_type
    operator()()
    {
        return n++;
    }
};

// Nonzero minimum, power-of-two range of 256 values.
struct NonzeroMin
{
    using result_type = std::uint32_t;
    result_type n = 0;
    static constexpr result_type
    min()
    {
        return 5;
    }
    static constexpr result_type
    max()
    {
        return 5 + 255;
    }
    result_type
    operator()()
    {
        return static_cast<result_type>(min() + (n++ % 256));
    }
};

}  // namespace

class random_test : public beast::unit_test::suite
{
    // 8-bit model of reject-then-modulo. slack = 2^w % count, drop the
    // low slack words, then modulo. Every accepted result must have the
    // same number of preimages.
    void
    testReducedMapping()
    {
        testcase("8-bit reject-then-modulo is uniform");
        constexpr int universe = 256;
        for (int count = 1; count <= universe; ++count)
        {
            int const slack = universe % count;
            std::array<int, 256> hits{};
            int accepted = 0;
            for (int raw = 0; raw < universe; ++raw)
            {
                if (raw < slack)
                    continue;
                ++hits[raw % count];
                ++accepted;
            }
            BEAST_EXPECT(accepted % count == 0);
            auto const each = accepted / count;
            for (int result = 0; result < count; ++result)
                BEAST_EXPECT(hits[result] == each);
        }
    }

    template <class Engine, class Integral>
    void
    expectInRange(Engine& engine, Integral min, Integral max, int samples)
    {
        for (int i = 0; i < samples; ++i)
        {
            auto const value = rand_int(engine, min, max);
            BEAST_EXPECT(value >= min);
            BEAST_EXPECT(value <= max);
        }
    }

public:
    void
    testEngineVectors()
    {
        testcase("engine range vectors and draw consumption");

        beast::xor_shift_engine full{1};
        beast::xor_shift_engine fullTwin{1};
        BEAST_EXPECT(detail::randomU64(full) == fullTwin());
        BEAST_EXPECT(full() == fullTwin());

        Bits32 narrow;
        Bits32 narrowTwin;
        auto const narrowWord = detail::randomU64(narrow);
        auto const nLow = static_cast<std::uint64_t>(narrowTwin());
        auto const nHigh = static_cast<std::uint64_t>(narrowTwin());
        BEAST_EXPECT(narrowWord == (nLow | (nHigh << 32)));
        BEAST_EXPECT(narrow() == narrowTwin());

        Bits32In64 wideStore;
        Bits32In64 wideStoreTwin;
        auto const wideWord = detail::randomU64(wideStore);
        auto const wLow = wideStoreTwin();
        auto const wHigh = wideStoreTwin();
        BEAST_EXPECT(wideWord == (wLow | (wHigh << 32)));
        BEAST_EXPECT(wideStore() == wideStoreTwin());

        NonzeroMin shifted;
        NonzeroMin shiftedTwin;
        std::uint64_t composed = 0;
        for (int i = 0; i < 8; ++i)
        {
            auto const piece =
                static_cast<std::uint64_t>(shiftedTwin() - NonzeroMin::min());
            composed |= piece << (8 * i);
        }
        BEAST_EXPECT(detail::randomU64(shifted) == composed);
        BEAST_EXPECT(shifted() == shiftedTwin());

        std::minstd_rand uneven{12345};
        std::minstd_rand unevenCopy{12345};
        std::independent_bits_engine<std::minstd_rand, 64, std::uint64_t> ibits{
            unevenCopy};
        for (int i = 0; i < 8; ++i)
            BEAST_EXPECT(detail::randomU64(uneven) == ibits());
    }

    void
    testBounds()
    {
        testcase("closed bounds, including signed endpoints and 2^40");

        auto check = [&](auto engine) {
            expectInRange(engine, 0, 10, 64);
            expectInRange(engine, 0, 9999, 64);
            expectInRange(engine, -20, 20, 64);
            expectInRange(engine, -5, 15, 64);
            expectInRange(
                engine, std::uint64_t{0}, (std::uint64_t{1} << 40) + 123u, 8);
        };

        check(beast::xor_shift_engine{7});
        check(Bits32{});
        check(Bits32In64{});
        check(NonzeroMin{});
        check(std::minstd_rand{7});
        check(std::mt19937{7});
    }

    void
    run() override
    {
        testReducedMapping();
        testEngineVectors();
        testBounds();
    }
};

BEAST_DEFINE_TESTSUITE(random, basics, ripple);

}  // namespace test
}  // namespace ripple
