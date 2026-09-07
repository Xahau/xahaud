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
#include <test/jtx/WSClient.h>
#include <test/jtx/envconfig.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/AmendmentTable.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpl/basics/FileUtilities.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/utility/temp_dir.h>
#include <xrpl/protocol/BuildInfo.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

namespace ripple {

class AmendmentBlocked_test : public beast::unit_test::suite
{
    // An amendment id this binary will never support.
    static uint256
    unsupportedAmendmentId()
    {
        std::string const in = "AmendmentBlocked_test.unsupported";
        sha256_hasher h;
        using beast::hash_append;
        hash_append(h, in);
        auto const d = static_cast<sha256_hasher::result_type>(h);
        uint256 result;
        std::memcpy(result.data(), d.data(), d.size());
        return result;
    }

    // The majority period only sets how far out activation is expected, so
    // shorten it from two weeks. That keeps the ledger close-time jumps in
    // these tests down to minutes, and makes the relationship to the
    // five-minute shutdown lead time in LedgerMaster obvious.
    static std::unique_ptr<Config>
    shortMajorityConfig()
    {
        using namespace std::chrono_literals;
        auto cfg = test::jtx::envconfig();
        cfg->AMENDMENT_MAJORITY_TIME = 15min;
        return cfg;
    }

    // Give the amendment table a majority, as of now, for an amendment we do
    // not support; activation is then expected one majority period out.
    // Bypasses the ReadView overload (and therefore needValidatedLedger) on
    // purpose: because lastUpdateSeq_ is set to the current sequence, later
    // closes within the same 256-ledger block will not recompute -- and so
    // will not clear -- what we inject here.
    void
    injectUnsupportedMajority(test::jtx::Env& env)
    {
        auto const seq = env.closed()->info().seq;
        majorityAmendments_t majority;
        majority[unsupportedAmendmentId()] = env.now();
        env.app().getAmendmentTable().doValidatedLedger(seq, {}, majority);

        auto const first =
            env.app().getAmendmentTable().firstUnsupportedExpected();
        BEAST_EXPECT(
            first &&
            *first == env.now() + env.app().config().AMENDMENT_MAJORITY_TIME);
    }

