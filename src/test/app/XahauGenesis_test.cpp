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

        // RH TODO:
        // check hookdefinitions
        // check hookparameters
        // check wasm hash
        // check gensis hooks array
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
