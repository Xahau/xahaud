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


    AccountID const genesisAccID = calcAccountID(
    generateKeyPair(KeyType::secp256k1, generateSeed("masterpassphrase"))
        .first);

    // the test cases in this test suite are based on changing the state of the ledger before
    // xahaugenesis is activated, to do this they call this templated function with an "execute-first" lambda
    void
    activate(jtx::Env& env, bool burnedViaTest = false, bool skipTests = false, bool testFlag = false)
    {
        using namespace jtx;

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

        uint32_t const startLgr = env.app().getLedgerMaster().getClosedLedger()->info().seq + 1;

        // insert a ttAMENDMENT pseudo into the open ledger
        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal j) -> bool
            {
                STTx tx (ttAMENDMENT, [&](auto& obj) {
                    obj.setAccountID(sfAccount, AccountID());
                    obj.setFieldH256(sfAmendment, featureXahauGenesis);
                    obj.setFieldU32(sfLedgerSequence, startLgr);
                    if (testFlag)
                        obj.setFieldU32(sfFlags, tfTestSuite);
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

        if (skipTests)
            return;

        // sum the initial distribution balances, these should equal total coins in the closed ledger

        XRPAmount total { GenesisAmount };
        for (auto const& [node, amt] : Distribution)
            total += amt;

        BEAST_EXPECT(burnedViaTest || env.app().getLedgerMaster().getClosedLedger()->info().drops == total);

        // is the hook array present
        auto genesisHooksLE = env.le(keylet::hook(genesisAccID));
        BEAST_REQUIRE(!!genesisHooksLE);
        auto genesisHookArray = genesisHooksLE->getFieldArray(sfHooks);
        BEAST_EXPECT(genesisHookArray.size() == 2);

        // make sure the account root exists and has the correct balance and ownercount
        auto genesisAccRoot = env.le(keylet::account(genesisAccID));
        BEAST_REQUIRE(!!genesisAccRoot);
        BEAST_EXPECT(genesisAccRoot->getFieldAmount(sfBalance) == XahauGenesis::GenesisAmount);
        BEAST_EXPECT(genesisAccRoot->getFieldU32(sfOwnerCount) == 2);

        // ensure the definitions are correctly set
        {
            auto const govHash = ripple::sha512Half_s(ripple::Slice(GovernanceHook.data(), GovernanceHook.size()));
            auto const govKL = keylet::hookDefinition(govHash);
            auto govSLE = env.le(govKL);

            BEAST_EXPECT(!!govSLE);
            BEAST_EXPECT(govSLE->getFieldH256(sfHookHash) == govHash);

            auto const govVL = govSLE->getFieldVL(sfCreateCode);
            BEAST_EXPECT(govHash == ripple::sha512Half_s(ripple::Slice(govVL.data(), govVL.size())));
            BEAST_EXPECT(govSLE->getFieldU64(sfReferenceCount) == 1);
            BEAST_EXPECT(govSLE->getFieldH256(sfHookOn) ==
                ripple::uint256("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFBFFFFF"));
            BEAST_EXPECT(govSLE->getFieldH256(sfHookNamespace) ==
                ripple::uint256("0000000000000000000000000000000000000000000000000000000000000000"));
            BEAST_EXPECT(govSLE->getFieldU16(sfHookApiVersion) == 0);
            auto const govFee = govSLE->getFieldAmount(sfFee);

            BEAST_EXPECT(isXRP(govFee) && govFee > beast::zero);
            BEAST_EXPECT(!govSLE->isFieldPresent(sfHookCallbackFee));
            BEAST_EXPECT(govSLE->getFieldH256(sfHookSetTxnID) != beast::zero);

            BEAST_EXPECT(genesisHookArray[0].getFieldH256(sfHookHash) == govHash);


            auto const rwdHash = ripple::sha512Half_s(ripple::Slice(RewardHook.data(), RewardHook.size()));
            auto const rwdKL = keylet::hookDefinition(rwdHash);
            auto rwdSLE = env.le(rwdKL);

            BEAST_EXPECT(!!rwdSLE);
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookHash) == rwdHash);

            auto const rwdVL = rwdSLE->getFieldVL(sfCreateCode);
            BEAST_EXPECT(rwdHash == ripple::sha512Half_s(ripple::Slice(rwdVL.data(), rwdVL.size())));
            BEAST_EXPECT(rwdSLE->getFieldU64(sfReferenceCount) == 1);
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookOn) ==
                ripple::uint256("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFFFFFFFFFFFFFFFBFFFFF"));
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookNamespace) ==
                ripple::uint256("0000000000000000000000000000000000000000000000000000000000000000"));
            BEAST_EXPECT(rwdSLE->getFieldU16(sfHookApiVersion) == 0);
            auto const rwdFee = rwdSLE->getFieldAmount(sfFee);
            BEAST_EXPECT(isXRP(rwdFee) && rwdFee > beast::zero);
            BEAST_EXPECT(!rwdSLE->isFieldPresent(sfHookCallbackFee));
            BEAST_EXPECT(rwdSLE->getFieldH256(sfHookSetTxnID) != beast::zero);

            BEAST_EXPECT(genesisHookArray[1].getFieldH256(sfHookHash) == rwdHash);
        }

        // check distribution amounts and hook parameters
        {
            uint8_t member_count = 0;
            std::map<std::vector<uint8_t>, std::vector<uint8_t>> params = XahauGenesis::GovernanceParameters;
            for (auto const& [rn, x]: XahauGenesis::Distribution)
            {
                const char first = rn.c_str()[0];
                BEAST_EXPECT(
                    (first == 'r' &&
                    !!parseBase58<AccountID>(rn)) ||
                    first == 'n' &&
                    !!parseBase58<PublicKey>(TokenType::NodePublic, rn));

                if (first == 'r')
                {
                    auto acc = env.le(keylet::account(*parseBase58<AccountID>(rn)));
                    BEAST_EXPECT(!!acc);
                    auto bal = acc->getFieldAmount(sfBalance);
                    BEAST_EXPECT(bal == STAmount(x));
                    continue;
                }

                // node based addresses
                auto const pk = parseBase58<PublicKey>(TokenType::NodePublic, rn);
                BEAST_EXPECT(!!pk);

                AccountID id = calcAccountID(*pk);
                auto acc = env.le(keylet::account(id));
                BEAST_EXPECT(!!acc);
                auto bal = acc->getFieldAmount(sfBalance);
                BEAST_EXPECT(bal == STAmount(x));

                // initial member enumeration
                params.emplace(
                    std::vector<uint8_t>{'I', 'S', member_count++},
                    std::vector<uint8_t>(id.data(), id.data() + 20));
            }

            // initial member count
            params.emplace(
                std::vector<uint8_t>{'I', 'M', 'C'},
                std::vector<uint8_t>{member_count});

            // check parameters
            auto leParams = genesisHookArray[0].getFieldArray(sfHookParameters);
            BEAST_EXPECT(leParams.size() == params.size());

            // these should be recorded in the same order
            std::set<std::vector<uint8_t>> keys_used;
            for (auto& param : leParams)
            {
                auto key = param.getFieldVL(sfHookParameterName);
                auto val = param.getFieldVL(sfHookParameterValue);

                // no duplicates allowed
                BEAST_EXPECT(keys_used.find(key) == keys_used.end());

                // should be in our precomputed params
                BEAST_EXPECT(params.find(key) != params.end());

                // value should match
                BEAST_EXPECT(params[key] == val);

                // add key to used set
                keys_used.emplace(key);
            }
        }

        // check fees object correctly recordsed activation seq
        auto fees = env.le(keylet::fees());
        BEAST_REQUIRE(!!fees);
        BEAST_EXPECT(fees->isFieldPresent(sfXahauActivationLgrSeq) &&
            fees->getFieldU32(sfXahauActivationLgrSeq) == startLgr);

        // ensure no signerlist
        BEAST_EXPECT(!env.le(keylet::signers(genesisAccID)));

        // ensure correctly blackholed
        BEAST_EXPECT(genesisAccRoot->isFieldPresent(sfRegularKey) &&
            genesisAccRoot->getAccountID(sfRegularKey) == noAccount() &&
            genesisAccRoot->getFieldU32(sfFlags) & lsfDisableMaster);


    }

    void
    testPlainActivation()
    {
        testcase("Test activation");
        using namespace jtx;
        Env env{*this, envconfig(), supported_amendments() - featureXahauGenesis, nullptr,
            beast::severities::kWarning
            //beast::severities::kTrace
        };

        activate(env);
    }

    void
    testWithSignerList()
    {
        using namespace jtx;
        testcase("Test signerlist");
        Env env{*this, envconfig(), supported_amendments() - featureXahauGenesis, nullptr,
            beast::severities::kWarning
            //beast::severities::kTrace
        };

        Account const alice{"alice", KeyType::ed25519};
        env.fund(XRP(1000), alice);
        env.memoize(env.master);
        env(signers(env.master, 1, {{alice, 1}}));
        env.close();

        activate(env, true);
    }

    void
    testWithRegularKey()
    {
        using namespace jtx;
        testcase("Test regkey");
        Env env{*this, envconfig(), supported_amendments() - featureXahauGenesis, nullptr,
            beast::severities::kWarning
            //beast::severities::kTrace
        };

        env.memoize(env.master);
        Account const alice("alice");
        env.fund(XRP(10000), alice);
        env(regkey(env.master, alice));
        env.close();

        activate(env, true);
    }


    void
    setupGov(jtx::Env& env, std::vector<AccountID> const members)
    {
        using namespace jtx;

        auto const invoker = Account("invoker");
        env.fund(XRP(10000), invoker);
        env.close();

        XahauGenesis::TestDistribution.clear();

        for (auto& m: members)
        {
            std::string acc = toBase58(m);
            XahauGenesis::TestDistribution[acc] =  XRPAmount(10000);
        }

        activate(env, true, true, true);

        XahauGenesis::TestDistribution.clear();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = invoker.human();
        invoke[jss::Destination] = env.master.human();
        env(invoke, fee(XRP(1)));
        env.close();
    }


    void
    testGovernance()
    {

        using namespace jtx;
        testcase("Test governance hook");

        Env env{*this, envconfig(), supported_amendments() - featureXahauGenesis, nullptr,
            beast::severities::kTrace
        };
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const david = Account("david");
        auto const edward = Account("edward");

        env.fund(XRP(10000), alice, bob, carol, david, edward);

        setupGov(env, {alice.id(), bob.id(), carol.id(), david.id(), edward.id()});

        auto vote = [](uint8_t topic1, 
            std::optional<uint8_t> topic2,
            std::vector<uint8_t> data,
            std::optional<uint8_t> layer,
            Json::Value& txn)
        {
            auto charToHex = [](uint8_t inp) -> std::string 
            {
                std::string s;
                s.reserve(2);
                s += "0123456789ABCDEF"[inp >> 4];
                s += "0123456789ABCDEF"[inp >> 0];
                return s;
            };

            txn[jss::HookParameters] = Json::arrayValue;
            txn[jss::HookParameters][0u][jss::HookParameter] = Json::objectValue;
            txn[jss::HookParameters][0u][jss::HookParameter][jss::HookParameterName] =
                "54"; // 'T'
            txn[jss::HookParameters][0u][jss::HookParameter][jss::HookParameterValue] =
                charToHex(topic1) + (topic2 ? charToHex(*topic2) : "");

            txn[jss::HookParameters][1u][jss::HookParameter][jss::HookParameterName] = 
                "56"; // 'V'

            std::string strData;
            strData.reserve(data.size() << 1U);
            for (uint8_t c : data)
                strData += charToHex(c);

            txn[jss::HookParameters][1u][jss::HookParameter][jss::HookParameterValue] = strData;


            if (layer)
            {
                txn[jss::HookParameters][2u][jss::HookParameter][jss::HookParameterName] = "4C";
                txn[jss::HookParameters][2u][jss::HookParameter][jss::HookParameterValue] = charToHex(*layer);
            }
        };




        BEAST_EXPECT(true);
    }