    void
    testReceiptFileHelpers()
    {
        testcase("amendment blocked receipt helpers");

        auto const id = unsupportedAmendmentId();

        {
            beast::temp_dir td;
            Config cfg;
            cfg.CONFIG_DIR = td.path();

            auto const path = amendmentBlockedFilePath(cfg);
            BEAST_EXPECT(path.filename() == "README_AMENDMENT_BLOCKED");
            BEAST_EXPECT(
                path.parent_path() == boost::filesystem::path{td.path()});
            BEAST_EXPECT(!boost::filesystem::exists(path));

            // Nothing to remove yet, and that is not an error.
            boost::system::error_code ec;
            BEAST_EXPECT(!removeAmendmentBlockedFile(cfg, ec));
            BEAST_EXPECT(!ec);

            BEAST_EXPECT(!writeAmendmentBlockedFile(
                cfg, {to_string(id) + "  (already active)"}));
            BEAST_EXPECT(boost::filesystem::exists(path));

            auto const contents = getFileContents(ec, path);
            BEAST_EXPECT(!ec);
            BEAST_EXPECT(
                contents.find("XAHAUD STOPPED: UPGRADE REQUIRED") == 0);
            // When it stopped, what was running, and what it choked on.
            BEAST_EXPECT(contents.find("Stopped at:") != std::string::npos);
            BEAST_EXPECT(
                contents.find(BuildInfo::getVersionString()) !=
                std::string::npos);
            BEAST_EXPECT(contents.find(to_string(id)) != std::string::npos);
            BEAST_EXPECT(contents.find("already active") != std::string::npos);
            BEAST_EXPECT(contents.find("Upgrade xahaud") != std::string::npos);
            // The receipt is self-clearing, so it must not tell the operator
            // to delete anything.
            BEAST_EXPECT(
                contents.find("removed automatically") != std::string::npos);

            // The timestamp is rendered, not a placeholder: to_string_iso
            // gives YYYY-MM-DDTHH:MM:SSZ.
            auto const stampAt = contents.find("Stopped at:");
            if (BEAST_EXPECT(stampAt != std::string::npos))
            {
                auto const eol = contents.find('\n', stampAt);
                auto const line = contents.substr(stampAt, eol - stampAt);
                BEAST_EXPECT(line.find("20") != std::string::npos);
                BEAST_EXPECT(line.find('T') != std::string::npos);
                BEAST_EXPECT(line.find('Z') != std::string::npos);
            }

            // Now it can be removed, and removal is reported.
            ec.clear();
            BEAST_EXPECT(removeAmendmentBlockedFile(cfg, ec));
            BEAST_EXPECT(!ec);
            BEAST_EXPECT(!boost::filesystem::exists(path));

            // With no amendments to name the receipt still says something
            // useful.
            BEAST_EXPECT(!writeAmendmentBlockedFile(cfg, {}));
            ec.clear();
            auto const bare = getFileContents(ec, path);
            BEAST_EXPECT(!ec);
            BEAST_EXPECT(
                bare.find("not supported by this build") != std::string::npos);
        }

        // A directory we cannot write to must surface an error rather than
        // throw. The shutdown continues either way; only the receipt is lost,
        // which is what happens on installs where CONFIG_DIR is read-only for
        // the account xahaud runs as.
        {
            Config cfg;
            cfg.CONFIG_DIR =
                boost::filesystem::path{"/"} / "no" / "such" / "directory";
            BEAST_EXPECT(!!writeAmendmentBlockedFile(cfg, {}));

            boost::system::error_code ec;
            BEAST_EXPECT(!removeAmendmentBlockedFile(cfg, ec));
        }
    }

    void
    testStandaloneDoesNotStop()
    {
        testcase("standalone does not stop or leave a receipt");
        using namespace test::jtx;

        beast::temp_dir td;
        Env env{*this, envconfig([&](std::unique_ptr<Config> cfg) {
                    cfg->CONFIG_DIR = td.path();
                    return cfg;
                })};
        BEAST_EXPECT(env.app().config().standalone());

        auto const path = amendmentBlockedFilePath(env.app().config());
        env.app().getOPs().setAmendmentBlocked();

        BEAST_EXPECT(env.app().getOPs().isAmendmentBlocked());
        BEAST_EXPECT(!env.app().getOPs().isAmendmentWarned());
        BEAST_EXPECT(!env.app().isStopping());
        BEAST_EXPECT(!boost::filesystem::exists(path));
    }

