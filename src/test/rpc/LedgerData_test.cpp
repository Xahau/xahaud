//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2016 Ripple Labs Inc.

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

#include <ripple/app/misc/HashRouter.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <test/jtx/TestHelpers.h>

namespace ripple {

class LedgerData_test : public beast::unit_test::suite
{
public:
    // test helper
    static bool
    checkMarker(Json::Value const& val)
    {
        return val.isMember(jss::marker) && val[jss::marker].isString() &&
            val[jss::marker].asString().size() > 0;
    }

    void
    testCurrentLedgerToLimits(bool asAdmin)
    {
        using namespace test::jtx;
        Env env{*this, asAdmin ? envconfig() : envconfig(no_admin)};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(100000), gw);

        int const max_limit = 256;  // would be 2048 for binary requests, no
                                    // need to test that here

        for (auto i = 0; i < max_limit + 10; i++)
        {
            Account const bob{std::string("bob") + std::to_string(i)};
            env.fund(XRP(1000), bob);
        }
        // Note that calls to env.close() fail without admin permission.
        if (asAdmin)
            env.close();

        // with no limit specified, we get the max_limit if the total number of
        // accounts is greater than max, which it is here
        Json::Value jvParams;
        jvParams[jss::ledger_index] = "current";
        jvParams[jss::binary] = false;
        {
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(
                jrr[jss::ledger_current_index].isIntegral() &&
                jrr[jss::ledger_current_index].asInt() > 0);
            BEAST_EXPECT(checkMarker(jrr));
            BEAST_EXPECT(checkArraySize(jrr[jss::state], max_limit));
        }

        // check limits values around the max_limit (+/- 1)
        for (auto delta = -1; delta <= 1; delta++)
        {
            jvParams[jss::limit] = max_limit + delta;
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(checkArraySize(
                jrr[jss::state],
                (delta > 0 && !asAdmin) ? max_limit : max_limit + delta));
        }
    }

    void
    testCurrentLedgerBinary()
    {
        using namespace test::jtx;
        Env env{*this, envconfig(no_admin)};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(100000), gw);

        int const num_accounts = 10;

        for (auto i = 0; i < num_accounts; i++)
        {
            Account const bob{std::string("bob") + std::to_string(i)};
            env.fund(XRP(1000), bob);
        }

