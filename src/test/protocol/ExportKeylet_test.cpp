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
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/ValidatorBitset.h>
#include <xrpl/protocol/digest.h>

#include <array>
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

        BEAST_EXPECT(ExportLimits::maxValidatorUniverseMembers == 256);
        BEAST_EXPECT(
            ExportLimits::maxCommitteeMaskBytes ==
            validatorBitsetBytes(ExportLimits::maxValidatorUniverseMembers));
        BEAST_EXPECT(ExportLimits::maxCommitteeMaskBytes == 32);
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
    testEnhancedLatchFormat()
    {
        testcase("enhanced Export latch format");

        BEAST_EXPECT(sfExportCount.fieldCode == field_code(STI_UINT16, 101));
        BEAST_EXPECT(sfExportNode.fieldCode == field_code(STI_UINT64, 29));
        BEAST_EXPECT(
            sfExportUniverseHash.fieldCode == field_code(STI_UINT256, 39));
        BEAST_EXPECT(sfExportCommittee.fieldCode == field_code(STI_VL, 34));

        AccountID const account{1};
        uint256 const origin{2};
        uint256 const intentDigest{3};
        uint256 const universeDigest{4};
        uint256 const witnessHash{5};
        Blob committee(ExportLimits::maxCommitteeMaskBytes, 0);
        committee.front() = 0x03;

        SLE latch{keylet::shadowTicket(account, origin)};
        latch.setAccountID(sfAccount, account);
        latch.setFieldU32(sfTicketSequence, 55'001);
        latch.setFieldH256(sfTransactionHash, origin);
        latch.setFieldH256(sfDigest, intentDigest);
        latch.setFieldU32(sfLedgerSequence, 4'123'200);
        latch.setFieldH256(sfExportUniverseHash, universeDigest);
        latch.setFieldVL(sfExportCommittee, committee);
        latch.setFieldU32(sfFlags, 1);
        latch.setFieldH256(sfExportSignatureHash, witnessHash);
        latch.setFieldU64(sfOwnerNode, 7);
        latch.setFieldU64(sfExportNode, 8);

        auto const serialized = latch.getSerializer();
        BEAST_EXPECT(serialized.size() == 230);
        BEAST_EXPECT(
            strHex(serialized.slice()) ==
            "115374220000000126003EEA4020290000D6D9"
            "340000000000000007301D0000000000000008"
            "530000000000000000000000000000000000000000000000000000000000000002"
            "501500000000000000000000000000000000000000000000000000000000000000"
            "03"
            "502600000000000000000000000000000000000000000000000000000000000000"
            "05"
            "502700000000000000000000000000000000000000000000000000000000000000"
            "04"
            "702220030000000000000000000000000000000000000000000000000000000000"
            "0000"
            "81140000000000000000000000000000000000000001");

        SerialIter sit{serialized.slice()};
        SLE const parsed{sit, latch.key()};
        BEAST_EXPECT(parsed.getType() == ltSHADOW_TICKET);
        BEAST_EXPECT(parsed.getAccountID(sfAccount) == account);
        BEAST_EXPECT(parsed.getFieldU32(sfTicketSequence) == 55'001);
        BEAST_EXPECT(parsed.getFieldH256(sfTransactionHash) == origin);
        BEAST_EXPECT(parsed.getFieldH256(sfDigest) == intentDigest);
        BEAST_EXPECT(parsed.getFieldU32(sfLedgerSequence) == 4'123'200);
        BEAST_EXPECT(
            parsed.getFieldH256(sfExportUniverseHash) == universeDigest);
        BEAST_EXPECT(parsed.getFieldVL(sfExportCommittee) == committee);
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
    }

    void
    run() override
    {
        testOriginIdentity();
        testLegacyKeySeparation();
        testPendingDirectory();
        testStructuralLimits();
        testEnhancedLatchFormat();
    }
};

BEAST_DEFINE_TESTSUITE(ExportKeylet, protocol, ripple);

}  // namespace test
}  // namespace ripple
