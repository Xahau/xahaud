//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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

#include <ripple/app/hook/Enum.h>
#include <ripple/app/misc/Manifest.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/beast/unit_test.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>

namespace ripple {

namespace test {

class LedgerRPC_test : public beast::unit_test::suite
{
public:
#define HSFEE fee(100'000'000)
    void
    checkErrorValue(
        Json::Value const& jv,
        std::string const& err,
        std::string const& msg)
    {
        if (BEAST_EXPECT(jv.isMember(jss::status)))
            BEAST_EXPECT(jv[jss::status] == "error");
        if (BEAST_EXPECT(jv.isMember(jss::error)))
            BEAST_EXPECT(jv[jss::error] == err);
        if (msg.empty())
        {
            BEAST_EXPECT(
                jv[jss::error_message] == Json::nullValue ||
                jv[jss::error_message] == "");
        }
        else if (BEAST_EXPECT(jv.isMember(jss::error_message)))
            BEAST_EXPECT(jv[jss::error_message] == msg);
    }

    // Corrupt a valid address by replacing the 10th character with '!'.
    // '!' is not part of the ripple alphabet.
    std::string
    makeBadAddress(std::string good)
    {
        std::string ret = std::move(good);
        ret.replace(10, 1, 1, '!');
        return ret;
    }

    void
    testLedgerRequest()
    {
        testcase("Basic Request");
        using namespace test::jtx;

        Env env{*this};

        env.close();
        BEAST_EXPECT(env.current()->info().seq == 4);

        {
            Json::Value jvParams;
            // can be either numeric or quoted numeric
            jvParams[jss::ledger_index] = 1;
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::ledger][jss::closed] == true);
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "1");
        }

