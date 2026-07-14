//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/digest.h>

#include <cstring>

namespace ripple {
namespace test {
namespace {

uint256
makeOrigin(char const* label)
{
    return sha512Half(Slice(label, std::strlen(label)));
}

}  // namespace

class ExportKeylet_test : public beast::unit_test::suite
{
public:
    void
    testOriginIdentity()
    {
        testcase("origin transaction identity");

        AccountID const fixtureAccount{1};
        uint256 const fixtureOrigin{2};
        uint256 const expected{
            "04FEBC83B833D1A2EBFD1605BA3A0574F"
            "F0B0B8C9690ADBE35CD2F258984EB56"};
        BEAST_EXPECT(
            keylet::shadowTicket(fixtureAccount, fixtureOrigin).key ==
            expected);

        auto const firstAccount =
            calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const secondAccount =
            calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const firstOrigin = makeOrigin("first-export-origin");
        auto const secondOrigin = makeOrigin("second-export-origin");

        auto const key = keylet::shadowTicket(firstAccount, firstOrigin);
        BEAST_EXPECT(key.type == ltSHADOW_TICKET);
        BEAST_EXPECT(
            key.key == keylet::shadowTicket(firstAccount, firstOrigin).key);
        BEAST_EXPECT(
            key.key != keylet::shadowTicket(firstAccount, secondOrigin).key);
        BEAST_EXPECT(
            key.key != keylet::shadowTicket(secondAccount, firstOrigin).key);
    }

    void
    testLegacyKeySeparation()
    {
        testcase("legacy ticket sequence separation");

        auto const account =
            calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const origin = makeOrigin("export-origin");

        BEAST_EXPECT(
            keylet::shadowTicket(account, origin).key !=
            keylet::shadowTicket(account, std::uint32_t{1}).key);
    }

    void
    run() override
    {
        testOriginIdentity();
        testLegacyKeySeparation();
    }
};

BEAST_DEFINE_TESTSUITE(ExportKeylet, protocol, ripple);

}  // namespace test
}  // namespace ripple
