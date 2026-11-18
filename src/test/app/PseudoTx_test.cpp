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
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/tx/apply.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STAccount.h>
#include <string>
#include <vector>

namespace ripple {
namespace test {

struct PseudoTx_test : public beast::unit_test::suite
{
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
                    BEAST_EXPECT(!result.applied && result.ter == temINVALID);
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

    // Xahau pseudo-transactions reach submitTransaction from peers' proposed
    // tx sets: every validator's ttENTROPY each ledger, a ttCRON whenever a
    // cron fires, the ttUNL_REPORTs on flag ledgers. They must be dropped
    // before the validity check, which would log a warning for each and
    // cache its verdict.
    void
    testSubmitIgnoresXahauPseudo()
    {
        testcase("submitTransaction ignores Xahau pseudo-transactions");
        using namespace jtx;
        Env env(*this);
        auto const seq = env.closed()->seq() + 1;
        auto& router = env.app().getHashRouter();

        // The flags checkValidity caches: SF_SIGBAD and SF_LOCALBAD.
        auto verdict = [&](STTx const& stx) {
            env.app().getOPs().submitTransaction(
                std::make_shared<STTx const>(stx));
            return router.getFlags(stx.getTransactionID()) &
                (SF_PRIVATE1 | SF_PRIVATE3);
        };

        BEAST_EXPECT(!verdict(STTx(ttENTROPY, [&](auto& obj) {
            obj[sfLedgerSequence] = seq;
            obj.setFieldVL(sfPublicKey, Blob(33, 0xED));
            obj.setFieldH256(sfNextRandomDigest, uint256(1));
        })));
        BEAST_EXPECT(!verdict(STTx(
            ttUNL_REPORT, [&](auto& obj) { obj[sfLedgerSequence] = seq; })));

        auto const cron = [&](std::uint32_t owner) {
            return STTx(ttCRON, [&](auto& obj) {
                obj[sfLedgerSequence] = seq;
                obj[sfOwner] = AccountID(owner);
            });
        };
        BEAST_EXPECT(!verdict(cron(1)));

        // A ttCRON this node generated itself is marked the way TxQ::accept
        // marks it (SF_SIGGOOD), so it would get past the signature check and
        // fail the local checks instead.
        auto const mine = cron(2);
        router.setFlags(mine.getTransactionID(), SF_PRIVATE2);
        router.setFlags(mine.getTransactionID(), SF_EMITTED);
        BEAST_EXPECT(!verdict(mine));

        // Upstream pseudo-transactions keep their rippled behaviour.
        for (auto const& stx : getPseudoTxs(env.closed()->rules(), seq))
            BEAST_EXPECT(verdict(stx));
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
        testSubmitIgnoresXahauPseudo();
    }
};

BEAST_DEFINE_TESTSUITE(PseudoTx, app, ripple);

}  // namespace test
}  // namespace ripple
