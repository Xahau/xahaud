//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER
    RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ExportShare.h>
#include <xrpl/protocol/SecretKey.h>

namespace ripple {
namespace test {

class ExportShare_test : public beast::unit_test::suite
{
    static ExportShare
    makeShare()
    {
        auto const [key, secret] = randomKeyPair(KeyType::secp256k1);
        auto const signature = sign(key, secret, Slice{"export-share", 12});
        return ExportShare{
            ExportShare::currentVersion,
            calcAccountID(key),
            uint256{1},
            4'200'000,
            uint256{2},
            17,
            key,
            signature};
    }

public:
    void
    testRoundTrip()
    {
        testcase("canonical Export share round trip");

        auto const share = makeShare();
        auto const encoded = share.serialize();
        BEAST_EXPECT(
            encoded.size() <= ExportLimits::maxSerializedExportShareBytes);

        auto const parsed = ExportShare::parse(encoded.slice());
        BEAST_EXPECT(parsed.has_value());
        if (!parsed)
            return;
        BEAST_EXPECT(parsed->version == share.version);
        BEAST_EXPECT(parsed->owner == share.owner);
        BEAST_EXPECT(parsed->originTxn == share.originTxn);
        BEAST_EXPECT(parsed->originLedgerSeq == share.originLedgerSeq);
        BEAST_EXPECT(parsed->originLedgerHash == share.originLedgerHash);
        BEAST_EXPECT(parsed->committeePosition == share.committeePosition);
        BEAST_EXPECT(parsed->signingKey == share.signingKey);
        BEAST_EXPECT(parsed->signature == share.signature);
        BEAST_EXPECT(parsed->wireHash() == share.wireHash());
        BEAST_EXPECT(parsed->serialize().slice() == encoded.slice());
    }

    void
    testMalformed()
    {
        testcase("malformed Export share framing");

        auto share = makeShare();
        auto encoded = share.serialize().peekData();

        BEAST_EXPECT(!ExportShare::parse(Slice{}));
        BEAST_EXPECT(!ExportShare::parse(Slice{encoded.data(), 20}));

        encoded.push_back(0);
        BEAST_EXPECT(!ExportShare::parse(makeSlice(encoded)));

        share.version = ExportShare::currentVersion + 1;
        BEAST_EXPECT(!share.validShape());
        bool threw = false;
        try
        {
            (void)share.serialize();
        }
        catch (std::invalid_argument const&)
        {
            threw = true;
        }
        BEAST_EXPECT(threw);

        share = makeShare();
        share.committeePosition = ExportLimits::maxCommitteeMembers;
        BEAST_EXPECT(!share.validShape());

        share = makeShare();
        share.signature = Buffer{73};
        BEAST_EXPECT(!share.validShape());

        share = makeShare();
        share.signature = Buffer{4};
        BEAST_EXPECT(!share.validShape());
    }

    void
    run() override
    {
        testRoundTrip();
        testMalformed();
    }
};

BEAST_DEFINE_TESTSUITE(ExportShare, protocol, ripple);

}  // namespace test
}  // namespace ripple
