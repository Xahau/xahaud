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
            BEAST_EXPECT(info->selected == 3);

        BEAST_EXPECT(validatorBitsetContains(makeSlice(bitset), 10, 0));
        BEAST_EXPECT(validatorBitsetContains(makeSlice(bitset), 10, 7));
        BEAST_EXPECT(!validatorBitsetContains(makeSlice(bitset), 10, 8));
        BEAST_EXPECT(validatorBitsetContains(makeSlice(bitset), 10, 9));
        BEAST_EXPECT(!validatorBitsetContains(makeSlice(bitset), 10, 10));
        BEAST_EXPECT(!validatorBitsetContains(makeSlice(bitset), 10, 80));

        Blob const wrongSize{0x81};
        BEAST_EXPECT(!validatorBitsetContains(makeSlice(wrongSize), 10, 0));
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
            BEAST_EXPECT(info->selected == 10);
        BEAST_EXPECT(!validateValidatorBitset(makeSlice(highBitSet), 10));
        BEAST_EXPECT(!validateValidatorBitset(makeSlice(tooLong), 10));

        Blob const fullByte{0xff};
        auto const full = validateValidatorBitset(makeSlice(fullByte), 8);
        BEAST_EXPECT(full);
        if (full)
            BEAST_EXPECT(full->selected == 8);
    }

    void
    run() override
    {
        testByteCount();
        testConstructionAndPopulation();
        testCanonicalShape();
    }
};

BEAST_DEFINE_TESTSUITE(ValidatorBitset, protocol, ripple);

}  // namespace test
}  // namespace ripple
