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

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

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
            keylet::exportLatch(fixtureAccount, fixtureOrigin).key == expected);

        auto const firstAccount =
            calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const secondAccount =
            calcAccountID(randomKeyPair(KeyType::secp256k1).first);
        auto const firstOrigin = makeOrigin("first-export-origin");
        auto const secondOrigin = makeOrigin("second-export-origin");

        auto const key = keylet::exportLatch(firstAccount, firstOrigin);
        BEAST_EXPECT(key.type == ltEXPORT_LATCH);
        BEAST_EXPECT(
            key.key == keylet::exportLatch(firstAccount, firstOrigin).key);
        BEAST_EXPECT(
            key.key != keylet::exportLatch(firstAccount, secondOrigin).key);
        BEAST_EXPECT(
            key.key != keylet::exportLatch(secondAccount, firstOrigin).key);
    }

    void
    testPendingDirectory()
    {
        testcase("pending Export directory");

        uint256 const expected{
            "70D4A5FF7B4087C41414E71BD1FF2ABE"
            "5F49C380C158ACDCC76254279B176018"};
        auto const& pending = keylet::pendingExports();

        BEAST_EXPECT(pending.type == ltDIR_NODE);
        BEAST_EXPECT(pending.key == expected);
        BEAST_EXPECT(&pending == &keylet::pendingExports());
    }

    void
    testStructuralLimits()
    {
        testcase("structural Export limits");

        BEAST_EXPECT(ExportLimits::maxCommitteeMembers == 32);
        BEAST_EXPECT(ExportLimits::maxCommitteeRosterBytes == 1056);
        BEAST_EXPECT(ExportLimits::maxCommitteeContributorBytes == 4);
        BEAST_EXPECT(
            ExportLimits::maxCommitteeMembers == STTx::maxMultiSigners());

        std::array<std::size_t, 11> constexpr expected{
            0, 1, 2, 3, 4, 4, 5, 6, 7, 8, 8};
        for (std::size_t members = 0; members < expected.size(); ++members)
            BEAST_EXPECT(
                ExportLimits::committeeQuorumThreshold(members) ==
                expected[members]);
        BEAST_EXPECT(ExportLimits::committeeQuorumThreshold(28) == 23);
        BEAST_EXPECT(ExportLimits::committeeQuorumThreshold(32) == 26);
    }

    void
    testCommitteeProfile()
    {
        testcase("Export committee profile");

        std::vector<PublicKey> masters;
        masters.reserve(28);
        for (std::size_t i = 0; i < 28; ++i)
            masters.push_back(randomKeyPair(KeyType::secp256k1).first);
        auto const roster = serializeExportCommittee(masters);

        auto profile = resolveExportCommittee(makeSlice(roster));
        BEAST_EXPECT(profile.has_value());
        if (profile)
        {
            BEAST_EXPECT(profile->members.size() == 28);
            BEAST_EXPECT(profile->quorum == 23);
            BEAST_EXPECT(profile->position(profile->members.front()) == 0);
            BEAST_EXPECT(profile->position(profile->members.back()) == 27);
        }
        BEAST_EXPECT(!exportCommitteeHash(makeSlice(roster)).isZero());

        Blob unordered;
        for (auto it = masters.rbegin(); it != masters.rend(); ++it)
            unordered.insert(unordered.end(), it->begin(), it->end());
        BEAST_EXPECT(!resolveExportCommittee(makeSlice(unordered)));
        auto const canonical =
            canonicalizeExportCommittee(makeSlice(unordered));
        BEAST_EXPECT(canonical.has_value());
        if (canonical)
        {
            BEAST_EXPECT(*canonical == roster);
            auto const unorderedProfile =
                resolveExportCommittee(makeSlice(*canonical));
            BEAST_EXPECT(unorderedProfile.has_value());
            if (unorderedProfile)
                BEAST_EXPECT(unorderedProfile->members == profile->members);
            BEAST_EXPECT(
                exportCommitteeHash(makeSlice(*canonical)) ==
                exportCommitteeHash(makeSlice(roster)));
        }

        Serializer expectedDigest;
        expectedDigest.add32(HashPrefix::exportCommittee);
        expectedDigest.add32(static_cast<std::uint32_t>(masters.size()));
        expectedDigest.addRaw(makeSlice(roster));
        BEAST_EXPECT(
            exportCommitteeHash(makeSlice(roster)) ==
            expectedDigest.getSHA512Half());

        BEAST_EXPECT(!resolveExportCommittee(Slice{}));

        Blob malformed(34, 0);
        BEAST_EXPECT(!resolveExportCommittee(makeSlice(malformed)));

        Blob duplicate;
        duplicate.insert(
            duplicate.end(), masters.front().begin(), masters.front().end());
        duplicate.insert(
            duplicate.end(), masters.front().begin(), masters.front().end());
        BEAST_EXPECT(!resolveExportCommittee(makeSlice(duplicate)));

        std::vector<PublicKey> tooMany;
        for (std::size_t i = 0; i <= ExportLimits::maxCommitteeMembers; ++i)
            tooMany.push_back(randomKeyPair(KeyType::secp256k1).first);
        BEAST_EXPECT(serializeExportCommittee(std::move(tooMany)).empty());
    }

    void
    testExportLatchFormat()
    {
        testcase("Export latch format");

        BEAST_EXPECT(sfExportCount.fieldCode == field_code(STI_UINT16, 101));
        BEAST_EXPECT(sfExportNode.fieldCode == field_code(STI_UINT64, 29));
        BEAST_EXPECT(
            sfExportCommitteeHash.fieldCode == field_code(STI_UINT256, 39));
        BEAST_EXPECT(sfExportCommittee.fieldCode == field_code(STI_VL, 34));

        AccountID const account{1};
        uint256 const origin{2};
        uint256 const intentDigest{3};
        uint256 const committeeDigest{4};
        uint256 const witnessHash{5};

        SLE latch{keylet::exportLatch(account, origin)};
        latch.setAccountID(sfAccount, account);
        latch.setFieldU32(sfTicketSequence, 55'001);
        latch.setFieldH256(sfTransactionHash, origin);
        latch.setFieldH256(sfDigest, intentDigest);
        latch.setFieldU32(sfLedgerSequence, 4'123'200);
        latch.setFieldH256(sfExportCommitteeHash, committeeDigest);
        latch.setFieldU32(sfFlags, 1);
        latch.setFieldH256(sfExportSignatureHash, witnessHash);
        latch.setFieldU64(sfOwnerNode, 7);
        latch.setFieldU64(sfExportNode, 8);

        auto const serialized = latch.getSerializer();

        SerialIter sit{serialized.slice()};
        SLE const parsed{sit, latch.key()};
        BEAST_EXPECT(parsed.getType() == ltEXPORT_LATCH);
        BEAST_EXPECT(parsed.getAccountID(sfAccount) == account);
        BEAST_EXPECT(parsed.getFieldU32(sfTicketSequence) == 55'001);
        BEAST_EXPECT(parsed.getFieldH256(sfTransactionHash) == origin);
        BEAST_EXPECT(parsed.getFieldH256(sfDigest) == intentDigest);
        BEAST_EXPECT(parsed.getFieldU32(sfLedgerSequence) == 4'123'200);
        BEAST_EXPECT(
            parsed.getFieldH256(sfExportCommitteeHash) == committeeDigest);
        BEAST_EXPECT(parsed.getFieldU32(sfFlags) == 1);
        BEAST_EXPECT(parsed.getFieldH256(sfExportSignatureHash) == witnessHash);
        BEAST_EXPECT(parsed.getFieldU64(sfOwnerNode) == 7);
        BEAST_EXPECT(parsed.getFieldU64(sfExportNode) == 8);

        SLE accountRoot{keylet::account(account)};
        accountRoot.setFieldU16(sfExportCount, 9);
        BEAST_EXPECT(accountRoot.getFieldU16(sfExportCount) == 9);

        SLE pendingRoot{keylet::pendingExports()};
        pendingRoot.setFieldU16(sfExportCount, 10);
        BEAST_EXPECT(pendingRoot.getFieldU16(sfExportCount) == 10);

        auto const master = randomKeyPair(KeyType::secp256k1).first;
        auto const roster = serializeExportCommittee({master});
        auto const digest = exportCommitteeHash(makeSlice(roster));
        auto const committeeKey = keylet::exportCommittee(account, digest);
        BEAST_EXPECT(committeeKey.type == ltEXPORT_COMMITTEE);

        SLE committee{committeeKey};
        committee.setAccountID(sfAccount, account);
        committee.setFieldH256(sfExportCommitteeHash, digest);
        committee.setFieldVL(sfExportCommittee, roster);
        committee.setFieldU64(sfOwnerNode, 11);
        auto const committeeBytes = committee.getSerializer();
        SerialIter committeeIter{committeeBytes.slice()};
        SLE const parsedCommittee{committeeIter, committeeKey.key};
        BEAST_EXPECT(parsedCommittee.getType() == ltEXPORT_COMMITTEE);
        BEAST_EXPECT(parsedCommittee.getFieldVL(sfExportCommittee) == roster);
    }

    void
    run() override
    {
        testOriginIdentity();
        testPendingDirectory();
        testStructuralLimits();
        testCommitteeProfile();
        testExportLatchFormat();
    }
};

BEAST_DEFINE_TESTSUITE(ExportKeylet, protocol, ripple);

}  // namespace test
}  // namespace ripple