    void
    testShutdownOnBlock()
    {
        testcase("amendment blocked stops the server");
        using namespace test::jtx;

        // NOTE: this is the only non-standalone Env in the test tree, and it
        // has to be, because setAmendmentBlocked() deliberately does nothing
        // in standalone mode. Consequences to be aware of when editing:
        // setup() arms the state timer, run() arms the deadlock detector, and
        // signalStop() below releases run() to tear the application down
        // concurrently with the rest of this function. Do all the assertions
        // straight away and let the Env go out of scope promptly.
        beast::temp_dir td;
        Env env{*this, envconfig([&](std::unique_ptr<Config> config) {
                    config->NODE_SIZE = 0;
                    config->setupControl(true, true, false);
                    // setupControl picks a production node size for
                    // non-standalone; put it back to "tiny" for the test.
                    config->NODE_SIZE = 0;
                    config->CONFIG_DIR = td.path();
                    config->legacy("database_path", td.path());
                    return config;
                })};
        BEAST_EXPECT(!env.app().config().standalone());

        auto const path = amendmentBlockedFilePath(env.app().config());
        BEAST_EXPECT(path.filename() == "README_AMENDMENT_BLOCKED");
        BEAST_EXPECT(!boost::filesystem::exists(path));
        BEAST_EXPECT(!env.app().isStopping());

        // Make the table report an unsupported amendment so the receipt has
        // something concrete to name.
        auto const id = unsupportedAmendmentId();
        env.app().getAmendmentTable().enable(id);
        BEAST_EXPECT(env.app().getAmendmentTable().hasUnsupportedEnabled());

        env.app().getOPs().setAmendmentBlocked();
        BEAST_EXPECT(env.app().getOPs().isAmendmentBlocked());
        BEAST_EXPECT(env.app().isStopping());
        BEAST_EXPECT(boost::filesystem::exists(path));

        boost::system::error_code readError;
        auto const contents = getFileContents(readError, path);
        BEAST_EXPECT(!readError);
        BEAST_EXPECT(
            contents.find("XAHAUD STOPPED: UPGRADE REQUIRED") !=
            std::string::npos);
        BEAST_EXPECT(contents.find("Upgrade xahaud") != std::string::npos);
        BEAST_EXPECT(contents.find(to_string(id)) != std::string::npos);
        BEAST_EXPECT(contents.find("already active") != std::string::npos);

        // Repeat calls are a no-op: Change::applyAmendment reaches this from
        // the transaction apply path without an isBlocked() guard, so it must
        // not rewrite the receipt or re-log once per ledger.
        BEAST_EXPECT(boost::filesystem::remove(path));
        env.app().getOPs().setAmendmentBlocked();
        BEAST_EXPECT(!boost::filesystem::exists(path));
    }

    void
    testWarnsOutsideShutdownWindow()
    {
        testcase("unsupported majority warns while activation is distant");
        using namespace test::jtx;

        Env env{*this, shortMajorityConfig()};
        BEAST_EXPECT(!env.app().getOPs().isBlocked());

        BEAST_EXPECT(env.close());
        injectUnsupportedMajority(env);
        BEAST_EXPECT(env.close());

        // A majority period out: warn, keep running.
        BEAST_EXPECT(env.app().getOPs().isAmendmentWarned());
        BEAST_EXPECT(!env.app().getOPs().isAmendmentBlocked());

        // The table can name the amendment, and reports it as pending rather
        // than active. This is what ends up in the receipt.
        auto const unsupported =
            env.app().getAmendmentTable().unsupportedAmendments();
        BEAST_EXPECT(unsupported.size() == 1);
        if (unsupported.size() == 1)
        {
            BEAST_EXPECT(unsupported[0].id == unsupportedAmendmentId());
            BEAST_EXPECT(
                unsupported[0].expected ==
                env.app().getAmendmentTable().firstUnsupportedExpected());
        }

        // Because firstUnsupportedExpected() is set, the warning carries the
        // expected activation date.
        auto const si = env.rpc("server_info")[jss::result];
        BEAST_EXPECT(si.isMember(jss::info));
        auto const& warnings = si[jss::info][jss::warnings];
        BEAST_EXPECT(warnings.isArray() && warnings.size() == 1);
        if (warnings.isArray() && warnings.size() == 1)
        {
            BEAST_EXPECT(
                warnings[0u][jss::id].asInt() == warnRPC_UNSUPPORTED_MAJORITY);
            auto const& details = warnings[0u][jss::details];
            BEAST_EXPECT(details.isMember(jss::expected_date));
            BEAST_EXPECT(details.isMember(jss::expected_date_UTC));
        }
    }