        // with no limit specified, we should get all of our fund entries
        // plus three more related to the gateway setup
        Json::Value jvParams;
        jvParams[jss::ledger_index] = "current";
        jvParams[jss::binary] = true;
        auto const jrr = env.rpc(
            "json",
            "ledger_data",
            boost::lexical_cast<std::string>(jvParams))[jss::result];
        BEAST_EXPECT(
            jrr[jss::ledger_current_index].isIntegral() &&
            jrr[jss::ledger_current_index].asInt() > 0);
        BEAST_EXPECT(!jrr.isMember(jss::marker));
        BEAST_EXPECT(checkArraySize(jrr[jss::state], num_accounts + 4));
    }

    void
    testBadInput()
    {
        using namespace test::jtx;
        Env env{*this};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        Account const bob{"bob"};

        env.fund(XRP(10000), gw, bob);
        env.trust(USD(1000), bob);

        {
            // bad limit
            Json::Value jvParams;
            jvParams[jss::limit] = "0";  // NOT an integer
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::status] == "error");
            BEAST_EXPECT(
                jrr[jss::error_message] ==
                "Invalid field 'limit', not integer.");
        }

        {
            // invalid marker
            Json::Value jvParams;
            jvParams[jss::marker] = "NOT_A_MARKER";
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::status] == "error");
            BEAST_EXPECT(
                jrr[jss::error_message] ==
                "Invalid field 'marker', not valid.");
        }

        {
            // invalid marker - not a string
            Json::Value jvParams;
            jvParams[jss::marker] = 1;
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "invalidParams");
            BEAST_EXPECT(jrr[jss::status] == "error");
            BEAST_EXPECT(
                jrr[jss::error_message] ==
                "Invalid field 'marker', not valid.");
        }

        {
            // ask for a bad ledger index
            Json::Value jvParams;
            jvParams[jss::ledger_index] = 10u;
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr[jss::error] == "lgrNotFound");
            BEAST_EXPECT(jrr[jss::status] == "error");
            BEAST_EXPECT(jrr[jss::error_message] == "ledgerNotFound");
        }
    }

    void
    testMarkerFollow()
    {
        using namespace test::jtx;
        Env env{*this, envconfig(no_admin)};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(100000), gw);

        int const num_accounts = 20;

        for (auto i = 0; i < num_accounts; i++)
        {
            Account const bob{std::string("bob") + std::to_string(i)};
            env.fund(XRP(1000), bob);
        }

        // with no limit specified, we should get all of our fund entries
        // plus three more related to the gateway setup
        Json::Value jvParams;
        jvParams[jss::ledger_index] = "current";
        jvParams[jss::binary] = false;
        auto jrr = env.rpc(
            "json",
            "ledger_data",
            boost::lexical_cast<std::string>(jvParams))[jss::result];
        auto const total_count = jrr[jss::state].size();

        // now make request with a limit and loop until we get all
        jvParams[jss::limit] = 5;
        jrr = env.rpc(
            "json",
            "ledger_data",
            boost::lexical_cast<std::string>(jvParams))[jss::result];
        BEAST_EXPECT(checkMarker(jrr));
        auto running_total = jrr[jss::state].size();
        while (jrr.isMember(jss::marker))
        {
            jvParams[jss::marker] = jrr[jss::marker];
            jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            running_total += jrr[jss::state].size();
        }
        BEAST_EXPECT(running_total == total_count);
    }

    void
    testLedgerHeader()
    {
        using namespace test::jtx;
        Env env{*this};
        env.fund(XRP(100000), "alice");
        env.close();

        // Ledger header should be present in the first query
        {
            // Closed ledger with non binary form
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "closed";
            auto jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            if (BEAST_EXPECT(jrr.isMember(jss::ledger)))
                BEAST_EXPECT(
                    jrr[jss::ledger][jss::ledger_hash] ==
                    to_string(env.closed()->info().hash));
        }
        {
            // Closed ledger with binary form
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "closed";
            jvParams[jss::binary] = true;
            auto jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            if (BEAST_EXPECT(jrr.isMember(jss::ledger)))
            {
                auto data =
                    strUnHex(jrr[jss::ledger][jss::ledger_data].asString());
                if (BEAST_EXPECT(data))
                {
                    Serializer s(data->data(), data->size());
                    std::uint32_t seq = 0;
                    BEAST_EXPECT(s.getInteger<std::uint32_t>(seq, 0));
                    BEAST_EXPECT(seq == 3);
                }
            }
        }
        {
            // Current ledger with binary form
            Json::Value jvParams;
            jvParams[jss::binary] = true;
            auto jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember(jss::ledger));
            BEAST_EXPECT(!jrr[jss::ledger].isMember(jss::ledger_data));
        }
    }

    void
    testLedgerType()
    {
        // Put a bunch of different LedgerEntryTypes into a ledger
        using namespace test::jtx;
        using namespace std::chrono;

        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC"
            "1"};
        Env env{*this, network::makeNetworkVLConfig(21337, keys)};

        Account const alice{"alice"};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(100000), gw, alice);

        auto makeRequest = [&env](Json::StaticString const& type) {
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "current";
            jvParams[jss::type] = type;
            return env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
        };

        // Assert that state is an empty array.
        for (auto const& type :
             {jss::amendments,
              jss::check,
              jss::directory,
              jss::offer,
              jss::signer_list,
              jss::state,
              jss::ticket,
              jss::escrow,
              jss::payment_channel,
              jss::hook,
              jss::hook_definition,
              jss::hook_state,
              jss::uri_token,
              jss::deposit_preauth})
        {
            auto const jrr = makeRequest(type);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 0));
        }

        int const num_accounts = 10;

        for (auto i = 0; i < num_accounts; i++)
        {
            Account const bob{std::string("bob") + std::to_string(i)};
            env.fund(XRP(1000), bob);
        }
        env(offer(Account{"bob0"}, USD(100), XRP(100)));
        env.trust(Account{"bob2"}["USD"](100), Account{"bob3"});

        auto majorities = getMajorityAmendments(*env.closed());
        for (int i = 0; i <= 256; ++i)
        {
            env.close();
            majorities = getMajorityAmendments(*env.closed());
            if (!majorities.empty())
                break;
        }
        env(signers(
            Account{"bob0"}, 1, {{Account{"bob1"}, 1}, {Account{"bob2"}, 1}}));
        env(ticket::create(env.master, 1));

        {
            Json::Value jv;
            jv[jss::TransactionType] = jss::EscrowCreate;
            jv[jss::Flags] = tfUniversal;
            jv[jss::Account] = Account{"bob5"}.human();
            jv[jss::Destination] = Account{"bob6"}.human();
            jv[jss::Amount] = XRP(50).value().getJson(JsonOptions::none);
            jv[sfFinishAfter.fieldName] = NetClock::time_point{env.now() + 10s}
                                              .time_since_epoch()
                                              .count();
            env(jv);
        }

        {
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
            Json::Value jv = ripple::test::jtx::hook(
                Account{"bob3"}, {{hso(createCodeHex)}}, 0);
            jv[jss::Hooks][0U][jss::Hook][jss::HookNamespace] = ns_str;
            env(jv, fee(100'000'000));
            env.close();
            env(pay(Account{"bob2"}, Account{"bob3"}, XRP(1)), fee(XRP(1)));
            env.close();
        }

        {
            std::string const uri(maxTokenURILength, '?');
            env(uritoken::mint(Account{"bob2"}, uri));
            env.close();
        }

        {
            Json::Value jv;
            jv[jss::TransactionType] = jss::PaymentChannelCreate;
            jv[jss::Flags] = tfUniversal;
            jv[jss::Account] = Account{"bob6"}.human();
            jv[jss::Destination] = Account{"bob7"}.human();
            jv[jss::Amount] = XRP(100).value().getJson(JsonOptions::none);
            jv[jss::SettleDelay] = NetClock::duration{10s}.count();
            jv[sfPublicKey.fieldName] = strHex(Account{"bob6"}.pk().slice());
            jv[sfCancelAfter.fieldName] = NetClock::time_point{env.now() + 300s}
                                              .time_since_epoch()
                                              .count();
            env(jv);
        }

        {
            auto const master = Account("masterpassphrase");
            env(noop(master), fee(10'000'000'000), ter(tesSUCCESS));
            env.close();
            env(import::import(
                    alice, import::loadXpop(test::ImportTCAccountSet::w_seed)),
                fee(10 * 10),
                ter(tesSUCCESS));
            env.close();
        }

        {
            // ADD UNL REPORT
            std::vector<std::string> const _ivlKeys = {
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC1",
                "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1"
                "CDC2",
            };

            std::vector<PublicKey> ivlKeys;
            for (auto const& strPk : _ivlKeys)
            {
                auto pkHex = strUnHex(strPk);
                ivlKeys.emplace_back(makeSlice(*pkHex));
            }

            std::vector<std::string> const _vlKeys = {
                "ED8E43A943A174190BA2FAE91F44AC6E2D1D8202EFDCC2EA3DBB39814576D6"
                "90F7",
                "ED45D1840EE724BE327ABE9146503D5848EFD5F38B6D5FEDE71E80ACCE5E6E"
                "738B"};

            std::vector<PublicKey> vlKeys;
            for (auto const& strPk : _vlKeys)
            {
                auto pkHex = strUnHex(strPk);
                vlKeys.emplace_back(makeSlice(*pkHex));
            }

            // insert a ttUNL_REPORT pseudo into the open ledger
            env.app().openLedger().modify(
                [&](OpenView& view, beast::Journal j) -> bool {
                    STTx tx = test::unl::createUNLReportTx(
                        env.current()->seq() + 1, ivlKeys[0], vlKeys[0]);
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

        env(check::create("bob6", "bob7", XRP(100)));

        // bob9 DepositPreauths bob4 and bob8.
        env(deposit::auth(Account{"bob9"}, Account{"bob4"}));
        env(deposit::auth(Account{"bob9"}, Account{"bob8"}));
        env.close();

        // Now fetch each type

        {  // jvParams[jss::type] = "account";
            auto const jrr = makeRequest(jss::account);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 13));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::AccountRoot);
        }

        {  // jvParams[jss::type] = "amendments";
            auto const jrr = makeRequest(jss::amendments);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 0));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::Amendments);
        }

        {  // jvParams[jss::type] = "unl_report";
            auto const jrr = makeRequest(jss::unl_report);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));

            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::UNLReport);
        }

        {  // jvParams[jss::type] = "check";
            auto const jrr = makeRequest(jss::check);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::Check);
        }

        {  // jvParams[jss::type] = "directory";
            auto const jrr = makeRequest(jss::directory);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 10));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::DirectoryNode);
        }

        {  // jvParams[jss::type] = "fee";
            auto const jrr = makeRequest(jss::fee);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::FeeSettings);
        }

        {  // jvParams[jss::type] = "hashes";
            auto const jrr = makeRequest(jss::hashes);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 2));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::LedgerHashes);
        }

        {  // jvParams[jss::type] = "import_vlseq";
            auto const jrr = makeRequest(jss::import_vlseq);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::ImportVLSequence);
        }

        {  // jvParams[jss::type] = "offer";
            auto const jrr = makeRequest(jss::offer);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::Offer);
        }

        {  // jvParams[jss::type] = "signer_list";
            auto const jrr = makeRequest(jss::signer_list);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::SignerList);
        }

        {  // jvParams[jss::type] = "state";
            auto const jrr = makeRequest(jss::state);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::RippleState);
        }

        {  // jvParams[jss::type] = "ticket";
            auto const jrr = makeRequest(jss::ticket);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::Ticket);
        }

        {  // jvParams[jss::type] = "escrow";
            auto const jrr = makeRequest(jss::escrow);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::Escrow);
        }

        {  // jvParams[jss::type] = "hook";
            auto const jrr = makeRequest(jss::hook);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::Hook);
        }

        {  // jvParams[jss::type] = "hook_definition";
            auto const jrr = makeRequest(jss::hook_definition);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::HookDefinition);
        }

        {  // jvParams[jss::type] = "hook_state";
            auto const jrr = makeRequest(jss::hook_state);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::HookState);
        }

        {  // jvParams[jss::type] = "uri_token";
            auto const jrr = makeRequest(jss::uri_token);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::URIToken);
        }

        {  // jvParams[jss::type] = "payment_channel";
            auto const jrr = makeRequest(jss::payment_channel);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 1));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::PayChannel);
        }

        {  // jvParams[jss::type] = "deposit_preauth";
            auto const jrr = makeRequest(jss::deposit_preauth);
            BEAST_EXPECT(checkArraySize(jrr[jss::state], 2));
            for (auto const& j : jrr[jss::state])
                BEAST_EXPECT(j["LedgerEntryType"] == jss::DepositPreauth);
        }

        {  // jvParams[jss::type] = "misspelling";
            Json::Value jvParams;
            jvParams[jss::ledger_index] = "current";
            jvParams[jss::type] = "misspelling";
            auto const jrr = env.rpc(
                "json",
                "ledger_data",
                boost::lexical_cast<std::string>(jvParams))[jss::result];
            BEAST_EXPECT(jrr.isMember("error"));
            BEAST_EXPECT(jrr["error"] == "invalidParams");
            BEAST_EXPECT(jrr["error_message"] == "Invalid field 'type'.");
        }
    }

    void
    run() override
    {
        testCurrentLedgerToLimits(true);
        testCurrentLedgerToLimits(false);
        testCurrentLedgerBinary();
        testBadInput();
        testMarkerFollow();
        testLedgerHeader();
        testLedgerType();
    }
};

BEAST_DEFINE_TESTSUITE_PRIO(LedgerData, app, ripple, 1);

}  // namespace ripple
