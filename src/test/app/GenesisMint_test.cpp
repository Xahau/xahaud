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

#include <ripple/protocol/Feature.h>
#include <ripple/app/tx/impl/XahauGenesis.h>

#include <test/jtx.h>
#include <optional>

namespace ripple {
namespace test {
struct GenesisMint_test : public beast::unit_test::suite
{
/*
    void
    injectEmitted(jtx::Env& env, STTx const txIn)
    {
        env.app().openLedger().modify(
            [&](OpenView& view, beast::Journal j) -> bool
            {
                STTx tx (txIn->getTxnType(), [&](auto& obj) {

                    obj = txIn;


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
    }
*/

    
    struct GenMint
    {
        std::optional<std::string> dest;
        std::optional<jtx::PrettyAmount> amt;
        std::optional<std::string> marks;
        std::optional<std::string> flags;

        GenMint(AccountID const& dst, jtx::PrettyAmount const& x,
            std::optional<std::string> m = std::nullopt, std::optional<std::string> f = std::nullopt):
            dest(toBase58(dst)), amt(x), marks(m), flags(f)
        {
        }
    };

    Json::Value
    mint(jtx::Account const& account, std::vector<GenMint> mints)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::GenesisMint;
        jv[jss::Account] = account.human();
        jv[jss::GenesisMints] = Json::arrayValue;

        uint32_t counter = 0;
        for (auto const& m: mints)
        {
            Json::Value inner = Json::objectValue;
            if (m.dest)
                inner[jss::Destination] = *m.dest;
            if (m.amt)
                inner[jss::Amount] = (*m.amt).value().getJson(JsonOptions::none);
            if (m.marks)
                inner[jss::GovernanceMarks] = *m.marks;
            if (m.flags)
                inner[jss::GovernanceFlags] = *m.flags;

            jv[jss::GenesisMints][counter] = Json::objectValue;
            jv[jss::GenesisMints][counter][jss::GenesisMint] = inner;
            counter++;
        }
        return jv;
    }

    Json::Value
    setMintHook(jtx::Account const& account)
    {
        using namespace jtx;
        Json::Value tx;
        tx[jss::Account] = account.human();
        tx[jss::TransactionType] = "SetHook";
        tx[jss::Hooks] = Json::arrayValue;
        tx[jss::Hooks][0u] = Json::objectValue;
        tx[jss::Hooks][0u][jss::Hook] = Json::objectValue;
        tx[jss::Hooks][0u][jss::Hook][jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        tx[jss::Hooks][0u][jss::Hook][jss::HookNamespace] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        tx[jss::Hooks][0u][jss::Hook][jss::HookApiVersion] = 0;
        
        tx[jss::Hooks][0u][jss::Hook][jss::CreateCode] = strHex(XahauGenesis::MintTestHook);
        return tx;
    }

    void
    testDisabled(FeatureBitset features)
    {
        testcase("Disabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        {
            test::jtx::Env env(*this, features - featureXahauGenesis);

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(10000), alice, bob);
            env.close();

            env(mint(env.master, {
                GenMint(bob.id(), XRP(123))
            }), ter(temDISABLED));
        }

        {
            test::jtx::Env env(*this, features - featureHooks);

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(10000), alice, bob);
            env.close();

            env(mint(env.master, {
                {bob.id(), XRP(123)}
            }), ter(temDISABLED));
        }
    }

    std::string
    makeBlob(std::vector<std::pair<AccountID, STAmount>> entries)
    {
        std::string blob = "F060";

        for (auto const& e : entries)
        {
            STObject m(sfGenesisMint);
            m.setAccountID(sfDestination, e.first);
            m.setFieldAmount(sfAmount, e.second);
            Serializer s;
            m.add(s);
            blob += "E060" + strHex(s.getData()) + "E1";
        }

        blob += "F1";
        return blob;
    }


