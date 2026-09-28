//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/UTF8.h>
#include <xrpl/beast/unit_test.h>

namespace ripple {

struct UTF8_test : public beast::unit_test::suite
{
    void
    run() override
    {
        testcase("isValidUTF8");

        auto const check = [](std::string const& s) {
            return isValidUTF8(makeSlice(s));
        };

        // Sequences truncated by the end of the buffer are invalid. The
        // previous decoder read past the end of the vector here.
        BEAST_EXPECT(!check("a\xC2"));
        BEAST_EXPECT(!check("a\xE0\xA4"));
        BEAST_EXPECT(!check("a\xEF\xBF"));
        BEAST_EXPECT(!check("a\xF0\x90\x8D"));
        BEAST_EXPECT(!check(std::string("\xC2", 1)));
        BEAST_EXPECT(!check(std::string("\xF4\x8F\xBF", 3)));

        // The complete sequences are fine.
        BEAST_EXPECT(check("a\xC2\xA2"));
        BEAST_EXPECT(check("a\xE0\xA4\xB9"));
        BEAST_EXPECT(check("a\xF0\x90\x8D\x88"));
        BEAST_EXPECT(check("a\xF4\x8F\xBF\xBF"));
        BEAST_EXPECT(check(""));

        // U+FFFE and U+FFFF are rejected wherever they appear.
        BEAST_EXPECT(!check("\xEF\xBF\xBE"));
        BEAST_EXPECT(!check("ab\xEF\xBF\xBF"));
        BEAST_EXPECT(!check("\xE2\x82\xAC\xEF\xBF\xBEz"));

        // Their neighbours are not, and nor are the other noncharacters,
        // which the validator has never rejected.
        BEAST_EXPECT(check("\xEF\xBF\xBD"));      // U+FFFD
        BEAST_EXPECT(check("\xEF\xBE\xBF"));      // U+FFBF
        BEAST_EXPECT(check("\xEF\xBB\xBF"));      // U+FEFF (BOM)
        BEAST_EXPECT(check("\xF0\x9F\xBF\xBE"));  // U+1FFFE
    }
};

BEAST_DEFINE_TESTSUITE(UTF8, basics, ripple);

}  // namespace ripple
