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

#define DEBUG_XGTEST 0

using namespace XahauGenesis;

namespace ripple {
namespace test {
/*
    Accounts used in this test suite:
    alice: AE123A8556F3CF91154711376AFB0F894F832B3D, rG1QQv2nh2gr7RCZ1P8YYcBUKCCN633jCn
    bob: F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90F, rPMh7Pi9ct699iZUTWaytJUoHcJ7cgyziK
    carol: B389FBCED0AF9DCDFF62900BFAEFA3EB872D8A96, rH4KEcG9dEwGwpn6AyoWK9cZPLL4RLSmWW
    david: 0F4BFC99EC975E3F753927A69713889359C7100E, rpPtwXbmeXxznrUvnMuGhUqTu4Vvk4V98i
    edward: 98D3AAD96D5D3F32C3723B6550A49DEE4DD9D4AC, rNAnHhommqNJvV3mLieHADNJU6mfdRqXKp
    m1: 1EF5C53AC2B0E1CDEAF960833A9B7B6814879152, rsF66BeCcW5dxrDhPbCqm4ReU5MjkmshQo
    m2: 0D20E8D13A89AA84F2334F57331903D21CCC866C, rpURDnkgizBF1ksJr9DmDTNLhds2NBYNbC
    m6: F41810565A673D2C45C63B7A51FEA139221DFB1B, rPEe68FWPYpfEYeL3TtdVGXR8WDET24h6e
    m7: 52C947B84E2412B5BD2211639E2B9DEDF3ECB5F4, r3YjXvPbu1srxx22tQefDwhXJS8qjCAgxg
    m8: 3392382F09E5EA935D5624D52919C683B661238A, rn6gdHFFtieGK748TioK8cZ8SPrHsWAfe3
    m9: 62F73BE6E7718B3328DEB497F8298B1C42FFD3D8, rwpHPvGwiJAFStN3KqVTeTLBTczFyK6Z6D
    m10: 971B25036F1EAAEEC7A0C299E15F60AB4C465B37, rNmyZrJEtbg5p84PSKGurNs1efdDNLhb8D
    m11: B79B324A56425919D0BF64FA4560C7B616F08509, rHjF2LuJKSRerNHtQMtyDUVXpQKmT18XuC
    m12: 8C3AA72EBB3172726BE9FDE2A91C9CE2C9F766E5, rD8T19Nz2gkAtKKEeLpXLkTb57AS7nJ45b
    m13: 3B8292604D9CF9C679E70CCD0ED5C9DC1DF84607, raRCHchPDpP8qAysxsmehC279GH613iGiM
    m14: E77F5DFE960C4DA9524389F946BCD13AC26AB0D0, r4fsCASeoGzhD2ZeFgcJj1c2tmDsm1re3r
    m15: D300D94BA59F55426889E144B423CFEE8F685450, rLNgbTE2HVk5CJG7SHrpSD8gsFcwu5cG1a
    m16: D9C6AC46D87BCFE374BE4F59C280CEC377E67788, rLiVdCesA2rV2NjC6HZPVK7k1VpvGwCbjx
    m17: A620E154D56E38B12AD76A094A214A25AD8B87A1, rG9QZQDR8XqCU1x2VARPuZwYxqcb3J9bYa
    m18: 7A5A72F059F6DCDD6E938DAFBCFFB0541E0A7481, rU9A8u622H6fxGQjux8uDgomks5D8QySfS
    m19: F6C06C3D86A9D39FF813AEF6B839AD041651BE7D, rPV6jx8QoQBCR1AcmVLDnu98QBu4QakwhU
    m20: 1B4D3113C2AB370293A0ACEA4D68C1B29A01A013, rsVM5ZaK9QMCgrW9UKyXLguESpDpsJnRbu
    m21: 748B256D2BAD918A967F40F465692C7B9A01F836, rBdNnJ4q9G3riJFcXjkwTgBu1MBR5pG4gD
*/



struct XahauGenesis_test : public beast::unit_test::suite
{

