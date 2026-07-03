//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2017 Ripple Labs Inc.
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

#include <test/jtx.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STAccount.h>
#include <xrpl/protocol/Sign.h>
#include <string>
#include <vector>

namespace ripple {
namespace test {

struct PseudoTx_test : public beast::unit_test::suite
{
    STTx
    consensusEntropyTx(
        std::uint32_t seq,
        std::uint8_t tier,
        std::uint16_t count,
        std::uint16_t denominator,
        Blob contributors)
    {
        return STTx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID());
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setFieldH256(sfDigest, uint256(3));
            obj.setFieldU16(sfEntropyCount, count);
            obj.setFieldU16(sfEntropyDenominator, denominator);
            obj.setFieldVL(sfEntropyContributors, contributors);
            obj.setFieldU8(sfEntropyTier, tier);
        });
    }

    std::vector<STTx>
    getPseudoTxs(Rules const& rules, std::uint32_t seq)
    {
        std::vector<STTx> res;

        res.emplace_back(STTx(ttFEE, [&](auto& obj) {
            obj[sfAccount] = AccountID();
            obj[sfLedgerSequence] = seq;
            if (rules.enabled(featureXRPFees))
            {
                obj[sfBaseFeeDrops] = XRPAmount{0};
                obj[sfReserveBaseDrops] = XRPAmount{0};
                obj[sfReserveIncrementDrops] = XRPAmount{0};
            }
            else
            {
                obj[sfBaseFee] = 0;
                obj[sfReserveBase] = 0;
                obj[sfReserveIncrement] = 0;
                obj[sfReferenceFeeUnits] = 0;
            }
        }));

        res.emplace_back(STTx(ttAMENDMENT, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID());
            obj.setFieldH256(sfAmendment, uint256(2));
            obj.setFieldU32(sfLedgerSequence, seq);
        }));

        res.emplace_back(consensusEntropyTx(
            seq, entropyTierValidatorQuorum, 1, 1, Blob{0x01}));

        auto const secret = generateSecretKey(KeyType::secp256k1, randomSeed());
        auto const publicKey = derivePublicKey(KeyType::secp256k1, secret);
        ExportResultBuilder::SignatureSnapshot signatures;
        std::uint8_t const signatureBytes[] = {1, 2, 3};
        signatures.emplace(
            publicKey, Buffer{signatureBytes, sizeof(signatureBytes)});
        res.emplace_back(ExportResultBuilder::buildSignatureWitness(
            uint256(4), signatures, seq));

        return res;
    }

    std::vector<STTx>
    getRealTxs()
    {
        std::vector<STTx> res;

        res.emplace_back(STTx(
            ttACCOUNT_SET, [&](auto& obj) { obj[sfAccount] = AccountID(1); }));

        res.emplace_back(STTx(ttPAYMENT, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID(2));
            obj.setAccountID(sfDestination, AccountID(3));
        }));

        return res;
    }

    void
    testPrevented(FeatureBitset features)
    {
        using namespace jtx;
        Env env(*this, features);

        for (auto const& stx :
             getPseudoTxs(env.closed()->rules(), env.closed()->seq() + 1))
        {
            std::string reason;
            BEAST_EXPECT(isPseudoTx(stx));
            BEAST_EXPECT(!passesLocalChecks(stx, reason));
            BEAST_EXPECT(reason == "Cannot submit pseudo transactions.");
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) {
                    auto const result =
                        ripple::apply(env.app(), view, stx, tapNONE, j);
                    BEAST_EXPECT(!result.applied);
                    return result.applied;
                });
        }
    }

    void
    testAllowed()
    {
        for (auto const& stx : getRealTxs())
        {
            std::string reason;
            BEAST_EXPECT(!isPseudoTx(stx));
            BEAST_EXPECT(passesLocalChecks(stx, reason));
        }
    }

    void
    expectOpenLedgerResult(jtx::Env& env, STTx const& tx, TER expected)
    {
        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            auto const result = ripple::apply(env.app(), view, tx, tapNONE, j);
            BEAST_EXPECT(result.ter == expected);
            BEAST_EXPECT(!result.applied);
            return result.applied;
        });
    }

    void
    testConsensusEntropyContributorMaskPreflight()
    {
        testcase("ConsensusEntropy contributor mask preflight");

        using namespace jtx;
        Env env(*this, supported_amendments() | featureConsensusEntropy);
        auto const seq = env.closed()->seq() + 1;

        expectOpenLedgerResult(
            env,
            consensusEntropyTx(seq, entropyTierConsensusFallback, 0, 0, Blob{}),
            temINVALID);
        expectOpenLedgerResult(
            env,
            consensusEntropyTx(
                seq, entropyTierValidatorQuorum, 2, 3, Blob{0x03}),
            temINVALID);

        expectOpenLedgerResult(
            env,
            consensusEntropyTx(
                seq, entropyTierConsensusFallback, 1, 1, Blob{0x01}),
            temMALFORMED);
        expectOpenLedgerResult(
            env,
            consensusEntropyTx(
                seq, entropyTierValidatorQuorum, 1, 9, Blob{0x01}),
            temMALFORMED);
        expectOpenLedgerResult(
            env,
            consensusEntropyTx(
                seq, entropyTierValidatorQuorum, 1, 9, Blob{0x01, 0x02}),
            temMALFORMED);
        expectOpenLedgerResult(
            env,
            consensusEntropyTx(
                seq, entropyTierValidatorQuorum, 2, 3, Blob{0x01}),
            temMALFORMED);
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        FeatureBitset const xrpFees{featureXRPFees};

        testPrevented(all - featureXRPFees);
        testPrevented(all);
        testAllowed();
        testConsensusEntropyContributorMaskPreflight();
    }
};

BEAST_DEFINE_TESTSUITE(PseudoTx, app, ripple);

}  // namespace test
}  // namespace ripple