    void
    testUnsupportedAmendmentReporting()
    {
        testcase("unsupported amendments are reported for the receipt");
        using namespace test::jtx;

        Env env{*this, shortMajorityConfig()};
        auto& table = env.app().getAmendmentTable();
        BEAST_EXPECT(table.unsupportedAmendments().empty());

        // Majority, not yet active -> reported with an expected time.
        BEAST_EXPECT(env.close());
        injectUnsupportedMajority(env);
        auto pending = table.unsupportedAmendments();
        BEAST_EXPECT(pending.size() == 1);
        if (pending.size() == 1)
            BEAST_EXPECT(pending[0].expected.has_value());

        // Majority lost -> nothing to report. The list is recomputed from the
        // ledger each time, so it must not accumulate.
        auto const seq = env.closed()->info().seq;
        table.doValidatedLedger(seq, {}, {});
        BEAST_EXPECT(table.unsupportedAmendments().empty());
        BEAST_EXPECT(!table.firstUnsupportedExpected());

        // Active -> reported with no expected time.
        BEAST_EXPECT(table.enable(unsupportedAmendmentId()));
        BEAST_EXPECT(table.hasUnsupportedEnabled());
        auto const active = table.unsupportedAmendments();
        BEAST_EXPECT(active.size() == 1);
        if (active.size() == 1)
        {
            BEAST_EXPECT(active[0].id == unsupportedAmendmentId());
            BEAST_EXPECT(!active[0].expected);
        }
    }

    void
    testWarningVisibleWithoutAdmin()
    {
        testcase("unsupported majority warning is not admin only");
        using namespace test::jtx;

        // No closes here: ledger_accept is Role::ADMIN, server_info is
        // Role::USER, which is the whole point of the test.
        Env env{*this, envconfig(no_admin)};
        env.app().getOPs().setAmendmentWarned();
        BEAST_EXPECT(env.app().getOPs().isAmendmentWarned());

        auto const si = env.rpc("server_info")[jss::result];
        BEAST_EXPECT(si.isMember(jss::info));
        auto const& warnings = si[jss::info][jss::warnings];
        BEAST_EXPECT(warnings.isArray() && warnings.size() == 1);
        if (warnings.isArray() && warnings.size() == 1)
        {
            BEAST_EXPECT(
                warnings[0u][jss::id].asInt() == warnRPC_UNSUPPORTED_MAJORITY);
        }
    }

    void
    testShutsDownInsideShutdownWindow()
    {
        testcase("unsupported majority stops the server before activation");
        using namespace test::jtx;

        Env env{*this, shortMajorityConfig()};
        BEAST_EXPECT(!env.app().getOPs().isBlocked());

        BEAST_EXPECT(env.close());
        injectUnsupportedMajority(env);

        // First close only warns: activation is still a majority period away.
        BEAST_EXPECT(env.close());
        BEAST_EXPECT(env.app().getOPs().isAmendmentWarned());
        BEAST_EXPECT(!env.app().getOPs().isAmendmentBlocked());

        auto const expected =
            *env.app().getAmendmentTable().firstUnsupportedExpected();

        // Close two minutes short of the expected activation time. This is not
        // a flag ledger and the warning has already been issued, so the check
        // has to run on every validated ledger to see this at all.
        BEAST_EXPECT(env.close(expected - NetClock::duration{120}));
        BEAST_EXPECT(env.app().getOPs().isAmendmentBlocked());
        BEAST_EXPECT(!env.app().getOPs().isAmendmentWarned());

        // Standalone, so the flag is set but the server keeps running.
        BEAST_EXPECT(!env.app().isStopping());
    }

    void
    testShutsDownWhenActivationOverdue()
    {
        testcase("unsupported majority stops the server when overdue");
        using namespace test::jtx;

        Env env{*this, shortMajorityConfig()};
        BEAST_EXPECT(!env.app().getOPs().isBlocked());

        BEAST_EXPECT(env.close());
        injectUnsupportedMajority(env);

        auto const expected =
            *env.app().getAmendmentTable().firstUnsupportedExpected();

        // firstUnsupportedExpected() is a lower bound: the amendment actually
        // activates at the first flag ledger at or after it, so the server can
        // legitimately still be running an hour past it. That is the most
        // dangerous state, not the safest, and must stop the server.
        BEAST_EXPECT(env.close(expected + NetClock::duration{3600}));
        BEAST_EXPECT(env.app().getOPs().isAmendmentBlocked());
        BEAST_EXPECT(!env.app().getOPs().isAmendmentWarned());
    }

