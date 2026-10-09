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

#include <xrpld/app/misc/RotatingBloomFilter.h>
#include <xrpl/beast/unit_test.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace ripple {
namespace test {

class RotatingBloomFilter_test : public beast::unit_test::suite
{
    using time_point = std::chrono::steady_clock::time_point;

    void
    testWindow()
    {
        testcase("window");
        using namespace std::chrono_literals;

        RotatingBloomFilter<1024, 4, 4> filter{10s};
        time_point const t{};
        std::string const a{"a"};
        std::string const b{"b"};

        filter.insert(a, t);
        BEAST_EXPECT(filter.contains(a, t));
        BEAST_EXPECT(!filter.contains(b, t));

        // Remembered to the end of the third epoch after its own, and
        // forgotten at the start of the fourth.
        BEAST_EXPECT(filter.contains(a, t + 39s));
        BEAST_EXPECT(!filter.contains(a, t + 40s));

        // Inserting again extends it.
        filter.insert(a, t + 35s);
        BEAST_EXPECT(filter.contains(a, t + 69s));
        BEAST_EXPECT(!filter.contains(a, t + 70s));

        // A clock that jumps finds everything expired.
        filter.insert(b, t + 70s);
        BEAST_EXPECT(!filter.contains(b, t + 1h));

        // An insertion that read the clock before another thread rotated its
        // slot lands in the newer epoch rather than being lost.
        filter.insert(b, t + 1h + 40s);
        filter.insert(a, t + 1h);
        BEAST_EXPECT(filter.contains(a, t + 1h + 40s));
    }

    void
    testFalsePositives()
    {
        testcase("false positives");
        using namespace std::chrono_literals;

        using Filter = RotatingBloomFilter<std::size_t{1} << 14, 4, 4>;
        static_assert(Filter::bytes == 8192);

        Filter filter{120s};
        time_point const t{};

        std::uint64_t const inserted = 1000;
        for (std::uint64_t i = 0; i < inserted; ++i)
            filter.insert(i, t);

        bool all = true;
        for (std::uint64_t i = 0; i < inserted; ++i)
            all = all && filter.contains(i, t);
        BEAST_EXPECT(all);

        // About 0.24% in theory for a thousand elements in one generation.
        std::uint64_t const probes = 100000;
        std::uint64_t hits = 0;
        for (std::uint64_t i = inserted; i < inserted + probes; ++i)
            hits += filter.contains(i, t) ? 1 : 0;
        BEAST_EXPECT(hits < probes / 100);
    }

public:
    void
    run() override
    {
        testWindow();
        testFalsePositives();
    }
};

BEAST_DEFINE_TESTSUITE(RotatingBloomFilter, app, ripple);

}  // namespace test
}  // namespace ripple