    Json::Value
    invoke(jtx::Account const& account, jtx::Account const& destination, std::string blob)
    {
        Json::Value tx = Json::objectValue;
        tx[jss::Account] = account.human();
        tx[jss::TransactionType] = "Invoke";
        tx[jss::Destination] = destination.human();
        tx[jss::Blob] = blob;
        return tx;
    }

    void
    testNonGenesisEmit(FeatureBitset features)
    {
        testcase("Non Genesis Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features, nullptr,
            beast::severities::kWarning
//            beast::severities::kTrace
        }; 
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        env.fund(XRP(10000), alice, bob, invoker);
        env.close();
        
        // set the test hook
        env(setMintHook(alice), fee(XRP(10)));
        env.close();

        // this should fail because emitted txns are preflighted
        // and the preflight checks the account and will find it's not genesis
        env(invoke(invoker, alice,
            makeBlob(
            {
                {bob.id(), XRP(123).value()},
            })), fee(XRP(1)), ter(tecHOOK_REJECTED));
    }

    void
    testGenesisEmit(FeatureBitset features)
    {
        testcase("Genesis Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features, nullptr,
//            beast::severities::kWarning
            beast::severities::kTrace
        }; 
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        env.fund(XRP(10000), alice, bob, invoker);
        env.close();
        
        // set the test hook
        env(setMintHook(env.master), fee(XRP(10)));
        env.close();

        {
            auto acc = env.le(keylet::account(bob.id()));
            BEAST_EXPECT(acc->getFieldAmount(sfBalance).xrp().drops() == 10000000000ULL);
        }

        // this should fail because emitted txns are preflighted
        // and the preflight checks the account and will find it's not genesis
        env(invoke(invoker, env.master,
            makeBlob(
            {
                {bob.id(), XRP(123).value()},
            })), fee(XRP(1)), ter(tesSUCCESS));

        env.close();
        env.close();

        {
            auto acc = env.le(keylet::account(bob.id()));
            BEAST_EXPECT(acc->getFieldAmount(sfBalance).xrp().drops() == 10123000000ULL);
        }
    }

