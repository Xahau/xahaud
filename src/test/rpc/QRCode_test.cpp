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

#include <xrpld/rpc/detail/QRCode.h>
#include <xrpl/beast/unit_test.h>
#include <algorithm>

namespace ripple {
namespace test {

/** The QR encoder behind export_setup. The vector is version 1-Q, mask 6,
    and matches segno; the encoder was also checked module for module
    against segno (alphanumeric) and python-qrcode (byte mode) across
    versions and masks, and decoded by zxing. */
class QRCode_test : public beast::unit_test::suite
{
    static std::vector<std::string>
    rows(std::vector<std::vector<bool>> const& code)
    {
        std::vector<std::string> out;
        for (auto const& r : code)
        {
            out.emplace_back();
            for (bool b : r)
                out.back() += b ? '1' : '0';
        }
        return out;
    }

    // UTF-8 code points
    static std::size_t
    width(std::string const& s)
    {
        return std::count_if(
            s.begin(), s.end(), [](char c) { return (c & 0xC0) != 0x80; });
    }

    void
    testVector()
    {
        testcase("vector");
        std::vector<std::string> const want{
            "111111100001001111111", "100000101100101000001",
            "101110100101101011101", "101110101111101011101",
            "101110101101001011101", "100000100100101000001",
            "111111101010101111111", "000000001101100000000",
            "010111101100111011010", "101111010000111101110",
            "001010110001001100000", "101101000101100011000",
            "110111111110111011111", "000000001000100101000",
            "111111100110011001111", "100000101010010010111",
            "101110101101001000111", "101110101011100010100",
            "101110100100001000011", "100000101110011100110",
            "111111100101000000010"};
        BEAST_EXPECT(rows(qr::encode("HELLO WORLD", 6)) == want);
    }

    void
    testCapacity()
    {
        testcase("capacity");
        // version 40-L holds 4296 alphanumeric characters or 2953 bytes
        BEAST_EXPECT(qr::encode(std::string(4296, 'A')).size() == 177);
        BEAST_EXPECT(qr::encode(std::string(4297, 'A')).empty());
        BEAST_EXPECT(qr::encode(std::string(2953, 'a')).size() == 177);
        BEAST_EXPECT(qr::encode(std::string(2954, 'a')).empty());

        // a lowercase link ending in uppercase hex packs the hex
        // alphanumerically: 3400 hex digits need version 36, not 40+
        auto const url = "https://xaman.app/detect/" + std::string(3400, '7');
        BEAST_EXPECT(qr::encode(url).size() == 4 * 36 + 17);
    }

    void
    testRender()
    {
        testcase("render");
        auto const code = qr::encode("HELLO WORLD");
        auto const lines = qr::render(code);
        BEAST_EXPECT(lines.size() == (21 + 8 + 1) / 2);
        for (auto const& l : lines)
            BEAST_EXPECT(width(l) == 21 + 8);

        // the quiet zone is light: drawn, or blank when inverted
        BEAST_EXPECT(lines.front() == [] {
            std::string s;
            for (int i = 0; i < 29; ++i)
                s += "\xE2\x96\x88";
            return s;
        }());
        BEAST_EXPECT(qr::render(code, true).front() == std::string(29, ' '));
    }

public:
    void
    run() override
    {
        testVector();
        testCapacity();
        testRender();
    }
};

BEAST_DEFINE_TESTSUITE(QRCode, rpc, ripple);

}  // namespace test
}  // namespace ripple