    uint256 const acceptHookHash =
            ripple::sha512Half_s(ripple::Slice(XahauGenesis::AcceptHook.data(), XahauGenesis::AcceptHook.size()));
    uint256 const governHookHash =
            ripple::sha512Half_s(
                ripple::Slice(XahauGenesis::GovernanceHook.data(), XahauGenesis::GovernanceHook.size()));
    uint256 const rewardHookHash =
            ripple::sha512Half_s(ripple::Slice(XahauGenesis::RewardHook.data(), XahauGenesis::RewardHook.size()));

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
            BEAST_EXPECT(govSLE->getFieldU64(sfReferenceCount) == 1 + XahauGenesis::L2Membership.size());
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
    setupGov(jtx::Env& env, 
        std::vector<AccountID> const members,
        std::map<AccountID, std::vector<AccountID>> const tables = {})
    {
        using namespace jtx;

        auto const invoker = Account("invoker");
        env.fund(XRP(10000), invoker);
        env.close();

        XahauGenesis::TestDistribution.clear();
        XahauGenesis::TestL2Membership.clear();

        for (auto& m: members)
        {
            std::string acc = toBase58(m);
            XahauGenesis::TestDistribution.emplace_back(acc, XRPAmount(10000000000));
        }

        for(auto& [t, members] : tables)
        {
            std::vector<std::string> membersStr;
            for (auto& m : members)
                membersStr.emplace_back(toBase58(m));

            XahauGenesis::TestL2Membership.emplace_back(
                toBase58(t), membersStr);

        }

        activate(env, true, true, true);

        env.close();

        XahauGenesis::TestDistribution.clear();
        XahauGenesis::TestL2Membership.clear();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = invoker.human();
        invoke[jss::Destination] = env.master.human();
        env(invoke, fee(XRP(1)));
        env.close();

        for (auto& [t, members] : tables)
        {
            // setup each L2 table
            Json::Value invoke;
            invoke[jss::TransactionType] = "Invoke";
            invoke[jss::Account] = invoker.human();
            invoke[jss::Destination] = toBase58(t);
            env(invoke, fee(XRP(1)));
            env.close();
            if (DEBUG_XGTEST)
                std::cout << "invoke: " << invoke << "\n";
        }
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
        
    inline
    static
    std::vector<uint8_t>
    vecFromAcc(jtx::Account const& acc)
    {
        uint8_t const* data = acc.id().data();
        return std::vector<uint8_t>(data, data+20);
    };

    inline
    static
    Json::Value
    vote(
        uint16_t lineno,
        jtx::Account const& acc,
        jtx::Account const& table,
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
        if (DEBUG_XGTEST)
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

        uint8_t upto = 2u;
        if (layer)
        {
            txn[jss::HookParameters][upto] = Json::objectValue;
            txn[jss::HookParameters][upto][jss::HookParameter][jss::HookParameterName] = "4C";
            txn[jss::HookParameters][upto][jss::HookParameter][jss::HookParameterValue] = charToHex(*layer);
            upto++;
        }

        // add a line number for debugging
        txn[jss::HookParameters][upto] = Json::objectValue;
        txn[jss::HookParameters][upto][jss::HookParameter][jss::HookParameterName] = "44"; // D
        {
            std::string strData;
            strData += charToHex((uint8_t)(lineno >> 8U));
            strData += charToHex((uint8_t)(lineno & 0xFFU));
            txn[jss::HookParameters][upto][jss::HookParameter][jss::HookParameterValue] = strData;
        }



        txn[jss::Account] = acc.human();
        txn[jss::TransactionType] = "Invoke";
        txn[jss::Destination] = table.human();
        return txn;
    };
    
    std::vector<uint8_t> const null_acc_id {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    AccountID const null_acc;
        
    inline
    static
    uint256
    makeStateKey
    (char voteType, char topic1, char topic2, uint8_t layer, AccountID const& id)
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
    }

    uint8_t const member_count_key[32] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,'M','C'};

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

        auto const m1 = Account("m1");
        auto const m2 = Account("m2");

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

        env.fund(XRP(10000), alice, bob, carol, david, edward, m1, m2,
            m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21);

        env.close();

        std::vector<Account> initial_members {alice, bob, carol, david, edward};
        std::vector<AccountID> initial_members_ids { alice.id(), bob.id(), carol.id(), david.id(), edward.id() };