    void
    testBlockedMethods()
    {
        using namespace test::jtx;
        Env env{*this, envconfig([](std::unique_ptr<Config> cfg) {
                    cfg->loadFromString("[" SECTION_SIGNING_SUPPORT "]\ntrue");
                    return cfg;
                })};
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];
        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        Account const ali{"ali", KeyType::secp256k1};
        env.fund(XRP(10000), alice, bob, gw);
        env.memoize(ali);
        // This close() ensures that all the accounts get created and their
        // default ripple flag gets set before the trust lines are created.
        // Without it, the ordering manages to create alice's trust line with
        // noRipple set on gw's end. The existing tests pass either way, but
        // better to do it right.
        env.close();
        env.trust(USD(600), alice);
        env.trust(USD(700), bob);
        env(pay(gw, alice, USD(70)));
        env(pay(gw, bob, USD(50)));
        env.close();

        auto wsc = test::makeWSClient(env.app().config());

        auto current = env.current();
        // ledger_accept
        auto jr = env.rpc("ledger_accept")[jss::result];
        BEAST_EXPECT(jr[jss::ledger_current_index] == current->seq() + 1);
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // ledger_current
        jr = env.rpc("ledger_current")[jss::result];
        BEAST_EXPECT(jr[jss::ledger_current_index] == current->seq() + 1);
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // owner_info
        jr = env.rpc("owner_info", alice.human())[jss::result];
        BEAST_EXPECT(jr.isMember(jss::accepted) && jr.isMember(jss::current));
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // path_find
        Json::Value pf_req;
        pf_req[jss::subcommand] = "create";
        pf_req[jss::source_account] = alice.human();
        pf_req[jss::destination_account] = bob.human();
        pf_req[jss::destination_amount] =
            bob["USD"](20).value().getJson(JsonOptions::none);
        jr = wsc->invoke("path_find", pf_req)[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::alternatives) && jr[jss::alternatives].isArray() &&
            jr[jss::alternatives].size() == 1);
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // submit
        auto jt = env.jt(noop(alice));
        Serializer s;
        jt.stx->add(s);
        jr = env.rpc("submit", strHex(s.slice()))[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::engine_result) &&
            jr[jss::engine_result] == "tesSUCCESS");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // submit_multisigned
        env(signers(bob, 1, {{alice, 1}}), sig(bob));
        env(regkey(alice, ali));
        env.close();

        Json::Value set_tx;
        set_tx[jss::Account] = bob.human();
        set_tx[jss::TransactionType] = jss::AccountSet;
        set_tx[jss::Fee] = (8 * env.current()->fees().base).jsonClipped();
        set_tx[jss::Sequence] = env.seq(bob);
        set_tx[jss::SigningPubKey] = "";

        Json::Value sign_for;
        sign_for[jss::tx_json] = set_tx;
        sign_for[jss::account] = alice.human();
        sign_for[jss::secret] = ali.name();
        jr = env.rpc("json", "sign_for", to_string(sign_for))[jss::result];
        BEAST_EXPECT(jr[jss::status] == "success");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        Json::Value ms_req;
        ms_req[jss::tx_json] = jr[jss::tx_json];
        jr = env.rpc(
            "json", "submit_multisigned", to_string(ms_req))[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::engine_result) &&
            jr[jss::engine_result] == "tesSUCCESS");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // set up an amendment warning. Nothing changes

        env.app().getOPs().setAmendmentWarned();

        current = env.current();
        // ledger_accept
        jr = env.rpc("ledger_accept")[jss::result];
        BEAST_EXPECT(jr[jss::ledger_current_index] == current->seq() + 1);
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // ledger_current
        jr = env.rpc("ledger_current")[jss::result];
        BEAST_EXPECT(jr[jss::ledger_current_index] == current->seq() + 1);
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // owner_info
        jr = env.rpc("owner_info", alice.human())[jss::result];
        BEAST_EXPECT(jr.isMember(jss::accepted) && jr.isMember(jss::current));
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // path_find
        pf_req[jss::subcommand] = "create";
        pf_req[jss::source_account] = alice.human();
        pf_req[jss::destination_account] = bob.human();
        pf_req[jss::destination_amount] =
            bob["USD"](20).value().getJson(JsonOptions::none);
        jr = wsc->invoke("path_find", pf_req)[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::alternatives) && jr[jss::alternatives].isArray() &&
            jr[jss::alternatives].size() == 1);
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // submit
        jt = env.jt(noop(alice));
        s.erase();
        jt.stx->add(s);
        jr = env.rpc("submit", strHex(s.slice()))[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::engine_result) &&
            jr[jss::engine_result] == "tesSUCCESS");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // submit_multisigned
        env(signers(bob, 1, {{alice, 1}}), sig(bob));
        env(regkey(alice, ali));
        env.close();

        set_tx[jss::Account] = bob.human();
        set_tx[jss::TransactionType] = jss::AccountSet;
        set_tx[jss::Fee] = (8 * env.current()->fees().base).jsonClipped();
        set_tx[jss::Sequence] = env.seq(bob);
        set_tx[jss::SigningPubKey] = "";

        sign_for[jss::tx_json] = set_tx;
        sign_for[jss::account] = alice.human();
        sign_for[jss::secret] = ali.name();
        jr = env.rpc("json", "sign_for", to_string(sign_for))[jss::result];
        BEAST_EXPECT(jr[jss::status] == "success");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        ms_req[jss::tx_json] = jr[jss::tx_json];
        jr = env.rpc(
            "json", "submit_multisigned", to_string(ms_req))[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::engine_result) &&
            jr[jss::engine_result] == "tesSUCCESS");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // make the network amendment blocked...now all the same
        // requests should fail

        env.app().getOPs().setAmendmentBlocked();

        // ledger_accept
        jr = env.rpc("ledger_accept")[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::error) && jr[jss::error] == "amendmentBlocked");
        BEAST_EXPECT(jr[jss::status] == "error");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // ledger_current
        jr = env.rpc("ledger_current")[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::error) && jr[jss::error] == "amendmentBlocked");
        BEAST_EXPECT(jr[jss::status] == "error");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // owner_info
        jr = env.rpc("owner_info", alice.human())[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::error) && jr[jss::error] == "amendmentBlocked");
        BEAST_EXPECT(jr[jss::status] == "error");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // path_find
        jr = wsc->invoke("path_find", pf_req)[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::error) && jr[jss::error] == "amendmentBlocked");
        BEAST_EXPECT(jr[jss::status] == "error");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // submit
        jr = env.rpc("submit", strHex(s.slice()))[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::error) && jr[jss::error] == "amendmentBlocked");
        BEAST_EXPECT(jr[jss::status] == "error");
        BEAST_EXPECT(!jr.isMember(jss::warnings));

        // submit_multisigned
        set_tx[jss::Sequence] = env.seq(bob);
        sign_for[jss::tx_json] = set_tx;
        jr = env.rpc("json", "sign_for", to_string(sign_for))[jss::result];
        BEAST_EXPECT(jr[jss::status] == "success");
        ms_req[jss::tx_json] = jr[jss::tx_json];
        jr = env.rpc(
            "json", "submit_multisigned", to_string(ms_req))[jss::result];
        BEAST_EXPECT(
            jr.isMember(jss::error) && jr[jss::error] == "amendmentBlocked");
        BEAST_EXPECT(!jr.isMember(jss::warnings));
    }

public:
    void
    run() override
    {
        testReceiptFileHelpers();
        testStandaloneDoesNotStop();
        testShutdownOnBlock();
        testWarnsOutsideShutdownWindow();
        testUnsupportedAmendmentReporting();
        testWarningVisibleWithoutAdmin();
        testShutsDownInsideShutdownWindow();
        testShutsDownWhenActivationOverdue();
        testBlockedMethods();
    }
};

BEAST_DEFINE_TESTSUITE(AmendmentBlocked, app, ripple);

}  // namespace ripple