        // RH UPTO: make a blob
        // invoke
        // get emit txn hash
        // close ledger
        // test emit failure
        
    

/*
    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337), features, nullptr, beast::severities::kTrace};

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const issuer = Account("issuer");

        env.fund(XRP(10000), alice, bob, issuer);
        env.close();

        // set claim rewards
        auto claimTx = claim(alice);
        claimTx[sfIssuer.jsonName] = issuer.human();
        env(claimTx, ter(tesSUCCESS));
        env.close();

        // burn fee
        env(noop(alice), fee(1'000'000'000), ter(tesSUCCESS));
        env.close();

        // mint rewards
        auto mintTx = mint(alice);

        // Emit Details
        mintTx[sfEmitDetails.jsonName] = Json::objectValue;
        mintTx[sfEmitDetails.jsonName][sfEmitBurden.jsonName] = "1";
        mintTx[sfEmitDetails.jsonName][sfEmitCallback.jsonName] = "rMPwD1b8dJUaqZHaBgEvFx4ENhtpPVvDsv";
        mintTx[sfEmitDetails.jsonName][sfEmitGeneration.jsonName] = 1;
        mintTx[sfEmitDetails.jsonName][sfEmitHookHash.jsonName] = "A9B5411F4A4368008B4736EEE47A34B0EFCBE74016B9B94CC6208FBC0BF5C0C2";
        mintTx[sfEmitDetails.jsonName][sfEmitNonce.jsonName] = "B8415274BAA38DC4640C6C35935C55FFAE8A6C13009736FCBE562A18376034FE";
        mintTx[sfEmitDetails.jsonName][sfEmitParentTxnID.jsonName] = "013F3650533681369F9D55FBFEEECF2C3AF1E9F3DAABB4F0C5C0F638321ABC7F";

        // Genesis Mints
        mintTx[sfGenesisMints.jsonName] = Json::arrayValue;
        mintTx[sfGenesisMints.jsonName][0u] = Json::objectValue;
        mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName] = Json::objectValue;
        mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][jss::Destination] = bob.human();
        mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][jss::Amount] = to_string(10000000);
        // mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][sfGovernanceFlags.jsonName] = "";
        // mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][sfGovernanceMarks.jsonName] = "";
        
        // mintTx[sfFlags.jsonName] = to_string(2147483648);

        // JTx jt = env.jt(mintTx);
        // jt.fill_sig = false;
        // auto const jrr = env.rpc("submit", strHex(jt.stx->getSerializer().slice()))[jss::result];
        // std::cout << "jrr: " << jrr << "\n";
        
        auto const tx_blob = "1200602100005359240000000368400000000000000A73210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C0528114AE123A8556F3CF91154711376AFB0F894F832B3DED202E000000013D00000000000000015B013F3650533681369F9D55FBFEEECF2C3AF1E9F3DAABB4F0C5C0F638321ABC7F5CB8415274BAA38DC4640C6C35935C55FFAE8A6C13009736FCBE562A18376034FE5DA9B5411F4A4368008B4736EEE47A34B0EFCBE74016B9B94CC6208FBC0BF5C0C28A14DF935D6B794939ABBA5D13007E65C2C6E70C625DE1F060E0606140000000009896808314F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1F1";
        auto const jrr = env.rpc("submit", tx_blob)[jss::result];
        std::cout << "jrr: " << jrr << "\n";
        // env(mintTx, ter(tesSUCCESS));

        env.close();

        // for (bool const withHooksGenesis : {false, true})
        // {
        //     // If the Hooks or the Xahau Genesis amendments are not enabled, you
        //     // should not be able to mint xrp.
        //     auto const amend =
        //         withHooksGenesis ? features : features - featureHooks;
        //     Env env{*this, amend};

        //     env.fund(XRP(1000), alice, bob);
        //     env.close();

            // MINT
            // auto tx = mint(alice);
            // tx[sfIssuer.jsonName] = issuer.human();
            // env(tx, txResult);
            // env.close();

        //     // auto const txResult =
        //     //     withHooksGenesis ? ter(tesSUCCESS) : ter(temDISABLED);
        // }
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("invalid preflight");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED
        // featureHooks

        // temDISABLED
        // featureXahauGenesis

        // temMALFORMED
        // can only be used by the genesis account in an emitted transaction.

        // temMALFORMED
        // destinations array empty.

        // temMALFORMED
        // destinations array exceeds 512 entries.

        // temMALFORMED
        // destinations array contained an invalid entry.

        // temMALFORMED
        // each destination must have at least one of: sfAmount, sfGovernanceFlags, sfGovernance marks
        
        // temMALFORMED
        // only native amounts can be minted.

        // temMALFORMED
        // only positive amounts can be minted.

        // temMALFORMED
        // destinations includes disallowed account zero or one.
        
        // temMALFORMED
        // duplicate in destinations.
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("invalid preclaim");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED
        // featureHooks

        // temDISABLED
        // featureXahauGenesis
    }

    void
    testInvalidDoApply(FeatureBitset features)
    {
        testcase("invalid doApply");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preflight

        // tecINTERNAL
        // Non-xrp amount.

        // tecINTERNAL
        // cannot credit " << dest << " due to balance overflow

        // tecINTERNAL
        // dropsAdded overflowed\n
    }

    void
    testWithFeats(FeatureBitset features)
    {
        BEAST_EXPECT(1 == 1);
        // testEnabled(features);
    }
*/
public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testDisabled(sa);
        testNonGenesisEmit(sa);
        testGenesisEmit(sa);
    }
};

BEAST_DEFINE_TESTSUITE(GenesisMint, app, ripple);

}  // namespace test
}  // namespace ripple
