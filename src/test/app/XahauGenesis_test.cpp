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

#include <ripple/app/tx/apply.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/tx/impl/XahauGenesis.h>
#include <string>
#include <test/jtx.h>
#include <vector>

using namespace XahauGenesis;

namespace ripple {
namespace test {

struct XahauGenesis_test : public beast::unit_test::suite
{
    void
    testActivation()
    {
        testcase("Test activation");
        using namespace jtx;
        Env env{*this, envconfig(), supported_amendments() - featureXahauGenesis, nullptr,
            beast::severities::kTrace
        };

        auto isEnabled = [&](void)->bool
        {
            auto const obj = env.le(keylet::amendments());
            if (!obj)
                return false;
            STVector256 amendments = obj->getFieldV256(sfAmendments);
            return
                std::find(amendments.begin(), amendments.end(), featureXahauGenesis) != amendments.end();
        };

        BEAST_EXPECT(!isEnabled());

        // insert a ttAMENDMENT pseudo into the open ledger
        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal j) -> bool {

                STTx tx (ttAMENDMENT, [&](auto& obj) {
                    obj.setAccountID(sfAccount, AccountID());
                    obj.setFieldH256(sfAmendment, featureXahauGenesis);
                    obj.setFieldU32(sfLedgerSequence, env.app().getLedgerMaster().getClosedLedger()->info().seq + 1);
                });

                uint256 txID = tx.getTransactionID();
                auto s = std::make_shared<ripple::Serializer>();
                tx.add(*s);
                env.app().getHashRouter().setFlags(txID, SF_PRIVATE2);
                view.rawTxInsert(txID, std::move(s), nullptr);

                return true;
            });

        // close the ledger
        env.close();

        BEAST_EXPECT(isEnabled());

        // sum the initial distribution balances, these should equal total coins in the closed ledger

        XRPAmount total { GenesisAmount };
        for (auto const& [node, amt] : Distribution)
        {
            std::optional<AccountID> id;
            if (node.c_str()[0] == 'r')
                id = parseBase58<AccountID>(node);
            else
            {
                auto const pk = parseBase58<PublicKey>(TokenType::NodePublic, node);
                if (pk)
                    id = calcAccountID(*pk);
            }

            BEAST_EXPECT(!!id);

            auto const root = env.le(keylet::account(*id));

            BEAST_EXPECT(root);

            BEAST_EXPECT(root->getFieldAmount(sfBalance).xrp() == amt);

            total += amt;
        }

        BEAST_EXPECT(env.app().getLedgerMaster().getClosedLedger()->info().drops == total);

        // ensure the definitions are correctly set
        {
            auto const govHash = ripple::sha512Half_s(ripple::Slice(GovernanceHook.data(), GovernanceHook.size()));
            auto const govKL = keylet::hookDefinition(govHash);
            auto govSLE = env.le(govKL);

            std::cout << "01\n";
            BEAST_EXPECT(!!govSLE);
            std::cout << "02\n";
            BEAST_EXPECT(govSLE->getFieldH256(sfHookHash) == govHash);

            auto const govVL = govSLE->getFieldVL(sfCreateCode);
            std::cout << "03\n";
            BEAST_EXPECT(govHash == ripple::sha512Half_s(ripple::Slice(govVL.data(), govVL.size())));
            std::cout << "04\n";
            BEAST_EXPECT(govSLE->getFieldU64(sfReferenceCount) == 1);
            std::cout << "05\n";
            BEAST_EXPECT(govSLE->getFieldH256(sfHookOn) ==
                ripple::uint256("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFBFFFFF"));
            std::cout << "06\n";
            BEAST_EXPECT(govSLE->getFieldH256(sfHookNamespace) ==
                ripple::uint256("0000000000000000000000000000000000000000000000000000000000000001"));
            std::cout << "07\n";
            BEAST_EXPECT(govSLE->getFieldU16(sfHookApiVersion) == 0);
            auto const govFee = govSLE->getFieldAmount(sfFee);

            std::cout << "08\n";
            BEAST_EXPECT(isXRP(govFee) && govFee > beast::zero);
            std::cout << "09\n";
            BEAST_EXPECT(!govSLE->isFieldPresent(sfHookCallbackFee));
            std::cout << "10\n";
            BEAST_EXPECT(govSLE->getFieldH256(sfHookSetTxnID) != beast::zero);

            auto const rwdHash = ripple::sha512Half_s(ripple::Slice(RewardHook.data(), RewardHook.size()));
            auto const rwdKL = keylet::hookDefinition(rwdHash);
            auto rwdSLE = env.le(rwdKL);

            std::cout << "11\n";
            BEAST_EXPECT(!!rwdSLE);
            std::cout << "12\n";
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookHash) == rwdHash);

            auto const rwdVL = rwdSLE->getFieldVL(sfCreateCode);
            std::cout << "13\n";
            BEAST_EXPECT(rwdHash == ripple::sha512Half_s(ripple::Slice(rwdVL.data(), rwdVL.size())));
            std::cout << "14\n";
            BEAST_EXPECT(rwdSLE->getFieldU64(sfReferenceCount) == 1);
            std::cout << "15\n";
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookOn) ==
                ripple::uint256("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFFFFFFFFFFFFFFFBFFFFF"));
            std::cout << "16\n";
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookNamespace) ==
                ripple::uint256("0000000000000000000000000000000000000000000000000000000000000010"));
            std::cout << "17\n";
            BEAST_EXPECT(rwdSLE->getFieldU16(sfHookApiVersion) == 0);
            auto const rwdFee = rwdSLE->getFieldAmount(sfFee);
            std::cout << "18\n";
            BEAST_EXPECT(isXRP(rwdFee) && rwdFee > beast::zero);
            std::cout << "19\n";
            BEAST_EXPECT(!rwdSLE->isFieldPresent(sfHookCallbackFee));
            std::cout << "20\n";
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookSetTxnID) != beast::zero);
            std::cout << "21\n";
        }

        // RH TODO:
        // check hookparameters
        // check wasm hash
        // check gensis hooks array
        // check start ledger
    }

    void
    run() override
    {
        using namespace test::jtx;
        testActivation();
    }
};

BEAST_DEFINE_TESTSUITE(XahauGenesis, app, ripple);

}  // namespace test
}  // namespace ripple
