//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ValidatorBitset.h>

#include <utility>

namespace ripple {
namespace test {

class ValidatorBitset_test : public beast::unit_test::suite
{
public:
    void
    testByteCount()
    {
        testcase("byte count");

        BEAST_EXPECT(validatorBitsetBytes(0) == 0);
        BEAST_EXPECT(validatorBitsetBytes(1) == 1);
        BEAST_EXPECT(validatorBitsetBytes(8) == 1);
        BEAST_EXPECT(validatorBitsetBytes(9) == 2);
        BEAST_EXPECT(validatorBitsetBytes(16) == 2);
    }

    void
    testConstructionAndPopulation()
    {
        testcase("construction and population");

        auto const bitset = makeValidatorBitset(
            10, [](std::size_t i) { return i == 0 || i == 7 || i == 9; });
        BEAST_EXPECT(bitset == Blob({0x81, 0x02}));

        auto const info = validateValidatorBitset(makeSlice(bitset), 10);
        BEAST_EXPECT(info);
        if (info)
        {
            BEAST_EXPECT(info->selected() == 3);
            BEAST_EXPECT(info->contains(0));
            BEAST_EXPECT(info->contains(7));
            BEAST_EXPECT(!info->contains(8));
            BEAST_EXPECT(info->contains(9));
            BEAST_EXPECT(!info->contains(10));
            BEAST_EXPECT(!info->contains(80));
        }
    }

    void
    testCanonicalShape()
    {
        testcase("canonical shape");

        BEAST_EXPECT(validateValidatorBitset(Slice{}, 0));

        Blob const tooShort{0xff};
        Blob const exact{0xff, 0x03};
        Blob const highBitSet{0xff, 0x83};
        Blob const tooLong{0xff, 0x03, 0x00};

        BEAST_EXPECT(!validateValidatorBitset(makeSlice(tooShort), 10));
        auto const info = validateValidatorBitset(makeSlice(exact), 10);
        BEAST_EXPECT(info);
        if (info)
            BEAST_EXPECT(info->selected() == 10);
        BEAST_EXPECT(!validateValidatorBitset(makeSlice(highBitSet), 10));
        BEAST_EXPECT(!validateValidatorBitset(makeSlice(tooLong), 10));

        Blob const fullByte{0xff};
        auto const full = validateValidatorBitset(makeSlice(fullByte), 8);
        BEAST_EXPECT(full);
        if (full)
            BEAST_EXPECT(full->selected() == 8);
    }

    void
    testValidatedOwnership()
    {
        testcase("validated ownership");

        auto const temporary = validateValidatorBitset(
            makeSlice(makeValidatorBitset(
                10, [](std::size_t i) { return i == 1 || i == 9; })),
            10);
        BEAST_EXPECT(temporary);
        if (temporary)
        {
            BEAST_EXPECT(temporary->selected() == 2);
            BEAST_EXPECT(temporary->contains(1));
            BEAST_EXPECT(temporary->contains(9));
        }

        Blob mutableBacking{0x01, 0x00};
        auto const copied =
            validateValidatorBitset(makeSlice(mutableBacking), 10);
        BEAST_EXPECT(copied);
        mutableBacking = Blob{0x00, 0x02};
        if (copied)
        {
            BEAST_EXPECT(copied->selected() == 1);
            BEAST_EXPECT(copied->contains(0));
            BEAST_EXPECT(!copied->contains(9));
        }
    }

    void
    testValueSemantics()
    {
        testcase("value semantics");

        Blob const bits{0x01, 0x02};
        auto source = validateValidatorBitset(makeSlice(bits), 10);
        BEAST_EXPECT(source);
        if (!source)
            return;

        auto copy = *source;
        BEAST_EXPECT(copy.selected() == 2);
        BEAST_EXPECT(copy.contains(0));
        BEAST_EXPECT(copy.contains(9));

        auto moved = std::move(*source);
        BEAST_EXPECT(moved.selected() == 2);
        BEAST_EXPECT(moved.contains(0));
        BEAST_EXPECT(moved.contains(9));
        BEAST_EXPECT(source->selected() == 0);
        BEAST_EXPECT(!source->contains(0));

        auto assigned = copy;
        assigned = std::move(moved);
        BEAST_EXPECT(assigned.selected() == 2);
        BEAST_EXPECT(assigned.contains(0));
        BEAST_EXPECT(assigned.contains(9));
        BEAST_EXPECT(moved.selected() == 0);
        BEAST_EXPECT(!moved.contains(0));
    }

    void
    run() override
    {
        testByteCount();
        testConstructionAndPopulation();
        testCanonicalShape();
        testValidatedOwnership();
        testValueSemantics();
    }
};

BEAST_DEFINE_TESTSUITE(ValidatorBitset, protocol, ripple);

}  // namespace test
}  // namespace ripple