        {
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "1";
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::ledger][jss::closed] == true);
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "1");
        }

        {
            // using current identifier
            auto const jrr = env.rpc("ledger", "current")[jss::result];
            BEAST_EXPECT(jrr[jss::ledger][jss::closed] == false);
            BEAST_EXPECT(
                jrr[jss::ledger][jss::ledger_index] ==
                std::to_string(env.current()->info().seq));
            BEAST_EXPECT(
                jrr[jss::ledger_current_index] == env.current()->info().seq);
        }
    }

    void
    testBadInput()
    {
        testcase("Bad Input");
        using namespace test::jtx;
        Env env{*this};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        Account const bob{"bob"};

        env.fund(XRP(10000), gw, bob);
        env.close();
        env.trust(USD(1000), bob);
        env.close();

        {
            // ask for an arbitrary string - index
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "potato";
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "invalidParams", "ledgerIndexMalformed");
        }

        {
            // ask for a negative index
            Json::Value jvParams;
            jvParams[jss::ledger_index] = -1;
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "invalidParams", "ledgerIndexMalformed");
        }

        {
            // ask for a bad ledger index
            Json::Value jvParams;
            jvParams[jss::ledger_index] = 10u;
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "lgrNotFound", "ledgerNotFound");
        }

        {
            // unrecognized string arg -- error
            auto const jrr = env.rpc("ledger", "arbitrary_text")[jss::result];
            checkErrorValue(jrr, "lgrNotFound", "ledgerNotFound");
        }

        {
            // Request queue for closed ledger
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "validated";
            jvParams[jss::queue] = true;
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "invalidParams", "Invalid parameters.");
        }

        {
            // Request a ledger with a very large (double) sequence.
            auto const ret =
                env.rpc("json", "ledger", "{ \"ledger_index\" : 2e15 }");
            BEAST_EXPECT(RPC::contains_error(ret));
            BEAST_EXPECT(ret[jss::error_message] == "Invalid parameters.");
        }

        {
            // Request a ledger with very large (integer) sequence.
            auto const ret = env.rpc(
                "json", "ledger", "{ \"ledger_index\" : 1000000000000000 }");
            checkErrorValue(ret, "invalidParams", "Invalid parameters.");
        }
    }

    void
    testLedgerCurrent()
    {
        testcase("ledger_current Request");
        using namespace test::jtx;

        Env env{*this};

        env.close();
        BEAST_EXPECT(env.current()->info().seq == 4);

        {
            auto const jrr = env.rpc("ledger_current")[jss::result];
            BEAST_EXPECT(
                jrr[jss::ledger_current_index] == env.current()->info().seq);
        }
    }

    void
    testMissingLedgerEntryLedgerHash()
    {
        testcase("Missing ledger_entry ledger_hash");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        Json::Value jvParams;
        jvParams[jss::account_root] = alice.human();
        jvParams[jss::ledger_hash] =
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
        auto const jrr =
            env.rpc("json", "ledger_entry", to_string(jvParams))[jss::result];
        checkErrorValue(jrr, "lgrNotFound", "ledgerNotFound");
    }

    void
    testLedgerFull()
    {
        testcase("Ledger Request, Full Option");
        using namespace test::jtx;

        Env env{*this};

        env.close();

        Json::Value jvParams;
        jvParams[jss::ledger_index] = 3u;
        jvParams[jss::full] = true;
        auto const jrr =
            env.rpc("json", "ledger", to_string(jvParams))[jss::result];
        BEAST_EXPECT(jrr[jss::ledger].isMember(jss::accountState));
        BEAST_EXPECT(jrr[jss::ledger][jss::accountState].isArray());
        BEAST_EXPECT(jrr[jss::ledger][jss::accountState].size() == 3u);
    }

    void
    testLedgerFullNonAdmin()
    {
        testcase("Ledger Request, Full Option Without Admin");
        using namespace test::jtx;

        Env env{*this, envconfig(no_admin_networkid)};

        //        env.close();

        Json::Value jvParams;
        jvParams[jss::ledger_index] = 1u;
        jvParams[jss::full] = true;
        auto const jrr =
            env.rpc("json", "ledger", to_string(jvParams))[jss::result];
        checkErrorValue(
            jrr, "noPermission", "You don't have permission for this command.");
    }

    void
    testLedgerAccounts()
    {
        testcase("Ledger Request, Accounts Option");
        using namespace test::jtx;

        Env env{*this};

        env.close();

        Json::Value jvParams;
        jvParams[jss::ledger_index] = 3u;
        jvParams[jss::accounts] = true;
        auto const jrr =
            env.rpc("json", "ledger", to_string(jvParams))[jss::result];
        BEAST_EXPECT(jrr[jss::ledger].isMember(jss::accountState));
        BEAST_EXPECT(jrr[jss::ledger][jss::accountState].isArray());
        BEAST_EXPECT(jrr[jss::ledger][jss::accountState].size() == 3u);
    }

    void
    testLedgerEntryAccountRoot()
    {
        testcase("ledger_entry Request AccountRoot");
        using namespace test::jtx;
        Env env{*this, supported_amendments() - featureXahauGenesis};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        {
            // Exercise ledger_closed along the way.
            Json::Value const jrr = env.rpc("ledger_closed")[jss::result];
            BEAST_EXPECT(jrr[jss::ledger_hash] == ledgerHash);
            BEAST_EXPECT(jrr[jss::ledger_index] == 3);
        }

        std::string accountRootIndex;
        {
            // Request alice's account root.
            Json::Value jvParams;
            jvParams[jss::account_root] = alice.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::node));
            BEAST_EXPECT(jrr[jss::node][jss::Account] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfBalance.jsonName] == "10000000000");
            accountRootIndex = jrr[jss::index].asString();
        }
        {
            constexpr char alicesAcctRootBinary[]{
                "1100612200800000240000000425000000032D00000000559CE54C3B934E4"
                "73A995B477E92EC229F99CED5B62BF4D2ACE4DC42719103AE2F6240000002"
                "540BE4008114AE123A8556F3CF91154711376AFB0F894F832B3D"};

            // Request alice's account root, but with binary == true;
            Json::Value jvParams;
            jvParams[jss::account_root] = alice.human();
            jvParams[jss::binary] = 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::node_binary));
            BEAST_EXPECT(jrr[jss::node_binary] == alicesAcctRootBinary);
        }
        {
            // Request alice's account root using the index.
            Json::Value jvParams;
            jvParams[jss::index] = accountRootIndex;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(!jrr.isMember(jss::node_binary));
            BEAST_EXPECT(jrr.isMember(jss::node));
            BEAST_EXPECT(jrr[jss::node][jss::Account] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfBalance.jsonName] == "10000000000");
        }
        {
            // Request alice's account root by index, but with binary == false.
            Json::Value jvParams;
            jvParams[jss::index] = accountRootIndex;
            jvParams[jss::binary] = 0;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::node));
            BEAST_EXPECT(jrr[jss::node][jss::Account] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfBalance.jsonName] == "10000000000");
        }
        {
            // Request using a corrupted AccountID.
            Json::Value jvParams;
            jvParams[jss::account_root] = makeBadAddress(alice.human());
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // Request an account that is not in the ledger.
            Json::Value jvParams;
            jvParams[jss::account_root] = Account("bob").human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryCheck()
    {
        testcase("ledger_entry Request Check");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const checkId = keylet::check(env.master, env.seq(env.master));

        env(check::create(env.master, alice, XRP(100)));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        {
            // Request a check.
            Json::Value jvParams;
            jvParams[jss::check] = to_string(checkId.key);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::node][sfLedgerEntryType.jsonName] == jss::Check);
            BEAST_EXPECT(jrr[jss::node][sfSendMax.jsonName] == "100000000");
        }
        {
            // Request an index that is not a check.  We'll use alice's
            // account root index.
            std::string accountRootIndex;
            {
                Json::Value jvParams;
                jvParams[jss::account_root] = alice.human();
                Json::Value const jrr = env.rpc(
                    "json", "ledger_entry", to_string(jvParams))[jss::result];
                accountRootIndex = jrr[jss::index].asString();
            }
            Json::Value jvParams;
            jvParams[jss::check] = accountRootIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "unexpectedLedgerType", "");
        }
    }

    void
    testLedgerEntryDepositPreauth()
    {
        testcase("ledger_entry Deposit Preauth");

        using namespace test::jtx;

        Env env{*this};
        Account const alice{"alice"};
        Account const becky{"becky"};

        env.fund(XRP(10000), alice, becky);
        env.close();

        env(deposit::auth(alice, becky));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        std::string depositPreauthIndex;
        {
            // Request a depositPreauth by owner and authorized.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::owner] = alice.human();
            jvParams[jss::deposit_preauth][jss::authorized] = becky.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];

            BEAST_EXPECT(
                jrr[jss::node][sfLedgerEntryType.jsonName] ==
                jss::DepositPreauth);
            BEAST_EXPECT(jrr[jss::node][sfAccount.jsonName] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfAuthorize.jsonName] == becky.human());
            depositPreauthIndex = jrr[jss::node][jss::index].asString();
        }
        {
            // Request a depositPreauth by index.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth] = depositPreauthIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];

            BEAST_EXPECT(
                jrr[jss::node][sfLedgerEntryType.jsonName] ==
                jss::DepositPreauth);
            BEAST_EXPECT(jrr[jss::node][sfAccount.jsonName] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfAuthorize.jsonName] == becky.human());
        }
        {
            // Malformed request: deposit_preauth neither object nor string.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth] = -5;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed request: deposit_preauth not hex string.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth] = "0123456789ABCDEFG";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed request: missing [jss::deposit_preauth][jss::owner]
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::authorized] = becky.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed request: [jss::deposit_preauth][jss::owner] not string.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::owner] = 7;
            jvParams[jss::deposit_preauth][jss::authorized] = becky.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed: missing [jss::deposit_preauth][jss::authorized]
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::owner] = alice.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed: [jss::deposit_preauth][jss::authorized] not string.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::owner] = alice.human();
            jvParams[jss::deposit_preauth][jss::authorized] = 47;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed: [jss::deposit_preauth][jss::owner] is malformed.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::owner] =
                "rP6P9ypfAmc!pw8SZHNwM4nvZHFXDraQas";

            jvParams[jss::deposit_preauth][jss::authorized] = becky.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedOwner", "");
        }
        {
            // Malformed: [jss::deposit_preauth][jss::authorized] is malformed.
            Json::Value jvParams;
            jvParams[jss::deposit_preauth][jss::owner] = alice.human();
            jvParams[jss::deposit_preauth][jss::authorized] =
                "rP6P9ypfAmc!pw8SZHNwM4nvZHFXDraQas";

            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAuthorized", "");
        }
    }

    void
    testLedgerEntryDirectory()
    {
        testcase("ledger_entry Request Directory");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();

        env.trust(USD(1000), alice);
        env.close();

        // Run up the number of directory entries so alice has two
        // directory nodes.
        for (int d = 1'000'032; d >= 1'000'000; --d)
        {
            env(offer(alice, USD(1), drops(d)));
        }
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        {
            // Exercise ledger_closed along the way.
            Json::Value const jrr = env.rpc("ledger_closed")[jss::result];
            BEAST_EXPECT(jrr[jss::ledger_hash] == ledgerHash);
            BEAST_EXPECT(jrr[jss::ledger_index] == 5);
        }

        std::string const dirRootIndex =
            "A33EC6BB85FB5674074C4A3A43373BB17645308F3EAE1933E3E35252162B217D";
        {
            // Locate directory by index.
            Json::Value jvParams;
            jvParams[jss::directory] = dirRootIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][sfIndexes.jsonName].size() == 32);
        }
        {
            // Locate directory by directory root.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::dir_root] = dirRootIndex;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::index] == dirRootIndex);
        }
        {
            // Locate directory by owner.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::owner] = alice.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::index] == dirRootIndex);
        }
        {
            // Locate directory by directory root and sub_index.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::dir_root] = dirRootIndex;
            jvParams[jss::directory][jss::sub_index] = 1;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::index] != dirRootIndex);
            BEAST_EXPECT(jrr[jss::node][sfIndexes.jsonName].size() == 2);
        }
        {
            // Locate directory by owner and sub_index.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::owner] = alice.human();
            jvParams[jss::directory][jss::sub_index] = 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::index] != dirRootIndex);
            BEAST_EXPECT(jrr[jss::node][sfIndexes.jsonName].size() == 2);
        }
        {
            // Null directory argument.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::nullValue;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Non-integer sub_index.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::dir_root] = dirRootIndex;
            jvParams[jss::directory][jss::sub_index] = 1.5;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed owner entry.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;

            std::string const badAddress = makeBadAddress(alice.human());
            jvParams[jss::directory][jss::owner] = badAddress;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // Malformed directory object.  Specify both dir_root and owner.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::owner] = alice.human();
            jvParams[jss::directory][jss::dir_root] = dirRootIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Incomplete directory object.  Missing both dir_root and owner.
            Json::Value jvParams;
            jvParams[jss::directory] = Json::objectValue;
            jvParams[jss::directory][jss::sub_index] = 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
    }

    void
    testLedgerEntryEmittedTxn()
    {
        testcase("ledger_entry Request Emitted Txn");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this};
        Account const alice{"alice"};

        env.fund(XRP(10000), alice);
        env.close();

        auto setHook = [](test::jtx::Account const& account) {
            std::string const createCodeHex =
                "0061736D01000000012E0760057F7F7F7F7F017E60017F017E60047F7F7F7F"
                "017E60037F7F7E017E6000017E60027F7F017E60027F7F017F02AD010B0365"
                "6E76057472616365000003656E760C6574786E5F7265736572766500010365"
                "6E760A7574696C5F6163636964000203656E760974726163655F6E756D0003"
                "03656E760A6C65646765725F736571000403656E760C686F6F6B5F6163636F"
                "756E74000503656E760C6574786E5F64657461696C73000503656E760D6574"
                "786E5F6665655F62617365000503656E7604656D6974000203656E76025F67"
                "000603656E7606616363657074000303030201010503010002062B077F0141"
                "B089040B7F004180080B7F0041AF090B7F004180080B7F0041B089040B7F00"
                "41000B7F0041010B070F02046362616B000B04686F6F6B000C0A8F900002AC"
                "800001017F230041106B220124002001200036020C419409411A41E9084119"
                "410010001A200141106A240042000BDC8F0001017F230041A0046B22012400"
                "2001200036029C04418209411141B6084110410010001A410110011A200120"
                "014180046A411441C608412310023703F803200142E8073703F00341A80841"
                "0D20012903F00310031A2001200141E0016A22003602DC01200120012903F0"
                "033703B801200141003602B401200141003602B001200110043E02AC012001"
                "41C0016A411410051A200141003A00AB0120012802DC0141123A0000200128"
                "02DC0120012D00AB014108763A000120012802DC0120012D00AB013A000220"
                "0120012802DC0141036A3602DC0120014180808080783602A401200141023A"
                "00A30120012802DC0120012D00A301410F7141206A3A000020012802DC0120"
                "012802A4014118763A000120012802DC0120012802A4014110763A00022001"
                "2802DC0120012802A4014108763A000320012802DC0120012802A4013A0004"
                "200120012802DC0141056A3602DC01200120012802B00136029C0120014103"
                "3A009B0120012802DC0120012D009B01410F7141206A3A000020012802DC01"
                "200128029C014118763A000120012802DC01200128029C014110763A000220"
                "012802DC01200128029C014108763A000320012802DC01200128029C013A00"
                "04200120012802DC0141056A3602DC012001410036029401200141043A0093"
                "0120012802DC0120012D009301410F7141206A3A000020012802DC01200128"
                "0294014118763A000120012802DC012001280294014110763A000220012802"
                "DC012001280294014108763A000320012802DC012001280294013A00042001"
                "20012802DC0141056A3602DC01200120012802B40136028C012001410E3A00"
                "8B0120012802DC0120012D008B01410F7141206A3A000020012802DC012001"
                "28028C014118763A000120012802DC01200128028C014110763A0002200128"
                "02DC01200128028C014108763A000320012802DC01200128028C013A000420"
                "0120012802DC0141056A3602DC01200120012802AC0141016A360284012001"
                "411A3A00830120012802DC0141203A000020012802DC0120012D0083013A00"
                "0120012802DC012001280284014118763A000220012802DC01200128028401"
                "4110763A000320012802DC012001280284014108763A000420012802DC0120"
                "01280284013A0005200120012802DC0141066A3602DC01200120012802AC01"
                "41056A36027C2001411B3A007B20012802DC0141203A000020012802DC0120"
                "012D007B3A000120012802DC01200128027C4118763A000220012802DC0120"
                "0128027C4110763A000320012802DC01200128027C4108763A000420012802"
                "DC01200128027C3A0005200120012802DC0141066A3602DC01200141013A00"
                "7A200120012903B80137037020012802DC0120012D007A410F7141E0006A3A"
                "000020012802DC012001290370423888423F8342407D3C000120012802DC01"
                "200129037042308842FF01833C000220012802DC01200129037042288842FF"
                "01833C000320012802DC01200129037042208842FF01833C000420012802DC"
                "01200129037042188842FF01833C000520012802DC01200129037042108842"
                "FF01833C000620012802DC01200129037042088842FF01833C000720012802"
                "DC01200129037042FF01833C0008200120012802DC0141096A3602DC012001"
                "20012802DC0136026C200141083A006B2001420037036020012802DC012001"
                "2D006B410F7141E0006A3A000020012802DC012001290360423888423F8342"
                "407D3C000120012802DC01200129036042308842FF01833C000220012802DC"
                "01200129036042288842FF01833C000320012802DC01200129036042208842"
                "FF01833C000420012802DC01200129036042188842FF01833C000520012802"
                "DC01200129036042108842FF01833C000620012802DC012001290360420888"
                "42FF01833C000720012802DC01200129036042FF01833C0008200120012802"
                "DC0141096A3602DC0120012802DC0141F3003A000020012802DC0141213A00"
                "0120012802DC01420037030220012802DC01420037030A20012802DC014200"
                "37031220012802DC014200370319200120012802DC0141236A3602DC012001"
                "41013A005F20012802DC0120012D005F4180016A3A000020012802DC014114"
                "3A000120012802DC0120012903C00137030220012802DC0120012903C80137"
                "030A20012802DC0120012802D001360212200120012802DC0141166A3602DC"
                "01200141033A005E20012802DC0120012D005E4180016A3A000020012802DC"
                "0141143A000120012802DC0120012903800437030220012802DC0120012903"
                "880437030A20012802DC01200128029004360212200120012802DC0141166A"
                "3602DC012001418E0220012802DC0120006B6BAD370350200120012802DC01"
                "2001290350A7100637034820012000418E021007370340200141083A003F20"
                "012001290340370330200128026C20012D003F410F7141E0006A3A00002001"
                "28026C2001290330423888423F8342407D3C0001200128026C200129033042"
                "308842FF01833C0002200128026C200129033042288842FF01833C00032001"
                "28026C200129033042208842FF01833C0004200128026C2001290330421888"
                "42FF01833C0005200128026C200129033042108842FF01833C000620012802"
                "6C200129033042088842FF01833C0007200128026C200129033042FF01833C"
                "00082001200128026C41096A36026C2001200141106A41202000418E021008"
                "370308418008410B200129030810031A41012200200010091A418C08411C42"
                "00100A1A200141A0046A240042000B0BB60101004180080BAE01656D69745F"
                "726573756C7400436172626F6E3A20456D6974746564207472616E73616374"
                "696F6E0064726F70735F746F5F73656E6400436172626F6E3A207374617274"
                "6564007266436172626F6E564E547558636B5836783271544D466D46536E6D"
                "36644557475800436172626F6E3A2063616C6C6261636B2063616C6C65642E"
                "0022436172626F6E3A2073746172746564220022436172626F6E3A2063616C"
                "6C6261636B2063616C6C65642E22";
            Json::Value jhv = hso(createCodeHex);
            jhv[jss::HookOn] =
                "fffffffffffffffffffffffffffffffffffffff7ffffffffffffffffffbfff"
                "ff";
            Json::Value jv = ripple::test::jtx::hook(account, {{jhv}}, 0);
            return jv;
        };

        env(setHook(alice), HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, fee(XRP(1000)));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        std::optional<uint256> emithash;
        {
            auto meta = env.meta();
            auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
            for (auto const& node : meta->getFieldArray(sfAffectedNodes))
            {
                SField const& metaType = node.getFName();
                uint16_t nodeType = node.getFieldU16(sfLedgerEntryType);
                if (metaType == sfCreatedNode && nodeType == ltEMITTED_TXN)
                {
                    auto const& nf = const_cast<ripple::STObject&>(node)
                                         .getField(sfNewFields)
                                         .downcast<STObject>();
                    auto const& et = const_cast<ripple::STObject&>(nf)
                                         .getField(sfEmittedTxn)
                                         .downcast<STObject>();
                    Blob txBlob = et.getSerializer().getData();
                    auto const tx = std::make_unique<STTx>(
                        Slice{txBlob.data(), txBlob.size()});
                    emithash = tx->getTransactionID();
                    break;
                }
            }
        }

        std::string emittedTxnIndex = to_string(*emithash);
        {
            // Request the emitted txn using its index.
            Json::Value jvParams;
            jvParams[jss::emitted_txn] = emittedTxnIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            Json::Value const emtTx = jrr[jss::node][jss::EmittedTxn];
            BEAST_EXPECT(emtTx[sfAccount.jsonName] == alice.human());
            BEAST_EXPECT(emtTx[sfAmount.jsonName] == "1000");
            BEAST_EXPECT(
                emtTx[sfDestination.jsonName] ==
                "rfCarbonVNTuXckX6x2qTMFmFSnm6dEWGX");
            BEAST_EXPECT(emtTx[sfDestinationTag.jsonName] == 0);
        }
        {
            // Request an index that is not a emitted txn.
            Json::Value jvParams;
            jvParams[jss::emitted_txn] = ledgerHash;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryEscrow()
    {
        testcase("ledger_entry Request Escrow");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Lambda to create an escrow.
        auto escrowCreate = [](test::jtx::Account const& account,
                               test::jtx::Account const& to,
                               STAmount const& amount,
                               NetClock::time_point const& cancelAfter) {
            Json::Value jv;
            jv[jss::TransactionType] = jss::EscrowCreate;
            jv[jss::Flags] = tfUniversal;
            jv[jss::Account] = account.human();
            jv[jss::Destination] = to.human();
            jv[jss::Amount] = amount.getJson(JsonOptions::none);
            jv[sfFinishAfter.jsonName] =
                cancelAfter.time_since_epoch().count() + 2;
            return jv;
        };

        using namespace std::chrono_literals;
        env(escrowCreate(alice, alice, XRP(333), env.now() + 2s));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        std::string escrowIndex;
        {
            // Request the escrow using owner and sequence.
            Json::Value jvParams;
            jvParams[jss::escrow] = Json::objectValue;
            jvParams[jss::escrow][jss::owner] = alice.human();
            jvParams[jss::escrow][jss::seq] = env.seq(alice) - 1;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::node][jss::Amount] == XRP(333).value().getText());
            escrowIndex = jrr[jss::index].asString();
        }
        {
            // Request the escrow by index.
            Json::Value jvParams;
            jvParams[jss::escrow] = escrowIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::node][jss::Amount] == XRP(333).value().getText());
        }
        {
            // Malformed owner entry.
            Json::Value jvParams;
            jvParams[jss::escrow] = Json::objectValue;

            std::string const badAddress = makeBadAddress(alice.human());
            jvParams[jss::escrow][jss::owner] = badAddress;
            jvParams[jss::escrow][jss::seq] = env.seq(alice) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedOwner", "");
        }
        {
            // Missing owner.
            Json::Value jvParams;
            jvParams[jss::escrow] = Json::objectValue;
            jvParams[jss::escrow][jss::seq] = env.seq(alice) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Missing sequence.
            Json::Value jvParams;
            jvParams[jss::escrow] = Json::objectValue;
            jvParams[jss::escrow][jss::owner] = alice.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Non-integer sequence.
            Json::Value jvParams;
            jvParams[jss::escrow] = Json::objectValue;
            jvParams[jss::escrow][jss::owner] = alice.human();
            jvParams[jss::escrow][jss::seq] =
                std::to_string(env.seq(alice) - 1);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
    }

    void
    testLedgerEntryHook()
    {
        testcase("ledger_entry Request Hook");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Lambda to create a hook.
        auto setHook = [](test::jtx::Account const& account) {
            std::string const createCodeHex =
                "0061736D0100000001130360037F7F7E017E60027F7F017F60017F017E0217"
                "0203656E7606616363657074000003656E76025F6700010302010205030100"
                "02062B077F01418088040B7F004180080B7F004180080B7F004180080B7F00"
                "418088040B7F0041000B7F0041010B07080104686F6F6B00020AB5800001B1"
                "800001017F230041106B220124002001200036020C41002200200042001000"
                "1A41012200200010011A200141106A240042000B";
            Json::Value jv =
                ripple::test::jtx::hook(account, {{hso(createCodeHex)}}, 0);
            return jv;
        };

        using namespace std::chrono_literals;
        env(setHook(alice), HSFEE);
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        std::string hookIndex;
        {
            // Request the hook using account.
            Json::Value jvParams;
            jvParams[jss::hook] = Json::objectValue;
            jvParams[jss::hook][jss::account] = alice.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][jss::Account] == alice.human());
            hookIndex = jrr[jss::index].asString();
        }
        {
            // Request the hook by index.
            Json::Value jvParams;
            jvParams[jss::hook] = hookIndex;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][jss::Account] == alice.human());
        }
        {
            // Malformed account entry.
            Json::Value jvParams;
            jvParams[jss::hook] = Json::objectValue;
            std::string const badAddress = makeBadAddress(alice.human());
            jvParams[jss::hook][jss::account] = badAddress;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // Missing account.
            Json::Value jvParams;
            jvParams[jss::hook] = Json::objectValue;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
    }

    void
    testLedgerEntryHookDefinition()
    {
        testcase("ledger_entry Request Hook Definition");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Lambda to create a hook.
        auto setHook = [](test::jtx::Account const& account) {
            std::string const createCodeHex =
                "0061736D0100000001130360037F7F7E017E60027F7F017F60017F017E0217"
                "0203656E7606616363657074000003656E76025F6700010302010205030100"
                "02062B077F01418088040B7F004180080B7F004180080B7F004180080B7F00"
                "418088040B7F0041000B7F0041010B07080104686F6F6B00020AB5800001B1"
                "800001017F230041106B220124002001200036020C41002200200042001000"
                "1A41012200200010011A200141106A240042000B";
            Json::Value jv =
                ripple::test::jtx::hook(account, {{hso(createCodeHex)}}, 0);
            return jv;
        };

        using namespace std::chrono_literals;
        env(setHook(alice), HSFEE);
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        auto const hook = env.le(keylet::hook(alice.id()));
        auto const& hooks = hook->getFieldArray(sfHooks);
        uint256 hookHash = hooks[0].getFieldH256(sfHookHash);

        {
            // Request the hook_definition using hash.
            Json::Value jvParams;
            jvParams[jss::hook_definition] = to_string(hookHash);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][jss::HookHash] == to_string(hookHash));
        }
        {
            // Malformed hook_definition entry.
            Json::Value jvParams;
            jvParams[jss::hook_definition] = Json::objectValue;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Request an index that is not a payment channel.
            Json::Value jvParams;
            jvParams[jss::hook_definition] = ledgerHash;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryHookState()
    {
        testcase("ledger_entry Request Hook State");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const key = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 'k',   'e',   'y',   0x00U})
                .data());

        auto const ns = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU})
                .data());

        auto const nons = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFFU})
                .data());

        // Lambda to create a hook.
        auto setHook = [](test::jtx::Account const& account) {
            std::string const createCodeHex =
                "0061736D01000000011B0460027F7F017F60047F7F7F7F017E60037F7F7E01"
                "7E60017F017E02270303656E76025F67000003656E760973746174655F7365"
                "74000103656E76066163636570740002030201030503010002062B077F0141"
                "9088040B7F004180080B7F00418A080B7F004180080B7F00419088040B7F00"
                "41000B7F0041010B07080104686F6F6B00030AE7800001E3800002017F017E"
                "230041106B220124002001200036020C41012200200010001A200141800828"
                "0000360208200141046A410022002F0088083B010020012000280084083602"
                "004100200020014106200141086A4104100110022102200141106A24002002"
                "0B0B1001004180080B096B65790076616C7565";
            std::string ns_str =
                "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECA"
                "FE";
            Json::Value jv =
                ripple::test::jtx::hook(account, {{hso(createCodeHex)}}, 0);
            jv[jss::Hooks][0U][jss::Hook][jss::HookNamespace] = ns_str;
            return jv;
        };

        using namespace std::chrono_literals;
        env(setHook(alice), HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)), fee(XRP(1)));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        {
            // Request the hook_state using hash.
            Json::Value jvParams;
            jvParams[jss::hook_state] = Json::objectValue;
            jvParams[jss::hook_state][jss::account] = alice.human();
            jvParams[jss::hook_state][jss::key] = to_string(key);
            jvParams[jss::hook_state][jss::namespace_id] = to_string(ns);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][jss::HookStateData] == "76616C756500");
            BEAST_EXPECT(jrr[jss::node][jss::HookStateKey] == to_string(key));
        }
        {
            // Malformed hook_state object.  Missing account member.
            Json::Value jvParams;
            jvParams[jss::hook_state] = Json::objectValue;
            jvParams[jss::hook_state][jss::key] = to_string(key);
            jvParams[jss::hook_state][jss::namespace_id] = to_string(ns);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed hook_state object.  Missing key member.
            Json::Value jvParams;
            jvParams[jss::hook_state] = Json::objectValue;
            jvParams[jss::hook_state][jss::account] = alice.human();
            jvParams[jss::hook_state][jss::namespace_id] = to_string(ns);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed hook_state object.  Missing namespace member.
            Json::Value jvParams;
            jvParams[jss::hook_state] = Json::objectValue;
            jvParams[jss::hook_state][jss::account] = alice.human();
            jvParams[jss::hook_state][jss::key] = to_string(key);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed hook_state account.
            Json::Value jvParams;
            jvParams[jss::hook_state] = Json::objectValue;
            jvParams[jss::hook_state][jss::account] =
                makeBadAddress(alice.human());
            jvParams[jss::hook_state][jss::key] = to_string(key);
            jvParams[jss::hook_state][jss::namespace_id] = to_string(ns);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // Request a hook_state that doesn't exist.
            Json::Value jvParams;
            jvParams[jss::hook_state] = Json::objectValue;
            jvParams[jss::hook_state][jss::account] = alice.human();
            jvParams[jss::hook_state][jss::key] = to_string(key);
            jvParams[jss::hook_state][jss::namespace_id] = to_string(nons);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryOffer()
    {
        testcase("ledger_entry Request Offer");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();

        env(offer(alice, USD(321), XRP(322)));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        std::string offerIndex;
        {
            // Request the offer using owner and sequence.
            Json::Value jvParams;
            jvParams[jss::offer] = Json::objectValue;
            jvParams[jss::offer][jss::account] = alice.human();
            jvParams[jss::offer][jss::seq] = env.seq(alice) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][jss::TakerGets] == "322000000");
            offerIndex = jrr[jss::index].asString();
        }
        {
            // Request the offer using its index.
            Json::Value jvParams;
            jvParams[jss::offer] = offerIndex;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][jss::TakerGets] == "322000000");
        }
        {
            // Malformed account entry.
            Json::Value jvParams;
            jvParams[jss::offer] = Json::objectValue;

            std::string const badAddress = makeBadAddress(alice.human());
            jvParams[jss::offer][jss::account] = badAddress;
            jvParams[jss::offer][jss::seq] = env.seq(alice) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // Malformed offer object.  Missing account member.
            Json::Value jvParams;
            jvParams[jss::offer] = Json::objectValue;
            jvParams[jss::offer][jss::seq] = env.seq(alice) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed offer object.  Missing seq member.
            Json::Value jvParams;
            jvParams[jss::offer] = Json::objectValue;
            jvParams[jss::offer][jss::account] = alice.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed offer object.  Non-integral seq member.
            Json::Value jvParams;
            jvParams[jss::offer] = Json::objectValue;
            jvParams[jss::offer][jss::account] = alice.human();
            jvParams[jss::offer][jss::seq] = std::to_string(env.seq(alice) - 1);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
    }

    void
    testLedgerEntryPayChan()
    {
        testcase("ledger_entry Request Pay Chan");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this};
        Account const alice{"alice"};

        env.fund(XRP(10000), alice);
        env.close();

        // Lambda to create a PayChan.
        auto payChanCreate = [](test::jtx::Account const& account,
                                test::jtx::Account const& to,
                                STAmount const& amount,
                                NetClock::duration const& settleDelay,
                                PublicKey const& pk) {
            Json::Value jv;
            jv[jss::TransactionType] = jss::PaymentChannelCreate;
            jv[jss::Account] = account.human();
            jv[jss::Destination] = to.human();
            jv[jss::Amount] = amount.getJson(JsonOptions::none);
            jv[sfSettleDelay.jsonName] = settleDelay.count();
            jv[sfPublicKey.jsonName] = strHex(pk.slice());
            return jv;
        };

        env(payChanCreate(alice, env.master, XRP(57), 18s, alice.pk()));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        uint256 const payChanIndex{
            keylet::payChan(alice, env.master, env.seq(alice) - 1).key};
        {
            // Request the payment channel using its index.
            Json::Value jvParams;
            jvParams[jss::payment_channel] = to_string(payChanIndex);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][sfAmount.jsonName] == "57000000");
            BEAST_EXPECT(jrr[jss::node][sfBalance.jsonName] == "0");
            BEAST_EXPECT(jrr[jss::node][sfSettleDelay.jsonName] == 18);
        }
        {
            // Request an index that is not a payment channel.
            Json::Value jvParams;
            jvParams[jss::payment_channel] = ledgerHash;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryRippleState()
    {
        testcase("ledger_entry Request RippleState");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();

        env.trust(USD(999), alice);
        env.close();

        env(pay(gw, alice, USD(97)));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        {
            // Request the trust line using the accounts and currency.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] = gw.human();
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::node][sfBalance.jsonName][jss::value] == "-97");
            BEAST_EXPECT(
                jrr[jss::node][sfHighLimit.jsonName][jss::value] == "999");
        }
        {
            // ripple_state is not an object.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = "ripple_state";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state.currency is missing.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] = gw.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state accounts is not an array.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = 2;
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state one of the accounts is missing.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state more than 2 accounts.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] = gw.human();
            jvParams[jss::ripple_state][jss::accounts][2u] = alice.human();
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state account[0] is not a string.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = 44;
            jvParams[jss::ripple_state][jss::accounts][1u] = gw.human();
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state account[1] is not a string.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] = 21;
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state account[0] == account[1].
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] = alice.human();
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // ripple_state malformed account[0].
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] =
                makeBadAddress(alice.human());
            jvParams[jss::ripple_state][jss::accounts][1u] = gw.human();
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // ripple_state malformed account[1].
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] =
                makeBadAddress(gw.human());
            jvParams[jss::ripple_state][jss::currency] = "USD";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // ripple_state malformed currency.
            Json::Value jvParams;
            jvParams[jss::ripple_state] = Json::objectValue;
            jvParams[jss::ripple_state][jss::accounts] = Json::arrayValue;
            jvParams[jss::ripple_state][jss::accounts][0u] = alice.human();
            jvParams[jss::ripple_state][jss::accounts][1u] = gw.human();
            jvParams[jss::ripple_state][jss::currency] = "USDollars";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedCurrency", "");
        }
    }

    void
    testLedgerEntryTicket()
    {
        testcase("ledger_entry Request Ticket");
        using namespace test::jtx;
        Env env{*this};
        env.close();

        // Create two tickets.
        std::uint32_t const tkt1{env.seq(env.master) + 1};
        env(ticket::create(env.master, 2));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};
        // Request four tickets: one before the first one we created, the
        // two created tickets, and the ticket that would come after the
        // last created ticket.
        {
            // Not a valid ticket requested by index.
            Json::Value jvParams;
            jvParams[jss::ticket] =
                to_string(getTicketIndex(env.master, tkt1 - 1));
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
        {
            // First real ticket requested by index.
            Json::Value jvParams;
            jvParams[jss::ticket] = to_string(getTicketIndex(env.master, tkt1));
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::node][sfLedgerEntryType.jsonName] == jss::Ticket);
            BEAST_EXPECT(jrr[jss::node][sfTicketSequence.jsonName] == tkt1);
        }
        {
            // Second real ticket requested by account and sequence.
            Json::Value jvParams;
            jvParams[jss::ticket] = Json::objectValue;
            jvParams[jss::ticket][jss::account] = env.master.human();
            jvParams[jss::ticket][jss::ticket_seq] = tkt1 + 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::node][jss::index] ==
                to_string(getTicketIndex(env.master, tkt1 + 1)));
        }
        {
            // Not a valid ticket requested by account and sequence.
            Json::Value jvParams;
            jvParams[jss::ticket] = Json::objectValue;
            jvParams[jss::ticket][jss::account] = env.master.human();
            jvParams[jss::ticket][jss::ticket_seq] = tkt1 + 2;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
        {
            // Request a ticket using an account root entry.
            Json::Value jvParams;
            jvParams[jss::ticket] = to_string(keylet::account(env.master).key);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "unexpectedLedgerType", "");
        }
        {
            // Malformed account entry.
            Json::Value jvParams;
            jvParams[jss::ticket] = Json::objectValue;

            std::string const badAddress = makeBadAddress(env.master.human());
            jvParams[jss::ticket][jss::account] = badAddress;
            jvParams[jss::ticket][jss::ticket_seq] = env.seq(env.master) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedAddress", "");
        }
        {
            // Malformed ticket object.  Missing account member.
            Json::Value jvParams;
            jvParams[jss::ticket] = Json::objectValue;
            jvParams[jss::ticket][jss::ticket_seq] = env.seq(env.master) - 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed ticket object.  Missing seq member.
            Json::Value jvParams;
            jvParams[jss::ticket] = Json::objectValue;
            jvParams[jss::ticket][jss::account] = env.master.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed ticket object.  Non-integral seq member.
            Json::Value jvParams;
            jvParams[jss::ticket] = Json::objectValue;
            jvParams[jss::ticket][jss::account] = env.master.human();
            jvParams[jss::ticket][jss::ticket_seq] =
                std::to_string(env.seq(env.master) - 1);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
    }

    void
    testLedgerEntryURIToken()
    {
        testcase("ledger_entry Request URIToken");
        using namespace test::jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Lambda to create an uritoken.
        auto mint = [](test::jtx::Account const& account,
                       std::string const& uri) {
            Json::Value jv;
            jv[jss::TransactionType] = jss::URITokenMint;
            jv[jss::Flags] = tfBurnable;
            jv[jss::Account] = account.human();
            jv[sfURI.jsonName] = strHex(uri);
            return jv;
        };

        using namespace std::chrono_literals;
        std::string const uri(maxTokenURILength, '?');
        env(mint(alice, uri));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        uint256 const uritokenIndex{
            keylet::uritoken(alice, Blob(uri.begin(), uri.end())).key};
        {
            // Request the uritoken using its index.
            Json::Value jvParams;
            jvParams[jss::uri_token] = to_string(uritokenIndex);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][sfOwner.jsonName] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfURI.jsonName] == strHex(uri));
            BEAST_EXPECT(jrr[jss::node][sfFlags.jsonName] == lsfBurnable);
        }
        {
            // Request the uritoken using its account and uri.
            Json::Value jvParams;
            jvParams[jss::uri_token] = Json::objectValue;
            jvParams[jss::uri_token][jss::account] = alice.human();
            jvParams[jss::uri_token][jss::uri] = uri;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][sfOwner.jsonName] == alice.human());
            BEAST_EXPECT(jrr[jss::node][sfURI.jsonName] == strHex(uri));
            BEAST_EXPECT(jrr[jss::node][sfFlags.jsonName] == lsfBurnable);
        }
        {
            // Malformed uritoken object.  Missing account member.
            Json::Value jvParams;
            jvParams[jss::uri_token] = Json::objectValue;
            jvParams[jss::uri_token][jss::uri] = uri;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed uritoken object.  Missing seq member.
            Json::Value jvParams;
            jvParams[jss::uri_token] = Json::objectValue;
            jvParams[jss::uri_token][jss::account] = env.master.human();
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Request an index that is not a uritoken.
            Json::Value jvParams;
            jvParams[jss::uri_token] = ledgerHash;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryImportVLSeq()
    {
        testcase("ledger_entry Request ImportVLSeq");
        using namespace test::jtx;

        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC"
            "1"};
        Env env{*this, network::makeNetworkVLConfig(21337, keys)};

        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const master = Account("masterpassphrase");
        env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
        env.close();
        env(import::import(
                alice, import::loadXpop(test::ImportTCAccountSet::w_seed)),
            fee(10 * 10),
            ter(tesSUCCESS));
        env.close();

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        auto const pk = PublicKey(makeSlice(*strUnHex(keys[0u])));
        uint256 const importvlIndex{keylet::import_vlseq(pk).key};
        {
            // Request the import vl using its index.
            Json::Value jvParams;
            jvParams[jss::import_vlseq] = to_string(importvlIndex);
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][sfPublicKey.jsonName] == keys[0u]);
        }
        {
            // Request the import vl using its public key.
            Json::Value jvParams;
            jvParams[jss::import_vlseq] = Json::objectValue;
            jvParams[jss::import_vlseq][jss::public_key] = keys[0u];
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::node][sfPublicKey.jsonName] == keys[0u]);
        }
        {
            // Malformed import vl object.  Missing public key.
            Json::Value jvParams;
            jvParams[jss::import_vlseq] = Json::objectValue;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed import vl object.  Bad public key.
            Json::Value jvParams;
            jvParams[jss::import_vlseq] = Json::objectValue;
            jvParams[jss::import_vlseq][jss::public_key] = 1;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Malformed import vl object.  Bad public key.
            Json::Value jvParams;
            jvParams[jss::import_vlseq] = Json::objectValue;
            jvParams[jss::import_vlseq][jss::public_key] = "DEADBEEF";
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "malformedRequest", "");
        }
        {
            // Request an index that is not a uritoken.
            Json::Value jvParams;
            jvParams[jss::import_vlseq] = ledgerHash;
            jvParams[jss::ledger_hash] = ledgerHash;
            Json::Value const jrr = env.rpc(
                "json", "ledger_entry", to_string(jvParams))[jss::result];
            checkErrorValue(jrr, "entryNotFound", "");
        }
    }

    void
    testLedgerEntryInvalidParams(unsigned int apiVersion)
    {
        testcase(
            "ledger_entry Request With Invalid Parameters v" +
            std::to_string(apiVersion));
        using namespace test::jtx;
        Env env{*this};

        std::string const ledgerHash{to_string(env.closed()->info().hash)};

        // "features" is not an option supported by ledger_entry.
        Json::Value jvParams;
        jvParams[jss::api_version] = apiVersion;
        jvParams[jss::features] = ledgerHash;
        jvParams[jss::ledger_hash] = ledgerHash;
        Json::Value const jrr =
            env.rpc("json", "ledger_entry", to_string(jvParams))[jss::result];

        if (apiVersion < 2u)
            checkErrorValue(jrr, "unknownOption", "");
        else
            checkErrorValue(jrr, "invalidParams", "");
    }

    /// @brief ledger RPC requests as a way to drive
    /// input options to lookupLedger. The point of this test is
    /// coverage for lookupLedger, not so much the ledger
    /// RPC request.
    void
    testLookupLedger()
    {
        testcase("Lookup ledger");
        using namespace test::jtx;
        Env env{*this, FeatureBitset{}};  // hashes requested below assume
                                          // no amendments

        env.fund(XRP(10000), "alice");
        env.close();
        env.fund(XRP(10000), "bob");
        env.close();
        env.fund(XRP(10000), "jim");
        env.close();
        env.fund(XRP(10000), "jill");

        {
            // access via the legacy ledger field, keyword index values
            Json::Value jvParams;
            jvParams[jss::ledger] = "closed";
            auto jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "5");

            jvParams[jss::ledger] = "validated";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "5");

            jvParams[jss::ledger] = "current";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "6");

            // ask for a bad ledger keyword
            jvParams[jss::ledger] = "invalid";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerIndexMalformed");

            // numeric index
            jvParams[jss::ledger] = 4;
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "4");

            // numeric index - out of range
            jvParams[jss::ledger] = 20;
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "lgrNotFound");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerNotFound");
        }

        {
            // access via the ledger_hash field
            Json::Value jvParams;
            jvParams[jss::ledger_hash] =
                "D39C52DE7CBF561ECA875A6D636B7C9095408DE1FAF4EC4AAF3FDD8AB3A1EA"
                "55";
            auto jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "3");

            // extra leading hex chars in hash will be ignored
            jvParams[jss::ledger_hash] =
                "DEADBEEF"
                "2E81FC6EC0DD943197E0C7E3FBE9AE30"
                "7F2775F2F7485BB37307984C3C0F2340";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerHashMalformed");

            // request with non-string ledger_hash
            jvParams[jss::ledger_hash] = 2;
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerHashNotString");

            // malformed (non hex) hash
            jvParams[jss::ledger_hash] =
                "2E81FC6EC0DD943197EGC7E3FBE9AE30"
                "7F2775F2F7485BB37307984C3C0F2340";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerHashMalformed");

            // properly formed, but just doesn't exist
            jvParams[jss::ledger_hash] =
                "8C3EEDB3124D92E49E75D81A8826A2E6"
                "5A75FD71FC3FD6F36FEB803C5F1D812D";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "lgrNotFound");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerNotFound");
        }

        {
            // access via the ledger_index field, keyword index values
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "closed";
            auto jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "5");
            BEAST_EXPECT(jrr.isMember(jss::ledger_index));

            jvParams[jss::ledger_index] = "validated";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "5");

            jvParams[jss::ledger_index] = "current";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(jrr[jss::ledger][jss::ledger_index] == "6");
            BEAST_EXPECT(jrr.isMember(jss::ledger_current_index));

            // ask for a bad ledger keyword
            jvParams[jss::ledger_index] = "invalid";
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerIndexMalformed");

            // numeric index
            for (auto i : {1, 2, 3, 4, 5, 6})
            {
                jvParams[jss::ledger_index] = i;
                jrr = env.rpc(
                    "json",
                    "ledger",
                    boost::lexical_cast<std::string>(jvParams))[jss::result];
                BEAST_EXPECT(jrr.isMember(jss::ledger));
                if (i < 6)
                    BEAST_EXPECT(jrr.isMember(jss::ledger_hash));
                BEAST_EXPECT(
                    jrr[jss::ledger][jss::ledger_index] == std::to_string(i));
            }

            // numeric index - out of range
            jvParams[jss::ledger_index] = 7;
            jrr = env.rpc(
                "json",
                "ledger",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "lgrNotFound");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerNotFound");
        }
    }

    void
    testNoQueue()
    {
        testcase("Ledger with queueing disabled");
        using namespace test::jtx;
        Env env{*this};

        Json::Value jv;
        jv[jss::ledger_index] = "current";
        jv[jss::queue] = true;
        jv[jss::expand] = true;

        auto jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        BEAST_EXPECT(!jrr.isMember(jss::queue_data));
    }

    void
    testQueue()
    {
        testcase("Ledger with Queued Transactions");
        using namespace test::jtx;
        Env env{
            *this,
            envconfig([](std::unique_ptr<Config> cfg) {
                auto& section = cfg->section("transaction_queue");
                section.set("minimum_txn_in_ledger_standalone", "3");
                section.set("normal_consensus_increase_percent", "0");
                return cfg;
            }),
            supported_amendments() - featureXahauGenesis};

        Json::Value jv;
        jv[jss::ledger_index] = "current";
        jv[jss::queue] = true;
        jv[jss::expand] = true;

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const charlie{"charlie"};
        Account const daria{"daria"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.close();
        env.fund(XRP(10000), charlie);
        env.fund(XRP(10000), daria);
        env.close();

        auto jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        BEAST_EXPECT(!jrr.isMember(jss::queue_data));

        // Fill the open ledger
        for (;;)
        {
            auto metrics = env.app().getTxQ().getMetrics(*env.current());
            if (metrics.openLedgerFeeLevel > metrics.minProcessingFeeLevel)
                break;
            env(noop(alice));
        }

        BEAST_EXPECT(env.current()->info().seq == 5);
        // Put some txs in the queue
        // Alice
        auto aliceSeq = env.seq(alice);
        env(pay(alice, "george", XRP(1000)),
            json(R"({"LastLedgerSequence":7})"),
            ter(terQUEUED));
        env(offer(alice, XRP(50000), alice["USD"](5000)),
            seq(aliceSeq + 1),
            ter(terQUEUED));
        env(noop(alice), seq(aliceSeq + 2), ter(terQUEUED));
        // Bob
        auto batch = [&env](Account a) {
            auto aSeq = env.seq(a);
            // Enough fee to get in front of alice in the queue
            for (int i = 0; i < 10; ++i)
            {
                env(noop(a), fee(1000 + i), seq(aSeq + i), ter(terQUEUED));
            }
        };
        batch(bob);
        // Charlie
        batch(charlie);
        // Daria
        batch(daria);

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        BEAST_EXPECT(jrr[jss::queue_data].size() == 33);

        // Close enough ledgers so that alice's first tx expires.
        env.close();
        env.close();
        env.close();
        BEAST_EXPECT(env.current()->info().seq == 8);

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        BEAST_EXPECT(jrr[jss::queue_data].size() == 11);

        env.close();

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        const std::string txid0 = [&]() {
            auto const& parentHash = env.current()->info().parentHash;
            if (BEAST_EXPECT(jrr[jss::queue_data].size() == 2))
            {
                const std::string txid1 = [&]() {
                    auto const& txj = jrr[jss::queue_data][0u];
                    BEAST_EXPECT(txj[jss::account] == alice.human());
                    BEAST_EXPECT(txj[jss::fee_level] == "256");
                    BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
                    BEAST_EXPECT(txj["retries_remaining"] == 10);
                    BEAST_EXPECT(txj.isMember(jss::tx));
                    auto const& tx = txj[jss::tx];
                    BEAST_EXPECT(tx[jss::Account] == alice.human());
                    BEAST_EXPECT(tx[jss::TransactionType] == jss::AccountSet);
                    return tx[jss::hash].asString();
                }();

                auto const& txj = jrr[jss::queue_data][1u];
                BEAST_EXPECT(txj[jss::account] == alice.human());
                BEAST_EXPECT(txj[jss::fee_level] == "256");
                BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
                BEAST_EXPECT(txj["retries_remaining"] == 10);
                BEAST_EXPECT(txj.isMember(jss::tx));
                auto const& tx = txj[jss::tx];
                BEAST_EXPECT(tx[jss::Account] == alice.human());
                BEAST_EXPECT(tx[jss::TransactionType] == jss::OfferCreate);
                const auto txid0 = tx[jss::hash].asString();
                uint256 tx0, tx1;
                BEAST_EXPECT(tx0.parseHex(txid0));
                BEAST_EXPECT(tx1.parseHex(txid1));
                BEAST_EXPECT((tx1 ^ parentHash) < (tx0 ^ parentHash));
                return txid0;
            }
            return std::string{};
        }();

        env.close();

        jv[jss::expand] = false;

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        if (BEAST_EXPECT(jrr[jss::queue_data].size() == 2))
        {
            auto const& parentHash = env.current()->info().parentHash;
            auto const txid1 = [&]() {
                auto const& txj = jrr[jss::queue_data][0u];
                BEAST_EXPECT(txj[jss::account] == alice.human());
                BEAST_EXPECT(txj[jss::fee_level] == "256");
                BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
                BEAST_EXPECT(txj.isMember(jss::tx));
                return txj[jss::tx].asString();
            }();
            auto const& txj = jrr[jss::queue_data][1u];
            BEAST_EXPECT(txj[jss::account] == alice.human());
            BEAST_EXPECT(txj[jss::fee_level] == "256");
            BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
            BEAST_EXPECT(txj["retries_remaining"] == 9);
            BEAST_EXPECT(txj["last_result"] == "terPRE_SEQ");
            BEAST_EXPECT(txj.isMember(jss::tx));
            BEAST_EXPECT(txj[jss::tx] == txid0);
            uint256 tx0, tx1;
            BEAST_EXPECT(tx0.parseHex(txid0));
            BEAST_EXPECT(tx1.parseHex(txid1));
            BEAST_EXPECT((tx1 ^ parentHash) < (tx0 ^ parentHash));
        }

        env.close();

        jv[jss::expand] = true;
        jv[jss::binary] = true;

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        if (BEAST_EXPECT(jrr[jss::queue_data].size() == 2))
        {
            auto const& txj = jrr[jss::queue_data][1u];
            BEAST_EXPECT(txj[jss::account] == alice.human());
            BEAST_EXPECT(txj[jss::fee_level] == "256");
            BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
            BEAST_EXPECT(txj["retries_remaining"] == 8);
            BEAST_EXPECT(txj["last_result"] == "terPRE_SEQ");
            BEAST_EXPECT(txj.isMember(jss::tx));
            BEAST_EXPECT(txj[jss::tx].isMember(jss::tx_blob));

            auto const& txj2 = jrr[jss::queue_data][0u];
            BEAST_EXPECT(txj2[jss::account] == alice.human());
            BEAST_EXPECT(txj2[jss::fee_level] == "256");
            BEAST_EXPECT(txj2["preflight_result"] == "tesSUCCESS");
            BEAST_EXPECT(txj2["retries_remaining"] == 10);
            BEAST_EXPECT(!txj2.isMember("last_result"));
            BEAST_EXPECT(txj2.isMember(jss::tx));
            BEAST_EXPECT(txj2[jss::tx].isMember(jss::tx_blob));
        }

        for (int i = 0; i != 9; ++i)
        {
            env.close();
        }

        jv[jss::expand] = false;
        jv[jss::binary] = false;

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        const std::string txid2 = [&]() {
            if (BEAST_EXPECT(jrr[jss::queue_data].size() == 1))
            {
                auto const& txj = jrr[jss::queue_data][0u];
                BEAST_EXPECT(txj[jss::account] == alice.human());
                BEAST_EXPECT(txj[jss::fee_level] == "256");
                BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
                BEAST_EXPECT(txj["retries_remaining"] == 1);
                BEAST_EXPECT(txj["last_result"] == "terPRE_SEQ");
                BEAST_EXPECT(txj.isMember(jss::tx));
                BEAST_EXPECT(txj[jss::tx] != txid0);
                return txj[jss::tx].asString();
            }
            return std::string{};
        }();

        jv[jss::full] = true;

        jrr = env.rpc("json", "ledger", to_string(jv))[jss::result];
        if (BEAST_EXPECT(jrr[jss::queue_data].size() == 1))
        {
            auto const& txj = jrr[jss::queue_data][0u];
            BEAST_EXPECT(txj[jss::account] == alice.human());
            BEAST_EXPECT(txj[jss::fee_level] == "256");
            BEAST_EXPECT(txj["preflight_result"] == "tesSUCCESS");
            BEAST_EXPECT(txj["retries_remaining"] == 1);
            BEAST_EXPECT(txj["last_result"] == "terPRE_SEQ");
            BEAST_EXPECT(txj.isMember(jss::tx));
            auto const& tx = txj[jss::tx];
            BEAST_EXPECT(tx[jss::Account] == alice.human());
            BEAST_EXPECT(tx[jss::TransactionType] == jss::AccountSet);
            BEAST_EXPECT(tx[jss::hash] == txid2);
        }
    }

    void
    testLedgerAccountsOption()
    {
        testcase("Ledger Request, Accounts Hashes");
        using namespace test::jtx;

        Env env{*this};

        env.close();

        std::string index;
        {
            Json::Value jvParams;
            jvParams[jss::ledger_index] = 3u;
            jvParams[jss::accounts] = true;
            jvParams[jss::expand] = true;
            jvParams[jss::type] = "hashes";
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::ledger].isMember(jss::accountState));
            BEAST_EXPECT(jrr[jss::ledger][jss::accountState].isArray());
            BEAST_EXPECT(jrr[jss::ledger][jss::accountState].size() == 1u);
            BEAST_EXPECT(
                jrr[jss::ledger][jss::accountState][0u]["LedgerEntryType"] ==
                jss::LedgerHashes);
            index = jrr[jss::ledger][jss::accountState][0u]["index"].asString();
        }
        {
            Json::Value jvParams;
            jvParams[jss::ledger_index] = 3u;
            jvParams[jss::accounts] = true;
            jvParams[jss::expand] = false;
            jvParams[jss::type] = "hashes";
            auto const jrr =
                env.rpc("json", "ledger", to_string(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::ledger].isMember(jss::accountState));
            BEAST_EXPECT(jrr[jss::ledger][jss::accountState].isArray());
            BEAST_EXPECT(jrr[jss::ledger][jss::accountState].size() == 1u);
            BEAST_EXPECT(jrr[jss::ledger][jss::accountState][0u] == index);
        }
    }

public:
    void
    run() override
    {
        testLedgerRequest();
        testBadInput();
        testLedgerCurrent();
        testMissingLedgerEntryLedgerHash();
        testLedgerFull();
        testLedgerFullNonAdmin();
        testLedgerAccounts();
        testLedgerEntryAccountRoot();
        testLedgerEntryCheck();
        testLedgerEntryDepositPreauth();
        testLedgerEntryDirectory();
        testLedgerEntryEmittedTxn();
        testLedgerEntryEscrow();
        testLedgerEntryHook();
        testLedgerEntryHookDefinition();
        testLedgerEntryHookState();
        testLedgerEntryOffer();
        testLedgerEntryPayChan();
        testLedgerEntryRippleState();
        testLedgerEntryTicket();
        testLedgerEntryURIToken();
        testLedgerEntryImportVLSeq();
        testLookupLedger();
        testNoQueue();
        testQueue();
        testLedgerAccountsOption();

        test::jtx::forAllApiVersions(std::bind_front(
            &LedgerRPC_test::testLedgerEntryInvalidParams, this));
    }
};

BEAST_DEFINE_TESTSUITE(LedgerRPC, app, ripple);

}  // namespace test
}  // namespace ripple