//        auto hooksArray =

        // RH TODO:


        // governance hook tests:
        //  last member tries to remove themselves
        //  try to add more than 20 members
        //  add a member normally
        //  remove a member normally
        //  add a member normally then re-action by adding another vote
        //  add a member normally then ensure it's not undone by removing one of the votes
        //  remove a member normally then re-action by adding another vote
        //  remove a member normally then ensure it's not undone when removing one of the votes
        //  action a hook change
        //  action a hook change to a non-existent hook
        //  action a reward rate change
        //  action a reward delay change
        //  L2 versions of all of the above



        // reward hook tests:
        //  test claim reward before time
        //  test claim reward after time
        //  test claim reward when UNL report empty
        //  test claim reward when UNL report full

        // genesis mint tests:
        //  test send from non-genesis account emitted txn
        //  test send from non-genesis account non-emitted txn
        //  test send from genesis account emitted txn
        //  test send from genesis account non-emitted txn
        //  test send to account that doesn't exist
        //  test send an overflow amount
        //  test set governance flags
        //  test no-destinations specified

        // unl report test:
        //  test several validators all get on the list
        //  test several validators all vote different vl import keys
        //  test badly behaved validators dont get on the list
        //  test no validators on list
        //  test whole unl on list

        // account counter
        //  test import created accounts get a sequence
        //  test payment created accounts get a sequence
        //  test genesis mint created accounts get a sequence
        //  test rpc

    void
    run() override
    {
        using namespace test::jtx;
        testPlainActivation();
        testWithSignerList();
        testWithRegularKey();
        testGovernance();
    }
};

BEAST_DEFINE_TESTSUITE(XahauGenesis, app, ripple);

}  // namespace test
}  // namespace ripple