        setupGov(env, initial_members_ids);


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
                static_cast<uint8_t>(topic1 == 'S' ? 0 : topic1), static_cast<uint8_t>(topic2)};
            // check actioning prior to vote
            {
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));
                if (DEBUG_XGTEST)
                {
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
                }
                BEAST_EXPECT((isOldDataZero && !entry) ||
                    (entry && entry->getFieldVL(sfHookStateData) == old_data));
            }

            // perform and check vote
            {
                env(vote(lineno, acc, env.master, topic1, topic2, vote_data), M(lineno), fee(XRP(1)),
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
                        if (DEBUG_XGTEST)
                        {
                            std::cout << "new data: " << strHex(vote_data) << "\n";
                            std::cout << "lgr data: " <<
                                (!entry ? "<doesn't exist>" : strHex(entry->getFieldVL(sfHookStateData))) << "\n";
                        }
                        BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == vote_data);
                    }
                }
                else
                {
                    if (!!entry && DEBUG_XGTEST)
                        std::cout << "old data: " << strHex(entry->getFieldVL(sfHookStateData)) << "\n";
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




        // four of the 5 vote to remove alice
        {

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
                if (entry->getFieldVL(sfHookStateData) != expected_data && DEBUG_XGTEST)
                {
                    std::cout
                        << "doSeatVote failed " << lineno <<"L. entry data: `"
                        << strHex(entry->getFieldVL(sfHookStateData)) << "` "
                        << "expected data: `" << strHex(expected_data) << "`\n";
                }
                BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
            }
        };

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

        auto const checkSeat = [&](uint8_t seat, Account const* acc)
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

                if (!acc)
                {
                    if (entry && DEBUG_XGTEST)
                    {
                        std::cout
                            << "checkSeat failed, "
                            << "seatno->accid present (but should be empty) for seat: "
                            << seat << "\n";
                    }
                    BEAST_EXPECT(!entry);
                    return;
                }

                if (DEBUG_XGTEST)
                {
                    if (!entry)
                        std::cout << "checkSeat failed, seatno->accid missing for seat: " << seat << "\n";
                    else if (entry->getFieldVL(sfHookStateData) != vecFromAcc(*acc))
                        std::cout << "checkSeat failed, seatno->accid incorrect for seat: " << seat << "\n";
                }
                BEAST_EXPECT(!!entry &&
                    entry->getFieldVL(sfHookStateData) == vecFromAcc(*acc));
            }

            // accid => seatno
            {
                for (int i = 12; i < 32; ++i)
                key[i] = acc->id().data()[i-12];

                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));

                if (DEBUG_XGTEST)
                {
                    if (!entry)
                        std::cout << "checkSeat failed, accid->seatno missing for seat: " << seat << "\n";
                    else if (entry->getFieldVL(sfHookStateData) != std::vector<uint8_t>{seat})
                        std::cout << "checkSeat failed, accid->seatno incorrect for seat: " << seat << "\n";
                }

                BEAST_EXPECT(!!entry &&
                    entry->getFieldVL(sfHookStateData) == std::vector<uint8_t>{seat});
            }
        };

        {
            std::vector<Account const*> finalSeats {
                    &bob, &edward, &m6, &m7, &alice, &carol, &david,
                    &m8, &m9, &m10, &m11, &m12, &m13, &m14, &m15, &m16, &m17, &m18, &m19, &m20};

            for (int i = 0; i < 20; ++i)
                checkSeat(i, finalSeats[i]);
        }

        // go back to the original 5 in their original seats: alice, bob, carol, david, edward

        // set alice into position 0, this frees up spot 4 bringing count down to 19
        // floor (20*0.8) = 16
        doSeatVote(__LINE__, 0, 19, alice.id(), bob, {
                &bob, &edward, &m6, &m7,
                &alice, &carol, &david,  &m8,
                &m9, &m10, &m11, &m12,
                &m13, &m14, &m15, &m16}, true);

        // now seating is:
        // {alice, edward, m6, m7, blank, carol, david, m8, ... }

        // edward into position 4, this frees up spot 1, and moves into a blank spot thus keeps count at 19
        // note floor(19*0.8) = 15
        doSeatVote(__LINE__, 4, 19, edward.id(), {}, {
                &alice, &edward, &m6, &m7,
                &m16, &carol, &david,  &m8,
                &m9, &m10, &m11, &m12,
                &m13, &m14, &m15}, true);

        // now seating is:
        // {alice, blank, m6, m7, edward, carol, david, m8, ...}

        // bob into position 1, the count becomes 20
        doSeatVote(__LINE__, 1, 20, bob.id(), {}, {
                &alice, &edward, &m6, &m7,
                &m16, &carol, &david,  &m8,
                &m9, &m10, &m11, &m12,
                &m13, &m14, &m15}, true);

        // {alice, bob, m6, m7, edward, carol, david, m8, ...}

        // carol into position 2, this frees up seat 5, bringing count to 19
        doSeatVote(__LINE__, 2, 19, carol.id(), m6, {
                &alice, &bob, &m6, &m7,
                &m16, &carol, &david,  &m8,
                &m9, &m10, &m11, &m12,
                &m13, &m14, &m15, &m17}, true);

        // {alice, bob, carol, m7, edward, blank, david, m8, ...}

        // david into position 3, this frees up seat 6, bringing count to 18
        doSeatVote(__LINE__, 3, 18, david.id(), m7, {
                &alice, &bob, &m18, &m17,
                &m16, &carol, &david,  &m8,
                &m9, &m10, &m11, &m12,
                &m13, &m14, &m15}, true);

        // {alice, bob, carol, david, edward, blank, blank, m8, ...}

        // remove all higher members


        // floor(18*0.8) = 14
        doSeatVote(__LINE__, 7, 17, null_acc, m8, {
                &alice, &bob, &carol, &david, &edward,
                &m8, &m9, &m10, &m11, &m12,
                &m13, &m14, &m15, &m16 }, true);

        // floor(17*0.8) = 13
        doSeatVote(__LINE__, 8, 16, null_acc, m9, {
                &alice, &bob, &carol, &david, &edward,
                &m9, &m10, &m11, &m12,
                &m13, &m14, &m15, &m16 }, true);

        // floor(16*0.8)=12
        doSeatVote(__LINE__, 9, 15, null_acc, m10, {
                &alice, &bob, &carol, &david, &edward,
                &m10, &m11, &m12,
                &m13, &m14, &m15, &m16 }, true);

        // floor(15*0.8)=12
        doSeatVote(__LINE__, 10, 14, null_acc, m11, {
                &alice, &bob, &carol, &david, &edward,
                &m11, &m12,
                &m13, &m14, &m15, &m16, &m17 }, true);


        // floor(14*0.8)=11
        doSeatVote(__LINE__, 11, 13, null_acc, m12, {
                &alice, &bob, &carol, &david, &edward,
                &m12,
                &m13, &m14, &m15, &m16, &m17 }, true);


        // floor(13*0.8)=10
        doSeatVote(__LINE__, 12, 12, null_acc, m13, {
                &alice, &bob, &carol, &david, &edward,
                &m13, &m14, &m15, &m16, &m17 }, true);

        // floor(12*0.8)=9
        doSeatVote(__LINE__, 13, 11, null_acc, m14, {
                &alice, &bob, &carol, &david, &edward,
                &m14, &m15, &m16, &m17 }, true);

        // floor(11*0.8)=8
        doSeatVote(__LINE__, 14, 10, null_acc, m15, {
                &alice, &bob, &carol, &david, &edward,
                &m15, &m16, &m17 }, true);

        // floor(10*0.8)=8
        doSeatVote(__LINE__, 15, 9, null_acc, m16, {
                &alice, &bob, &carol, &david, &edward,
                &m17, &m18, &m19 }, true);

        // floor(9*0.8)=7
        doSeatVote(__LINE__, 16, 8, null_acc, m17, {
                &alice, &bob, &carol, &david, &edward,
                &m19, &m20 }, true);

        // floor(8*0.8)=6
        doSeatVote(__LINE__, 17, 7, null_acc, m18, {
                &alice, &bob, &carol, &david, &edward,
                &m20 }, true);

        // floor(7*0.8)=5
        doSeatVote(__LINE__, 18, 6, null_acc, m19, {
                &alice, &bob, &carol, &david, &edward}, true);

        // floor(6*0.8)=4
        doSeatVote(__LINE__, 19, 5, null_acc, m20, {
                &alice, &bob, &carol, &david}, true);


        // check the seats are correct
        {
            std::vector<Account const*> finalSeats {
                    &alice, &bob, &carol, &david, &edward};

            for (int i = 0; i < 20; ++i)
                checkSeat(i, i < 5 ? finalSeats[i] : NULL);
        }



    }

    void testGovernanceL2()
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

        auto const t1 = Account("t1");
        auto const t2 = Account("t2");
        auto const t3 = Account("t3");

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

        auto const governHookHash =
                ripple::sha512Half_s(
                    ripple::Slice(XahauGenesis::GovernanceHook.data(), XahauGenesis::GovernanceHook.size()));
        
        // check tables correctly configured
        auto checkL2Table = [&](AccountID const& tableID, std::vector<Account const*> members) -> void
        {
            auto const hookLE = env.le(keylet::hook(tableID));
            BEAST_EXPECT(!!hookLE);
            uint256 const ns = beast::zero;
            uint8_t mc = 0;
            if (hookLE)
            {
                auto const hooksArray = hookLE->getFieldArray(sfHooks);
                BEAST_EXPECT(hooksArray.size() == 1 && hooksArray[0].getFieldH256(sfHookHash) == governHookHash);
            
 
                for (Account const* m: members)
                {

                    auto const mVec = vecFromAcc(*m);
                    // forward key

                    uint8_t key[32] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,mc};
                    
                    auto const forward = env.le(keylet::hookState(tableID, uint256::fromVoid(key), ns));
                    BEAST_EXPECT(!!forward && forward->getFieldVL(sfHookStateData) == mVec);
                
                    // reverse key

                    for (int i = 12; i < 32; ++i)
                        key[i] = mVec[i-12];

                    auto const reverse = env.le(keylet::hookState(tableID, uint256::fromVoid(key), ns));
                    BEAST_EXPECT(!!reverse && reverse->getFieldVL(sfHookStateData) == std::vector<uint8_t>{mc});
                    mc++;
                }
            }

            // check membercount is correct
            {
                uint8_t key[32] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 'M', 'C'};
                auto const mcLE = env.le(keylet::hookState(tableID, uint256::fromVoid(key), ns));
                BEAST_EXPECT(!!mcLE && mcLE->getFieldVL(sfHookStateData) == std::vector<uint8_t>{mc});
            }

            // check the hook didn't setup a reward rate or delay key
            {
                uint8_t key[32] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 'R', 'R'};
                {
                    auto const le = env.le(keylet::hookState(tableID, uint256::fromVoid(key), ns));
                    BEAST_EXPECT(!le);
                }
                key[31] = 'D';
                {
                    auto const le = env.le(keylet::hookState(tableID, uint256::fromVoid(key), ns));
                    BEAST_EXPECT(!le);
                }
            }

            // check account has correct ownercount and is blackholed
            auto const root = env.le(keylet::account(tableID));
            BEAST_EXPECT(!!root);
            if (root)
            {
                BEAST_EXPECT(root->getFieldU32(sfOwnerCount) == mc*2 + 2);
                BEAST_EXPECT(root->getFieldU32(sfFlags) & lsfDisableMaster);
                BEAST_EXPECT(root->getAccountID(sfRegularKey) == noAccount());
            }
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
                static_cast<uint8_t>(topic1 == 'S' ? 0 : topic1), static_cast<uint8_t>(topic2)};
            // check actioning prior to vote
            {
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key), beast::zero));
                if (DEBUG_XGTEST)
                {
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
                }
                BEAST_EXPECT((isOldDataZero && !entry) ||
                    (entry && entry->getFieldVL(sfHookStateData) == old_data));
            }

            // perform and check vote
            {
                env(vote(lineno, acc, env.master, topic1, topic2, vote_data), M(lineno), fee(XRP(1)),
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
                        if (DEBUG_XGTEST)
                        {
                            std::cout << "new data: " << strHex(vote_data) << "\n";
                            std::cout << "lgr data: " <<
                                (!entry ? "<doesn't exist>" : strHex(entry->getFieldVL(sfHookStateData))) << "\n";
                        }
                        BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == vote_data);
                    }
                }
                else
                {
                    if (!!entry && DEBUG_XGTEST)
                        std::cout << "old data: " << strHex(entry->getFieldVL(sfHookStateData)) << "\n";
                    
                    BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == old_data);
                }
            }
        };

        auto doL2Vote = [&](
                uint64_t lineno, uint8_t layer, Account const& table,
                Account const& acc, char topic1, char topic2,
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
                static_cast<uint8_t>(topic1 == 'S' ? 0 : topic1), static_cast<uint8_t>(topic2)};
            // check actioning prior to vote
            {
                auto entry = env.le(keylet::hookState(
                    layer == 1 ? env.master.id() : table.id(),
                    uint256::fromVoid(key), beast::zero));
                if (DEBUG_XGTEST)
                {
                    std::cout
                        << "topic L2 vote precheck: " << lineno << "L\n"
                        << "\tlayer: " << std::string(1, '0' + layer) << " table: " << table.human() << "\n"
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
                }
                BEAST_EXPECT((isOldDataZero && !entry) ||
                    (entry && entry->getFieldVL(sfHookStateData) == old_data));
            }

            // perform and check vote
            {
                env(vote(lineno, acc, table, topic1, topic2, vote_data, layer), M(lineno), fee(XRP(1)),
                    shouldFail ? ter(tecHOOK_REJECTED) : ter(tesSUCCESS));
                env.close();
                auto entry =
                    env.le(keylet::hookState(table.id(),
                        makeStateKey('V', topic1, topic2, layer, acc.id()),
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
                auto entry = env.le(keylet::hookState(
                    layer == 1 ? env.master.id() : table.id(),
                    uint256::fromVoid(key), beast::zero));

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
                        if (DEBUG_XGTEST)
                        {
                            std::cout << lineno << "L "
                                << "new data: " << strHex(vote_data) << "\n"
                                << "lgr data: " <<
                                    (!entry ? "<doesn't exist>" : strHex(entry->getFieldVL(sfHookStateData))) << "\n";
                        }
                        BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == vote_data);
                    }
                }
                else
                {
                    
                    if (DEBUG_XGTEST)
                    {
                        std::cout
                            << lineno << "L old data: "
                            << (entry ? strHex(entry->getFieldVL(sfHookStateData)) : "<doesn't exist>")
                            << " == "  << strHex(old_data) << "\n";
                    }
                    BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == old_data);
                }
            }
        };
        
        auto doL1SeatVote = [&](uint64_t lineno, uint8_t seat_no, uint8_t final_count,
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
                if (entry->getFieldVL(sfHookStateData) != expected_data && DEBUG_XGTEST)
                {
                    std::cout
                        << "doSeatVote failed " << lineno <<"L. entry data: `"
                        << strHex(entry->getFieldVL(sfHookStateData)) << "` "
                        << "expected data: `" << strHex(expected_data) << "`\n";
                }
                BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
            }
        };

        
        auto doL2SeatVote = [&](uint64_t lineno, uint8_t layer, Account const& table,
            uint8_t seat_no, uint8_t final_count,
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
                doL2Vote(lineno, layer, table, *voter, 'S', seat_no, id, previd, --count <= 0 && actioned, shouldFail);

            if (!shouldFail)
            {
                auto entry = env.le(keylet::hookState(
                    layer == 1 ? env.master.id() : table.id(),
                    uint256::fromVoid(member_count_key), beast::zero));
                std::vector<uint8_t> const expected_data {final_count};

                BEAST_REQUIRE(!!entry);
                if (entry->getFieldVL(sfHookStateData) != expected_data && DEBUG_XGTEST)
                {
                    std::cout
                        << "doSeatVote failed " << lineno <<"L. entry data: `"
                        << strHex(entry->getFieldVL(sfHookStateData)) << "` "
                        << "expected data: `" << strHex(expected_data) << "`\n";
                }

                BEAST_EXPECT(entry->getFieldVL(sfHookStateData) == expected_data);
            }
        };


        env.fund(XRP(10000), alice, bob, carol, david, edward, t1, t2, t3,
            m6, m7, m8, m9, m10, m11, m12, m13, m14, m15, m16, m17, m18, m19, m20, m21);

        env.close();
        
        setupGov(env, 
            {alice.id(), bob.id(), t1.id(), t2.id()},
            {
                {t1.id(), {m6, m7}},
                {t2.id(), {m11, m12, m13, m14, m15}}
            });

        env.close();
        checkL2Table(t1.id(), std::vector<Account const*>{&m6, &m7});
        checkL2Table(t2.id(), std::vector<Account const*>{&m11, &m12, &m13, &m14, &m15});

        // test voting from layer 2
        // vote for a different reward rate
        {
            // this will be the new reward rate
            std::vector<uint8_t> vote_data {0x00U,0x80U,0xC6U,0xA4U,0x7EU,0x8DU,0x03U,0x54U};

            // this is the default reward rate
            std::vector<uint8_t> const original_data {0x00U,0xE4U,0x61U,0xEEU,0x78U,0x90U,0x83U,0x54U};

            doL1Vote(__LINE__, alice, 'R', 'R', vote_data, original_data, false);

            // trying to vote for a changed layer2 reward rate should fail because layer2 doesn't have one
            doL2Vote(__LINE__, 2, t1, m6, 'R', 'R', vote_data, {}, false, true);

            // but layer2 can vote for layer1 reward rate:
            doL2Vote(__LINE__, 1, t1, m6, 'R', 'R', vote_data, original_data, false);
            doL2Vote(__LINE__, 1, t1, m7, 'R', 'R', vote_data, original_data, false);

            // a vote by the L2 table should have been emitted now
            env.close();
            env.close();

            // check the vote counter
            auto checkCounter = [&](uint64_t lineno, uint8_t c)
            {
                uint8_t counter_key[32] = {'C','R','R',1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                    0x00U,0x80U,0xC6U,0xA4U,0x7EU,0x8DU,0x03U,0x54U};
                uint256 ns = beast::zero;
                auto const counterLE = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(counter_key), ns));
                
                if (DEBUG_XGTEST)
                {
                    std::cout
                        << "Counter at " << lineno << "L: "
                        << (!!counterLE ? strHex(counterLE->getFieldVL(sfHookStateData)) : "doesn't exist")
                        << " vs " << std::string(1, (char)('0' + c)) << "\n"; 
                }
                BEAST_EXPECT(!!counterLE && counterLE->getFieldVL(sfHookStateData) == std::vector<uint8_t>{c});
            };

            checkCounter(__LINE__, 2);
                
            // vote with 60% of the second table
            doL2Vote(__LINE__, 1, t2, m11, 'R', 'R', vote_data, original_data, false);
            doL2Vote(__LINE__, 1, t2, m12, 'R', 'R', vote_data, original_data, false);
            doL2Vote(__LINE__, 1, t2, m13, 'R', 'R', vote_data, original_data, false);

            env.close();
            env.close();

            checkCounter(__LINE__, 3);

            // final L1 vote should activate it
            doL1Vote(__LINE__, bob, 'R', 'R', vote_data, original_data, true);
            
            checkCounter(__LINE__, 4);

            // reverting a L2 vote should emit a txn undoing the vote
            doL2Vote(__LINE__, 1, t2, m13, 'R', 'R', original_data, vote_data, false);

            env.close();
            env.close();

            checkCounter(__LINE__, 3);

        }

        // vote for a different reward delay
        {
            // this will be the new reward delay
            std::vector<uint8_t> vote_data {0x00U,0x80U,0xC6U,0xA4U,0x7EU,0x8DU,0x03U,0x55U};

            // this is the default reward delay
            std::vector<uint8_t> const original_data {0x00U,0x80U,0x6AU,0xACU,0xAFU,0x3CU,0x09U,0x56U};

            doL1Vote(__LINE__, alice, 'R', 'D', vote_data, original_data, false);

            // trying to vote for a changed layer2 reward rate should fail because layer2 doesn't have one
            doL2Vote(__LINE__, 2, t1, m6, 'R', 'D', vote_data, {}, false, true);

            // but layer2 can vote for layer1 reward rate:
            doL2Vote(__LINE__, 1, t1, m6, 'R', 'D', vote_data, original_data, false);
            doL2Vote(__LINE__, 1, t1, m7, 'R', 'D', vote_data, original_data, false);

            // a vote by the L2 table should have been emitted now
            env.close();
            env.close();

            // check the vote counter
            auto checkCounter = [&](uint64_t lineno, uint8_t c)
            {
                uint8_t counter_key[32] = {'C','R','D',1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                    0x00U,0x80U,0xC6U,0xA4U,0x7EU,0x8DU,0x03U,0x55U};
                uint256 ns = beast::zero;
                auto const counterLE = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(counter_key), ns));
                
                if (DEBUG_XGTEST)
                {
                    std::cout
                        << "Counter at " << lineno << "L: "
                        << (!!counterLE ? strHex(counterLE->getFieldVL(sfHookStateData)) : "doesn't exist")
                        << " vs " << std::string(1, (char)('0' + c)) << "\n"; 
                }
                BEAST_EXPECT(!!counterLE && counterLE->getFieldVL(sfHookStateData) == std::vector<uint8_t>{c});
            };

            checkCounter(__LINE__, 2);
                
            doL1Vote(__LINE__, bob, 'R', 'D', vote_data, original_data, false);
            
            // vote with 60% of the second table
            doL2Vote(__LINE__, 1, t2, m11, 'R', 'D', vote_data, original_data, false);
            doL2Vote(__LINE__, 1, t2, m12, 'R', 'D', vote_data, original_data, false);
            doL2Vote(__LINE__, 1, t2, m13, 'R', 'D', vote_data, original_data, true);

            env.close();
            env.close();

            checkCounter(__LINE__, 4);

            // reverting a L2 vote should emit a txn undoing the vote
            doL2Vote(__LINE__, 1, t2, m11, 'R', 'D', original_data, vote_data, false);

            env.close();
            env.close();

            checkCounter(__LINE__, 3);
        }


        // vote an L1 seat in using L2 voting
        {
            // we'll vote claire into seat 4 using t2 as a voting party
            // this requires 51% of t2
            doL2SeatVote(__LINE__, 1, t2, 4, 4, carol, null_acc, {
                &m11, &m12, &m13 }, false);

            env.close();
            env.close();

            // change our vote
            doL2SeatVote(__LINE__, 1, t2, 4, 4, null_acc, null_acc, {
                &m12, &m13, &m14 }, false);

            env.close();
            env.close();

            // vote at l1
            doL1SeatVote(__LINE__, 4, 4, carol.id(), {}, {&alice, &bob}, false);

            // change our vote again
            doL2SeatVote(__LINE__, 1, t2, 4, 5, carol, null_acc, {
                &m11, &m13, &m14 }, true);

            env.close();
            env.close();

            // check the member was voted in successfully
            {
                std::vector<uint8_t> key { 
                    0,0,0,0, 0,0,0,0,  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x04U };
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key.data()), beast::zero));
                BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == vecFromAcc(carol));
            }
            // check member count
            {
                std::vector<uint8_t> key { 
                    0,0,0,0, 0,0,0,0,  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,'M', 'C' };
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key.data()), beast::zero));
                BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == std::vector<uint8_t>{0x05U});
            }

            // now vote carol out
            doL2SeatVote(__LINE__, 1, t2, 4, 5, null_acc, carol, {
                &m11, &m12, &m13 }, false);

            env.close();
            env.close();

            // vote at l1
            doL1SeatVote(__LINE__, 4, 4, null_acc, carol, {&alice, &carol, &bob}, true);

            env.close();

            // check the member was voted out successfully
            {
                std::vector<uint8_t> key { 
                    0,0,0,0, 0,0,0,0,  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0, 0x04U };
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key.data()), beast::zero));
                BEAST_EXPECT(!entry);
            }
            // check member count
            {
                std::vector<uint8_t> key { 
                    0,0,0,0, 0,0,0,0,  0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,'M', 'C' };
                auto entry = env.le(keylet::hookState(env.master.id(), uint256::fromVoid(key.data()), beast::zero));
                BEAST_EXPECT(!!entry && entry->getFieldVL(sfHookStateData) == std::vector<uint8_t>{0x04U});
            }


        }

        // t2 votes out seat 0
        // floor(0.8*5) = 4
        doL2SeatVote(__LINE__, 2, t2, 0, 4, null_acc, m11, {
            &m12, &m13, &m14, &m15}, true);

        // t2 votes out seat 3
        // floor(0.8*4) = 3
        // first try to use the newly voted out person as a vote
        doL2SeatVote(__LINE__, 2, t2, 4, 3, null_acc, m15, {
            &m11}, false, true);
        doL2SeatVote(__LINE__, 2, t2, 4, 3, null_acc, m15, {
            &m12, &m13, &m14}, true);

        // t2 votes out seat 1
        // floor(0.8*3) = 2
        doL2SeatVote(__LINE__, 2, t2, 1, 2, null_acc, m12, {
            &m12, &m13}, true);

        // t2 = { empty, empty, m13, m14, empty }

        // 2 seats is the minimum so trying to vote out another will fail
        doL2SeatVote(__LINE__, 2, t2, 2, 2, null_acc, m13, {
            &m13}, false, false);
        doL2SeatVote(__LINE__, 2, t2, 2, 2, null_acc, m13, {
            &m14}, false, true);

        // vote in some invalid seats
        doL2SeatVote(__LINE__, 2, t2, 20, 2, m14, null_acc, {
            &m13, &m14}, false, true);
        
        doL2SeatVote(__LINE__, 2, t2, 255, 2, m14, null_acc, {
            &m13, &m14}, false, true);

        // vote in lots of members
        doL2SeatVote(__LINE__, 2, t2, 0, 3, m11, null_acc, {
            &m13, &m14}, true);


        doL2SeatVote(__LINE__, 2, t2, 1, 4, m12, null_acc, {
            &m13, &m14}, true);
        
        doL2SeatVote(__LINE__, 2, t2, 4, 5, m15, null_acc, {
            &m12, &m13, &m14}, true);


        // t2 = { m11, m12, m13, m14, m15 }

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

            std::vector<uint8_t> const null_data
            {
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
                0x00U,0x00U
            };

            doL2Vote(__LINE__, 2, t2, m11, 'H', 7, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 7, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 7, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 7, accept_data, null_data, false);

            env.close();
            env.close();

            // check if the hook not was installed because we have not given it 100% votes yet
            {
                auto const hooks = env.le(keylet::hook(t2.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() == 1);
            }


            // now cast final vote
            doL2Vote(__LINE__, 2, t2, m15, 'H', 7, accept_data, null_data, false);

            env.close();
            env.close();

            // now check it was installed
            {
                auto const hooks = env.le(keylet::hook(t2.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() > 7 &&
                    hooks->getFieldArray(sfHooks)[7].isFieldPresent(sfHookHash) &&
                    hooks->getFieldArray(sfHooks)[7].getFieldH256(sfHookHash) == acceptHookHash);
            }

            // now change a vote (note that the topic state is never recorded for hooks, so old data is null)
            doL2Vote(__LINE__, 2, t2, m11, 'H', 7, null_data, null_data, false);

            env.close();
            env.close();

            // now check it's still installed
            {
                auto const hooks = env.le(keylet::hook(t2.id()));
                BEAST_EXPECT(!!hooks && hooks->getFieldArray(sfHooks).size() > 7 &&
                    hooks->getFieldArray(sfHooks)[7].isFieldPresent(sfHookHash) &&
                    hooks->getFieldArray(sfHooks)[7].getFieldH256(sfHookHash) == acceptHookHash);
            }

            // now vote to delete it
            doL2Vote(__LINE__, 2, t2, m12, 'H', 7, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 7, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 7, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 7, null_data, null_data, false);


            env.close();
            env.close();

            // now check it's still installed
            {
                auto const hooks = env.le(keylet::hook(t2.id()));
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

            doL2Vote(__LINE__, 2, t2, m11, 'H', 1, invalid_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 1, invalid_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 1, invalid_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 1, invalid_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 1, invalid_data, null_data, false, true);


            uint8_t const* gdata = governHookHash.data();
            std::vector<uint8_t> govern_data(gdata, gdata+32);
            uint8_t const* rdata = rewardHookHash.data();
            std::vector<uint8_t> reward_data(rdata, rdata+32);


            // vote to put governance hook into position 2
            doL2Vote(__LINE__, 2, t2, m11, 'H', 2, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 2, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 2, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 2, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 2, govern_data, null_data, false);

            env.close();
            env.close();

            // vote to replace the hook at position 1 with accept
            doL2Vote(__LINE__, 2, t2, m11, 'H', 1, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 1, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 1, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 1, accept_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 1, accept_data, null_data, false);

            env.close();
            env.close();

            // vote to place reward hook at position 2
            doL2Vote(__LINE__, 2, t2, m11, 'H', 0, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 0, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 0, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 0, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 0, reward_data, null_data, false);

            env.close();
            env.close();

            // hooks array should now look like {govern, accept, reward, nothing ...}

            {
                auto const hooksLE = env.le(keylet::hook(t2.id()));
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
            doL2Vote(__LINE__, 2, t2, m11, 'H', 1, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 1, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 1, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 1, reward_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 1, reward_data, null_data, false);

            env.close();
            env.close();

            // set hook 0 back to govern
            doL2Vote(__LINE__, 2, t2, m11, 'H', 0, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 0, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 0, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 0, govern_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 0, govern_data, null_data, false);

            env.close();
            env.close();

            // delete hook at 2
            doL2Vote(__LINE__, 2, t2, m11, 'H', 2, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m12, 'H', 2, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m13, 'H', 2, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m14, 'H', 2, null_data, null_data, false);
            doL2Vote(__LINE__, 2, t2, m15, 'H', 2, null_data, null_data, false);

            env.close();
            env.close();


            // check we're back the way we were
            {
                auto const hooksLE = env.le(keylet::hook(t2.id()));
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
            doL2Vote(__LINE__, 2, t2, m11, 'H', 0, reward_data, null_data, false);
            env.close();
            env.close();
            {
                auto const hooksLE = env.le(keylet::hook(t2.id()));
                BEAST_EXPECT(!!hooksLE && hooksLE->getFieldArray(sfHooks).size() >= 2);

                if (hooksLE && hooksLE->getFieldArray(sfHooks).size() >=2)
                {
                    auto const hooks = hooksLE->getFieldArray(sfHooks);
                    BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == governHookHash);
                    BEAST_EXPECT(hooks[1].getFieldH256(sfHookHash) == rewardHookHash);
                    BEAST_EXPECT(hooks.size() == 2 || !hooks[2].isFieldPresent(sfHookHash));
                }
            }


            // vote to install a hook on L1 from a L2 table
            {
                // alice, bob need to vote, because 100% L1 votes are needed to change a hook
                doL1Vote(__LINE__, alice, 'H', 2, accept_data, null_data, false);
                doL1Vote(__LINE__, bob, 'H', 2, accept_data, null_data, false);

                // 51% of t2 must vote
                doL2Vote(__LINE__, 1, t2, m11, 'H', 2, accept_data, null_data, false);
                doL2Vote(__LINE__, 1, t2, m12, 'H', 2, accept_data, null_data, false);
                doL2Vote(__LINE__, 1, t2, m13, 'H', 2, accept_data, null_data, false);
    

                // 51% of t1 must vote, but vote count cannot be less than 2
                doL2Vote(__LINE__, 1, t1, m6, 'H', 2, accept_data, null_data, false);
                doL2Vote(__LINE__, 1, t1, m7, 'H', 2, accept_data, null_data, false);
    
                env.close();
                env.close();
                env.close();
                env.close();
                env.close();
                env.close();

                // RH NOTE: idk why this ended up being so slow to finalize, probably multiple emitted txns settling
            
                // check it was actioned correctly
                auto genesisHooksLE = env.le(keylet::hook(genesisAccID));
                BEAST_REQUIRE(!!genesisHooksLE);
                auto genesisHookArray = genesisHooksLE->getFieldArray(sfHooks);
                BEAST_EXPECT(genesisHookArray.size() >= 3);
                auto const acceptHash = ripple::sha512Half_s(ripple::Slice(AcceptHook.data(), AcceptHook.size()));
                BEAST_EXPECT(genesisHookArray.size() >= 3 &&
                    genesisHookArray[2].isFieldPresent(sfHookHash) &&
                    genesisHookArray[2].getFieldH256(sfHookHash) == acceptHash);
            }

        }

    }


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
        testGovernanceL1();
        testGovernanceL2();
    }
};

BEAST_DEFINE_TESTSUITE(XahauGenesis, app, ripple);

}  // namespace test
}  // namespace ripple
#undef M
