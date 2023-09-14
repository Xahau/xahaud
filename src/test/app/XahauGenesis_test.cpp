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
#include <ripple/protocol/Indexes.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/tx/impl/XahauGenesis.h>
#include <string>
#include <test/jtx.h>
#include <vector>

// Function to handle integer types
template<typename T>
std::string maybe_to_string(T val, std::enable_if_t<std::is_integral_v<T>, int> = 0) {
    return std::to_string(val);
}

// Overload to handle non-integer types
template<typename T>
std::string maybe_to_string(T val, std::enable_if_t<!std::is_integral_v<T>, int> = 0) {
    return val;
}

#define M(m) memo(maybe_to_string(m), "", "")

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
            std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> params =
                XahauGenesis::GovernanceParameters;
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
                    AccountID id = *parseBase58<AccountID>(rn);
                    auto acc = env.le(keylet::account(id));
                    BEAST_EXPECT(!!acc);
                    auto bal = acc->getFieldAmount(sfBalance);
                    BEAST_EXPECT(bal == STAmount(x));
                
                    params.emplace_back(
                        std::vector<uint8_t>{'I', 'S', member_count++},
                        std::vector<uint8_t>(id.data(), id.data() + 20));
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
                params.emplace_back(
                    std::vector<uint8_t>{'I', 'S', member_count++},
                    std::vector<uint8_t>(id.data(), id.data() + 20));
            }

            // initial member count
            params.emplace_back(
                std::vector<uint8_t>{'I', 'M', 'C'},
                std::vector<uint8_t>{member_count});

            // check parameters
            auto leParams = genesisHookArray[0].getFieldArray(sfHookParameters);

            BEAST_EXPECT(leParams.size() == params.size());

            // these should be recorded in the same order
            std::set<std::vector<uint8_t>> keys_used;
            int counter = 0;
            for (auto& param : leParams)
            {
                auto key = param.getFieldVL(sfHookParameterName);
                auto val = param.getFieldVL(sfHookParameterValue);

                // no duplicates allowed
                BEAST_EXPECT(keys_used.find(key) == keys_used.end());

                BEAST_EXPECT(counter < leParams.size());

                BEAST_EXPECT(params[counter].first == key);
                BEAST_EXPECT(params[counter].second == val);

                // add key to used set
                keys_used.emplace(key);
                counter++;
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
            XahauGenesis::TestDistribution.emplace_back(acc, XRPAmount(10000000000));
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


    inline
    static
    std::string
    charToHex(uint8_t inp)
    {
        std::string ret("00");
        ret.data()[0] = "0123456789ABCDEF"[inp >> 4];
        ret.data()[1] = "0123456789ABCDEF"[(inp >> 0) & 0xFU];
        return ret;
    }

    void
    testGovernanceL1()
    {

        using namespace jtx;
        testcase("Test governance membership voting L1");

        Env env{*this, envconfig(), supported_amendments() - featureXahauGenesis, nullptr,
            beast::severities::kTrace
        };
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const david = Account("david");
        auto const edward = Account("edward");

        auto const m6 = Account("m6");
        auto const m7 = Account("m7");
        auto const m8 = Account("m8");
        auto const m9 = Account("m9");
        auto const m10 = Account("m10");
        auto const m11 = Account("m11");
        auto const m12 = Account("m12");
        auto const m13 = Account("m13");
        auto const m14 = Account("m14");
        auto const m15 = Account("m15");
        auto const m16 = Account("m16");
        auto const m17 = Account("m17");
        auto const m18 = Account("m18");
        auto const m19 = Account("m19");
        auto const m20 = Account("m20");
        auto const m21 = Account("m21");

        auto vecFromAcc = [](Account const& acc) -> std::vector<uint8_t>
        {
            uint8_t const* data = acc.id().data();
            std::vector<uint8_t> ret(data, data+20);
            std::cout << "ret: `" << strHex(ret) << "`, size: " << ret.size() << "\n";
            return ret;
        };  

        env.fund(XRP(10000), alice, bob, carol, david, edward,
            m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21);

        env.close();
        
        std::vector<Account> initial_members {alice, bob, carol, david, edward};
        std::vector<AccountID> initial_members_ids { alice.id(), bob.id(), carol.id(), david.id(), edward.id() };

        setupGov(env, initial_members_ids);

        auto vote = [&](
            uint16_t lineno,
            Account const& acc,
            char topic1, 
            std::optional<char> topic2,
            std::vector<uint8_t> data,
            std::optional<uint8_t> layer = std::nullopt)
        {

            Json::Value txn (Json::objectValue);

            txn[jss::HookParameters] = Json::arrayValue;
            txn[jss::HookParameters][0u] = Json::objectValue;

            txn[jss::HookParameters][0u][jss::HookParameter] = Json::objectValue;
            txn[jss::HookParameters][0u][jss::HookParameter][jss::HookParameterName] =
                "54"; // 'T'
            std::string val = charToHex(topic1) + (topic2 ? charToHex(*topic2) : "");
            std::cout << "val: `" << val << "`\n";
            txn[jss::HookParameters][0u][jss::HookParameter][jss::HookParameterValue] = val;

            txn[jss::HookParameters][1u] = Json::objectValue;
            txn[jss::HookParameters][1u][jss::HookParameter][jss::HookParameterName] = 
                "56"; // 'V'

            {
                std::string strData;
                strData.reserve(data.size() << 1U);
                for (uint8_t c : data)
                    strData += charToHex(c);

                txn[jss::HookParameters][1u][jss::HookParameter][jss::HookParameterValue] = strData;
            }

            // add a line number for debugging
            txn[jss::HookParameters][2u] = Json::objectValue;
            txn[jss::HookParameters][2u][jss::HookParameter][jss::HookParameterName] = "44"; // D
            {
                std::string strData;
                strData += charToHex((uint8_t)(lineno >> 8U));
                strData += charToHex((uint8_t)(lineno & 0xFFU));
                txn[jss::HookParameters][2u][jss::HookParameter][jss::HookParameterValue] = strData;
            }            


            if (layer)
            {
                txn[jss::HookParameters][2u] = Json::objectValue;
                txn[jss::HookParameters][2u][jss::HookParameter][jss::HookParameterName] = "4C";
                txn[jss::HookParameters][2u][jss::HookParameter][jss::HookParameterValue] = charToHex(*layer);
            }

            txn[jss::Account] = acc.human();
            txn[jss::TransactionType] = "Invoke";
            txn[jss::Destination] = env.master.human(); 
            return txn;
        };

        auto makeStateKey =
            [&](char voteType, char topic1, char topic2, uint8_t layer, AccountID const& id) -> uint256
        {

            uint8_t data[32];

            memset(data, 0, 32);
            data[0] = voteType;
            data[1] = topic1;
            data[2] = topic2;
            data[3] = layer;

            for (int i = 0; i < 20; ++i)
                data[12 + i] = id.data()[i];

            return uint256::fromVoid(data);
        };


        auto doL1Vote = [&](uint64_t lineno, Account const& acc, char topic1, char topic2, 
                            std::vector<uint8_t> const& vote_data,
                            std::vector<uint8_t> const& old_data,
                            bool actioned = true, bool const shouldFail = false)
        {

            if (shouldFail)
                actioned = false;

            bool isZero = true;
            for (auto&  x: vote_data)
            if (x != 0)
            {
                isZero = false;
                break;
            }

            bool isOldDataZero = true;
            for (auto& x : old_data)
            if (x != 0)
            {
                isOldDataZero = false;
                break;
            }                    

            uint8_t const key[32] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 
                topic1 == 'S' ? 0 : topic1, topic2};
            // check actioning prior to vote
            {
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));
                std::cout 
                        << "topic vote precheck: " << lineno << "L\n" 
                        << "\tacc: " << acc.human() << " shouldAction: " << (actioned ? "true": "false")
                        << "\tshouldFail: " << (shouldFail ? "true": "false") << "\n"
                        << "\ttopic: " << topic1 << "," << (topic2 < 48 ? topic2 + '1' : topic2) << "\n"
                        << "\tisOldDataZero:  " << (isOldDataZero ? "true" : "false") << "\n"
                        << "\tisVoteDataZero: " << (isZero ? "true" : "false") << "\n"
                        << "\told_data: " << strHex(old_data) << "\n"
                        << "\tlgr_data: " << (!entry ? "doesn't exist" : strHex(entry->getFieldVL(sfHookStateData))) << "\n"
                        << "\tnew_data: " << strHex(vote_data) << "\n"
                        << "\t(isOldDataZero && !entry): " << (isOldDataZero && !entry ? "true" : "false") << "\n"
                        << "\t(entry && entry->getFieldVL(sfHookStateData) == old_data): " << 
                            (entry && entry->getFieldVL(sfHookStateData) == old_data ? "true" : "false") << "\n"
                        << ((isOldDataZero && !entry) ||
                            (entry && entry->getFieldVL(sfHookStateData) == old_data) ? "" : "\tfailed: ^^^\n");
                    
                BEAST_EXPECT((isOldDataZero && !entry) || 
                    (entry && entry->getFieldVL(sfHookStateData) == old_data));
            }    

            // perform and check vote
            {
                env(vote(lineno, acc, topic1, topic2, vote_data), M(lineno), fee(XRP(1)),
                    shouldFail ? ter(tecHOOK_REJECTED) : ter(tesSUCCESS));
                env.close();
                auto entry =
                    env.le(keylet::hookState(env.master.id(), makeStateKey('V', topic1, topic2, 1, acc.id()),
                        beast::zero));
                if (!shouldFail)
                {
                    BEAST_REQUIRE(!!entry);
                    auto lgr_data = entry->getFieldVL(sfHookStateData);
                    BEAST_EXPECT(lgr_data.size() == vote_data.size());
                    BEAST_EXPECT(lgr_data == vote_data);
                }
            }

            // check actioning
            if (!shouldFail)
            {
                // if the vote count isn't high enough it will be hte old value if it's high enough it will be the
                // new value
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));
            
                if (!actioned && isOldDataZero)
                {
                    BEAST_EXPECT(!entry || entry->getFieldVL(sfHookStateData) == old_data);
                    return;
                }

                if (actioned)
                {
                    if (isZero)
                        BEAST_EXPECT(!entry);
                    else
                    {
                        std::cout << "new data: " << strHex(vote_data) << "\n";
                        std::cout << "lgr data: " << 
                            (!entry ? "<doesn't exist>" : strHex(entry->getFieldVL(sfHookStateData))) << "\n";
                        BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == vote_data);
                    }
                }
                else
                {
                    if (!!entry)
                    {
                        std::cout << "old data: " << strHex(entry->getFieldVL(sfHookStateData)) << "\n";
                    }
                    BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == old_data);
                }
            }
        };
        
        // 100% vote for a different reward rate
        {
            // this will be the new reward rate
            std::vector<uint8_t> vote_data {0x00U,0x81U,0xC6U,0xA4U,0x7EU,0x8DU,0x43U,0x54U};
            
            // this is the default reward rate
            std::vector<uint8_t> const original_data {0x00U,0xE4U,0x61U,0xEEU,0x78U,0x90U,0x83U,0x54U};
            
            doL1Vote(__LINE__, alice, 'R', 'R', vote_data, original_data, false);                
            doL1Vote(__LINE__, bob, 'R', 'R', vote_data, original_data, false);
            doL1Vote(__LINE__, carol, 'R', 'R', vote_data, original_data, false);
            doL1Vote(__LINE__, david, 'R', 'R', vote_data, original_data, false);
            doL1Vote(__LINE__, edward, 'R', 'R', vote_data, original_data, true);

            // reverting a vote should not undo the action
            doL1Vote(__LINE__, carol, 'R', 'R', original_data, vote_data, false);

            // submitting a null vote should delete the vote, and should not undo the action
            std::vector<uint8_t> const null_data {0,0,0,0,0,0,0,0};
            doL1Vote(__LINE__, david, 'R', 'R', null_data, vote_data, false);
        }
        
        // 100% vote for a different reward delay
        {
            // this will be the new reward delay
            std::vector<uint8_t> vote_data {0x00U,0x80U,0xC6U,0xA4U,0x7EU,0x8DU,0x03U,0x55U};
            
            // this is the default reward delay
            std::vector<uint8_t> const original_data {0x00U,0x80U,0x6AU,0xACU,0xAFU,0x3CU,0x09U,0x56U};

            doL1Vote(__LINE__, edward, 'R', 'D', vote_data, original_data, false);                
            doL1Vote(__LINE__, david, 'R', 'D', vote_data, original_data, false);
            doL1Vote(__LINE__, carol, 'R', 'D', vote_data, original_data, false);
            doL1Vote(__LINE__, bob, 'R', 'D', vote_data, original_data, false);
            doL1Vote(__LINE__, alice, 'R', 'D', vote_data, original_data, true);

            // reverting a vote should not undo the action
            doL1Vote(__LINE__, david, 'R', 'D', original_data, vote_data, false);

            // submitting a null vote should delete the vote, and should not undo the action
            std::vector<uint8_t> const null_data {0,0,0,0,0,0,0,0};
            doL1Vote(__LINE__, alice, 'R', 'D', null_data, vote_data, false);
        }

        // 100% vote to install the accept hook at hook position 7
        // create a definition for accept hook first
        auto const acceptHookHash = 
                ripple::sha512Half_s(ripple::Slice(XahauGenesis::AcceptHook.data(), XahauGenesis::AcceptHook.size()));
        auto const governHookHash = 
                ripple::sha512Half_s(
                    ripple::Slice(XahauGenesis::GovernanceHook.data(), XahauGenesis::GovernanceHook.size()));
        auto const rewardHookHash = 
                ripple::sha512Half_s(ripple::Slice(XahauGenesis::RewardHook.data(), XahauGenesis::RewardHook.size()));
        {
            Json::Value tx (Json::objectValue);

            tx[jss::Account] = m21.human();
            tx[jss::TransactionType] = "SetHook";
            tx[jss::Hooks] = Json::arrayValue;
            tx[jss::Hooks][0u] = Json::objectValue;

            tx[jss::Hooks][0u][jss::Hook] = Json::objectValue;

            tx[jss::Hooks][0u][jss::Hook][jss::CreateCode] = strHex(XahauGenesis::AcceptHook);

            tx[jss::Hooks][0u][jss::Hook][jss::HookApiVersion] = "0";
            tx[jss::Hooks][0u][jss::Hook][jss::HookNamespace] =
                "0000000000000000000000000000000000000000000000000000000000000000";
            tx[jss::Hooks][0u][jss::Hook][jss::HookOn] =
                "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFBFFFFF";

            // we'll also make a reference anchor for reward and governance hooks here, so they aren't
            // deleted from the ledger in subsequent tests
            tx[jss::Hooks][1u] = Json::objectValue;
            tx[jss::Hooks][1u][jss::Hook] = Json::objectValue;
            tx[jss::Hooks][1u][jss::Hook][jss::HookHash] = strHex(governHookHash);


            tx[jss::Hooks][2u] = Json::objectValue;
            tx[jss::Hooks][2u][jss::Hook] = Json::objectValue;
            tx[jss::Hooks][2u][jss::Hook][jss::HookHash] = strHex(rewardHookHash);
                

            env(tx,  M(__LINE__), fee(XRP(100)));
            env.close();


            BEAST_EXPECT(!!env.le(ripple::keylet::hookDefinition(acceptHookHash)));

            uint8_t const* data = acceptHookHash.data();
            std::vector<uint8_t> accept_data(data, data+32);

            std::cout << "accept_data-strhex: " << strHex(accept_data) << "\n";
            std::vector<uint8_t> const null_data 
            {
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U
            };

            doL1Vote(__LINE__, alice, 'H', 7, accept_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 7, accept_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 7, accept_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 7, accept_data, null_data, false);
    
            env.close();
            env.close();

            // check if the hook not was installed because we have not given it 100% votes yet
            {
                auto const hooks = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() == 2);
            }


            // now cast final vote
            doL1Vote(__LINE__, david, 'H', 7, accept_data, null_data, false);

            env.close();
            env.close();

            // now check it was installed
            {
                auto const hooks = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() > 7 &&
                    hooks->getFieldArray(sfHooks)[7].isFieldPresent(sfHookHash) && 
                    hooks->getFieldArray(sfHooks)[7].getFieldH256(sfHookHash) == acceptHookHash);
            }

            // now change a vote (note that the topic state is never recorded for hooks, so old data is null)
            doL1Vote(__LINE__, carol, 'H', 7, null_data, null_data, false);

            env.close();
            env.close();
            
            // now check it's still installed
            {
                auto const hooks = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() > 7 &&
                    hooks->getFieldArray(sfHooks)[7].isFieldPresent(sfHookHash) && 
                    hooks->getFieldArray(sfHooks)[7].getFieldH256(sfHookHash) == acceptHookHash);
            }

            // now vote to delete it
            doL1Vote(__LINE__, alice, 'H', 7, null_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 7, null_data, null_data, false);
            doL1Vote(__LINE__, david, 'H', 7, null_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 7, null_data, null_data, false);
            

            env.close();
            env.close();
            
            // now check it's still installed
            {
                auto const hooks = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() > 7 &&
                    !hooks->getFieldArray(sfHooks)[7].isFieldPresent(sfHookHash));
            }
            
            // vote to place an invalid hook
            std::vector<uint8_t> invalid_data 
            {
                0xFFU,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U
            };

            doL1Vote(__LINE__, alice, 'H', 1, invalid_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 1, invalid_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 1, invalid_data, null_data, false);
            doL1Vote(__LINE__, david, 'H', 1, invalid_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 1, invalid_data, null_data, false, true);


            uint8_t const* gdata = governHookHash.data();
            std::vector<uint8_t> govern_data(gdata, gdata+32);
            uint8_t const* rdata = rewardHookHash.data();
            std::vector<uint8_t> reward_data(rdata, rdata+32);
    
            
            // vote to put governance hook into position 2
            doL1Vote(__LINE__, alice, 'H', 2, govern_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 2, govern_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 2, govern_data, null_data, false);
            doL1Vote(__LINE__, david, 'H', 2, govern_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 2, govern_data, null_data, false);

            env.close();
            env.close();            

            // vote to replace the hook at position 1 with accept
            doL1Vote(__LINE__, alice, 'H', 1, accept_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 1, accept_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 1, accept_data, null_data, false);
            doL1Vote(__LINE__, david, 'H', 1, accept_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 1, accept_data, null_data, false);

            env.close();
            env.close();

            // vote to place reward hook at position 2
            doL1Vote(__LINE__, alice, 'H', 0, reward_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 0, reward_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 0, reward_data, null_data, false);
            doL1Vote(__LINE__, david, 'H', 0, reward_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 0, reward_data, null_data, false);

            env.close();
            env.close();

            // hooks array should now look like {governi, accept, reward, nothing ...}

            {
                auto const hooksLE = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooksLE && hooksLE->getFieldArray(sfHooks).size() >= 3);
                
                if (hooksLE && hooksLE->getFieldArray(sfHooks).size() >=3)
                {
                    auto const hooks = hooksLE->getFieldArray(sfHooks);
                    BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == rewardHookHash);
                    BEAST_EXPECT(hooks[1].getFieldH256(sfHookHash) == acceptHookHash);
                    BEAST_EXPECT(hooks[2].getFieldH256(sfHookHash) == governHookHash);
                }
            }

            // set hook 1 back to reward
            doL1Vote(__LINE__, alice, 'H', 1, reward_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 1, reward_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 1, reward_data, null_data, false); 
            doL1Vote(__LINE__, david, 'H', 1, reward_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 1, reward_data, null_data, false);
            
            env.close();
            env.close();
            
            // set hook 0 back to govern
            doL1Vote(__LINE__, alice, 'H', 0, govern_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 0, govern_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 0, govern_data, null_data, false); 
            doL1Vote(__LINE__, david, 'H', 0, govern_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 0, govern_data, null_data, false);
            
            env.close();
            env.close();

            // delete hook at 2
            doL1Vote(__LINE__, alice, 'H', 2, null_data, null_data, false);
            doL1Vote(__LINE__, bob, 'H', 2, null_data, null_data, false);
            doL1Vote(__LINE__, carol, 'H', 2, null_data, null_data, false);
            doL1Vote(__LINE__, david, 'H', 2, null_data, null_data, false);
            doL1Vote(__LINE__, edward, 'H', 2, null_data, null_data, false);

            env.close();
            env.close();
    

            // check we're back the way we were
            {
                auto const hooksLE = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooksLE && hooksLE->getFieldArray(sfHooks).size() >= 2);
                
                if (hooksLE && hooksLE->getFieldArray(sfHooks).size() >=2)
                {
                    auto const hooks = hooksLE->getFieldArray(sfHooks);
                    BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == governHookHash);
                    BEAST_EXPECT(hooks[1].getFieldH256(sfHookHash) == rewardHookHash);
                    BEAST_EXPECT(hooks.size() == 2 || !hooks[2].isFieldPresent(sfHookHash));
                }
            }

            // change a vote, and ensure nothing changed
            doL1Vote(__LINE__, alice, 'H', 0, reward_data, null_data, false);
            env.close();
            env.close();
            {
                auto const hooksLE = env.le(keylet::hook(env.master.id()));
                BEAST_EXPECT(!!hooksLE && hooksLE->getFieldArray(sfHooks).size() >= 2);
                
                if (hooksLE && hooksLE->getFieldArray(sfHooks).size() >=2)
                {
                    auto const hooks = hooksLE->getFieldArray(sfHooks);
                    BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == governHookHash);
                    BEAST_EXPECT(hooks[1].getFieldH256(sfHookHash) == rewardHookHash);
                    BEAST_EXPECT(hooks.size() == 2 || !hooks[2].isFieldPresent(sfHookHash));
                }
            }

        }       

        uint8_t const member_count_key[32] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,'M','C'};
        std::vector<uint8_t> const null_acc_id {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

        

        // four of the 5 vote to remove alice
        {
            
            std::vector<uint8_t> const null_acc_id {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
            std::vector<uint8_t> id = vecFromAcc(alice);
            doL1Vote(__LINE__, bob, 'S', 0, null_acc_id, id, false);
            doL1Vote(__LINE__, carol, 'S', 0, null_acc_id, id, false);
            doL1Vote(__LINE__, david, 'S', 0, null_acc_id, id, false);
            doL1Vote(__LINE__, edward, 'S', 0, null_acc_id, id, true);
        }

        // check the membercount is now 4
        {
            auto entry = env.le(keylet::hookState(env.master.id(),
                uint256::fromVoid(member_count_key), beast::zero));
            std::vector<uint8_t> const expected_data {0x04U};
            BEAST_REQUIRE(!!entry);
            BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
        }

        // continue to remove members (david)
        {
            std::vector<uint8_t> id = vecFromAcc(david);
            doL1Vote(__LINE__, bob, 'S', 3, null_acc_id, id, false);
            doL1Vote(__LINE__, carol, 'S', 3, null_acc_id, id, false);
            doL1Vote(__LINE__, edward, 'S', 3, null_acc_id, id, true);
        }

        {
            // RH TODO: check all david's previous votes were correctly removed
            // check expected hookstate count
        }

        // check the membercount is now 3
        {
            auto entry = env.le(keylet::hookState(env.master.id(),
                uint256::fromVoid(member_count_key), beast::zero));
            std::vector<uint8_t> const expected_data {0x03U};
            BEAST_REQUIRE(!!entry);
            BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
        }

        // continue to remove members (carol)
        {
            std::vector<uint8_t> id = vecFromAcc(carol);
            doL1Vote(__LINE__, bob, 'S', 2, null_acc_id, id, false);
            doL1Vote(__LINE__, edward, 'S', 2, null_acc_id, id, true);
        }
        
        // check the membercount is now 2
        {
            auto entry = env.le(keylet::hookState(env.master.id(),
                uint256::fromVoid(member_count_key), beast::zero));
            std::vector<uint8_t> const expected_data {0x02U};
            BEAST_REQUIRE(!!entry);
            BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
        }

        // member count can't fall below 2
        // try to vote out edward using 2/2 votes
        {
            std::vector<uint8_t> id = vecFromAcc(edward);
            doL1Vote(__LINE__, bob, 'S', 4, null_acc_id, id, false);
            doL1Vote(__LINE__, edward, 'S', 4, null_acc_id, id, false, true);
        }

        // check the membercount is now 2
        {
            auto entry = env.le(keylet::hookState(env.master.id(),
                uint256::fromVoid(member_count_key), beast::zero));
            std::vector<uint8_t> const expected_data {0x02U};
            BEAST_REQUIRE(!!entry);
            BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
        }

        // try to remove bob using 1/2 votes
        {
            std::vector<uint8_t> id = vecFromAcc(bob);
            doL1Vote(__LINE__, bob, 'S', 1, null_acc_id, id, false, false);
        }

        // that shoul fail
        // check the membercount is now 2
        {
            auto entry = env.le(keylet::hookState(env.master.id(),
                uint256::fromVoid(member_count_key), beast::zero));
            std::vector<uint8_t> const expected_data {0x02U};
            BEAST_REQUIRE(!!entry);
            BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
        }

        // replace edward with alice
        {
            std::vector<uint8_t> alice_id = vecFromAcc(alice);
            std::vector<uint8_t> edward_id = vecFromAcc(edward);
            doL1Vote(__LINE__, bob, 'S', 4, alice_id, edward_id, false);
            doL1Vote(__LINE__, edward, 'S', 4, alice_id, edward_id, true);
            // subsequent edward vote should fail because he's not a member anymore
            doL1Vote(__LINE__, edward, 'S', 4, null_acc_id, alice_id, false, true);
        }
        
        // check the membercount is now 2
        {
            auto entry = env.le(keylet::hookState(env.master.id(),
                uint256::fromVoid(member_count_key), beast::zero));
            std::vector<uint8_t> const expected_data {0x02U};
            BEAST_REQUIRE(!!entry);
            BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
        }

        auto doSeatVote = [&](uint64_t lineno, uint8_t seat_no, uint8_t final_count,
            AccountID const& candidate, std::optional<AccountID const> previous,
            std::vector<Account const*> const& voters, bool actioned = true, bool shouldFail = false)
        {
            std::vector<uint8_t> previd { 0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0} ;
            if (previous)
                memcpy(previd.data(), previous->data(), 20);

            std::vector<uint8_t> id { 0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0} ;
            memcpy(id.data(), candidate.data(), 20);

            int count = voters.size();
            for (Account const* voter : voters)
                doL1Vote(lineno, *voter, 'S', seat_no, id, previd, --count <= 0 && actioned, shouldFail);
           
            if (!shouldFail) 
            {
                auto entry = env.le(keylet::hookState(env.master.id(),
                    uint256::fromVoid(member_count_key), beast::zero));
                std::vector<uint8_t> const expected_data {final_count};

                BEAST_REQUIRE(!!entry);
                if (entry->getFieldVL(sfHookStateData) != expected_data)
                    std::cout 
                        << "doSeatVote failed " << lineno <<"L. entry data: `" 
                        << strHex(entry->getFieldVL(sfHookStateData)) << "` "
                        << "expected data: `" << strHex(expected_data) << "`\n";
 
                BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
            }
        };

        std::cout << "Put edward back into seat 0\n";

        // put edward into seat 0 previously occupied by alice
        doSeatVote(__LINE__, 0, 3, edward.id(), {}, {&alice, &bob});
        
        // at this point the governance table looks like
        // 0 - edward
        // 1 - bob
        // 2 - empty
        // 3 - empty
        // 4 - alice
 
        // fill seats 2,3 with accounts m6 and m7. this should take 2 votes only
        // alice's vote alone is not enough
        doSeatVote(__LINE__, 2, 3, m6.id(), {}, {&alice}, false);
        // edward's vote makes 2/3 which is floor(0.8 * members)
        doSeatVote(__LINE__, 2, 4, m6.id(), {}, {&edward});

        // re-action should succeed
        doSeatVote(__LINE__, 2, 4, m6.id(), m6, {&alice});

        // re-voting for something else should not change the action outcome
        doSeatVote(__LINE__, 2, 4, m11.id(), m6, {&alice}, false);

        // check that m6 is still in seat 2
        {
            uint8_t key[32] = {
                0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,
                0,0,0,0,0,0,0
            };

            for (int i = 12; i < 32; ++i)
                key[i] = m6.id().data()[i-12];
    
            auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));
            BEAST_EXPECT(!!entry &&
                entry->getFieldVL(sfHookStateData) == std::vector<uint8_t>{0x02U});
        }


        // now we will need 3 votes to put m7 into seat 3
        doSeatVote(__LINE__, 3, 4, m7.id(), {}, {&m6}, false);
        doSeatVote(__LINE__, 3, 4, m7.id(), {}, {&alice}, false);
        doSeatVote(__LINE__, 3, 5, m7.id(), {}, {&bob}, true);

        // now we have: ed, bob, m6, m7, alice
        // let's try moving ed to another seat, this should succeed
        doSeatVote(__LINE__, 5, 5, edward.id(), {}, {&alice, &bob, &edward, &m7});

        // now lets move edward over the top of bob
        doSeatVote(__LINE__, 1, 4, edward.id(), bob, {&alice, &bob, &m6, &m7});

        // first put bob in where edward was
        doSeatVote(__LINE__, 0, 5, bob.id(), {}, {&alice, &edward, &m6});
    
        // now we have: bob, ed, m6, m7, alice
        // we're going to fill the remaining seats up to 20
        doSeatVote(__LINE__, 5, 6, carol.id(), {}, {&alice, &edward, &m6, &m7});
        doSeatVote(__LINE__, 6, 7, david.id(), {}, {&alice, &edward, &m6, &m7});


        // { bob, ed, m6, m7, alice, carol, david }

        // quorum = floor(0.8*7) = 5
        doSeatVote(__LINE__, 7, 8, m8.id(), {}, {&alice, &edward, &m6, &m7, &carol});

        // q = floor(0.8*8) = 6
        doSeatVote(__LINE__, 8, 9, m9.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8});

        // q = floor(0.8*9) = 7
        doSeatVote(__LINE__, 9, 10, m10.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9});

        // q = floor(0.8*10) = 8
        doSeatVote(__LINE__, 10, 11, m11.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10});

        // q = floor(0.8*11) = 8
        doSeatVote(__LINE__, 11, 12, m12.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10});

        // q = floor(0.8*12) = 9
        doSeatVote(__LINE__, 12, 13, m13.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11});

        // q = floor(0.8*13) = 10
        doSeatVote(__LINE__, 13, 14, m14.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12});

        // q = floor(0.8*14) = 11
        doSeatVote(__LINE__, 14, 15, m15.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13});

        // q = floor(0.8*15) = 12
        doSeatVote(__LINE__, 15, 16, m16.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13, &m14});

        // q = floor(0.8*16) = 12
        doSeatVote(__LINE__, 16, 17, m17.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13, &m14});

        // q = floor(0.8*17) = 13
        doSeatVote(__LINE__, 17, 18, m18.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13, &m14, &m15});

        // q = floor(0.8*18) = 14
        doSeatVote(__LINE__, 18, 19, m19.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13, &m14, &m15, &m16});

        // q = floor(0.8*19) = 15
        doSeatVote(__LINE__, 19, 20, m20.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13, &m14, &m15, &m16, &m17});

        // q = floor(0.8*20) = 16
        // voting for seat 20 is invalid, only seats 0 to 19 are valid
        doSeatVote(__LINE__, 20, 21, m21.id(), {}, {&alice, &edward, &m6, &m7, &carol, &m8, &m9, &m10, &m11, &m12, &m13, &m14, &m15, &m16, &m17, &m18}, false, true);


        // voting for another random high seat
        doSeatVote(__LINE__, 255, 21, m21.id(), {}, {&alice}, false, true);
        doSeatVote(__LINE__, 101, 21, m21.id(), {}, {&alice}, false, true);
        

        // RH TODO: check state count 

        // membership:
        // { bob, ed, m6, m7, alice, carol, david, m8, ... m20 }
        // check this is the case forward and reverse keys

        auto const checkSeat = [&](uint8_t seat, Account const& acc)
        {
            uint8_t key[32] = {
                0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,
                0,0,0,0,0,0,0
            };

            // seatno => accid
            {
                key[31] = seat;
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));
                if (!entry)
                    std::cout << "checkSeat failed, seatno->accid missing for seat: " << seat << "\n";
                else if (entry->getFieldVL(sfHookStateData) != vecFromAcc(acc))
                    std::cout << "checkSeat failed, seatno->accid incorrect for seat: " << seat << "\n";
                BEAST_EXPECT(!!entry &&
                    entry->getFieldVL(sfHookStateData) == vecFromAcc(acc));
            }
            
            // accid => seatno
            {
                for (int i = 12; i < 32; ++i)
                key[i] = acc.id().data()[i-12];
    
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));

                if (!entry)
                    std::cout << "checkSeat failed, accid->seatno missing for seat: " << seat << "\n";
                else if (entry->getFieldVL(sfHookStateData) != std::vector<uint8_t>{seat})
                    std::cout << "checkSeat failed, accid->seatno incorrect for seat: " << seat << "\n";

                BEAST_EXPECT(!!entry &&
                    entry->getFieldVL(sfHookStateData) == std::vector<uint8_t>{seat});
            }
        };

        std::vector<Account const*> finalSeats {
                &bob, &edward, &m6, &m7, &alice, &carol, &david, 
                &m8, &m9, &m10, &m11, &m12, &m13, &m14, &m15, &m16, &m17, &m18, &m19, &m20};

        for (int i = 0; i < 20; ++i)
            checkSeat(i, *(finalSeats[i]));

    }

//        auto hooksArray =

        // RH TODO:


        // governance hook tests:
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
        //testPlainActivation();
        //testWithSignerList();
        //testWithRegularKey();
        testGovernanceL1();
    }
};

BEAST_DEFINE_TESTSUITE(XahauGenesis, app, ripple);

}  // namespace test
}  // namespace ripple
