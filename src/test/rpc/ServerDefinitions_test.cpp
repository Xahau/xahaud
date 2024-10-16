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

#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/beast/unit_test.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {

namespace test {

class ServerDefinitions_test : public beast::unit_test::suite
{
public:
    static Json::Value
    loadJson(std::string content)
    {
        // If the string is empty, return an empty Json::Value
        if (content.empty())
        {
            std::cout << "JSON string was empty"
                      << "\n";
            return {};
        }

        Json::Value jsonValue;
        Json::Reader reader;
        reader.parse(content, jsonValue);
        return jsonValue;
    }

    void
    testDefinitions(FeatureBitset features)
    {
        testcase("Definitions");

        using namespace test::jtx;

        {
            Env env(*this);
            auto const result = env.rpc("server_definitions");
            BEAST_EXPECT(!result[jss::result].isMember(jss::error));
            BEAST_EXPECT(result[jss::result].isMember(jss::FIELDS));
            BEAST_EXPECT(result[jss::result].isMember(jss::LEDGER_ENTRY_TYPES));
            BEAST_EXPECT(
                result[jss::result].isMember(jss::TRANSACTION_RESULTS));
            BEAST_EXPECT(result[jss::result].isMember(jss::TRANSACTION_TYPES));
            BEAST_EXPECT(result[jss::result].isMember(jss::TRANSACTION_FLAGS));
            BEAST_EXPECT(
                result[jss::result].isMember(jss::TRANSACTION_FLAGS_INDICES));
            BEAST_EXPECT(result[jss::result].isMember(jss::TYPES));
            BEAST_EXPECT(result[jss::result].isMember(jss::hash));
            BEAST_EXPECT(result[jss::result][jss::status] == "success");
        }
    }

    void
    testDefinitionsHash(FeatureBitset features)
    {
        testcase("Definitions Hash");

        using namespace test::jtx;
        // test providing the same hash
        {
            Env env(*this, features);
            auto const firstResult = env.rpc("server_definitions");
            auto const hash = firstResult[jss::result][jss::hash].asString();
            Json::Value params;
            params[jss::hash] = hash;
            auto const result =
                env.rpc("json", "server_definitions", to_string(params));
            BEAST_EXPECT(!result[jss::result].isMember(jss::error));
            BEAST_EXPECT(result[jss::result][jss::status] == "success");
            BEAST_EXPECT(!result[jss::result].isMember(jss::FIELDS));
            BEAST_EXPECT(
                !result[jss::result].isMember(jss::LEDGER_ENTRY_TYPES));
            BEAST_EXPECT(
                !result[jss::result].isMember(jss::TRANSACTION_RESULTS));
            BEAST_EXPECT(!result[jss::result].isMember(jss::TRANSACTION_TYPES));
            BEAST_EXPECT(!result[jss::result].isMember(jss::TRANSACTION_FLAGS));
            BEAST_EXPECT(
                !result[jss::result].isMember(jss::TRANSACTION_FLAGS_INDICES));
            BEAST_EXPECT(!result[jss::result].isMember(jss::TYPES));
            BEAST_EXPECT(result[jss::result].isMember(jss::hash));
        }

        // test providing a different hash
        {
            Env env(*this, features);
            std::string const hash =
                "54296160385A27154BFA70A239DD8E8FD4CC2DB7BA32D970BA3A5B132CF749"
                "D1";
            Json::Value params;
            params[jss::hash] = hash;
            auto const result =
                env.rpc("json", "server_definitions", to_string(params));
            BEAST_EXPECT(!result[jss::result].isMember(jss::error));
            BEAST_EXPECT(result[jss::result][jss::status] == "success");
            BEAST_EXPECT(result[jss::result].isMember(jss::FIELDS));
            BEAST_EXPECT(result[jss::result].isMember(jss::LEDGER_ENTRY_TYPES));
            BEAST_EXPECT(
                result[jss::result].isMember(jss::TRANSACTION_RESULTS));
            BEAST_EXPECT(result[jss::result].isMember(jss::TRANSACTION_TYPES));
            BEAST_EXPECT(result[jss::result].isMember(jss::TRANSACTION_FLAGS));
            BEAST_EXPECT(
                result[jss::result].isMember(jss::TRANSACTION_FLAGS_INDICES));
            BEAST_EXPECT(result[jss::result].isMember(jss::TYPES));
            BEAST_EXPECT(result[jss::result].isMember(jss::hash));
        }
    }

    void
    testNoParams(FeatureBitset features)
    {
        testcase("No Params, None Enabled");

        using namespace test::jtx;
        Env env{*this};

        std::map<std::string, VoteBehavior> const& votes =
            ripple::detail::supportedAmendments();

        auto jrr = env.rpc("server_definitions")[jss::result];
        if (!BEAST_EXPECT(jrr.isMember(jss::features)))
            return;
        for (auto const& feature : jrr[jss::features])
        {
            if (!BEAST_EXPECT(feature.isMember(jss::name)))
                return;
            // default config - so all should be disabled, and
            // supported. Some may be vetoed.
            bool expectVeto =
                (votes.at(feature[jss::name].asString()) ==
                 VoteBehavior::DefaultNo);
            bool expectObsolete =
                (votes.at(feature[jss::name].asString()) ==
                 VoteBehavior::Obsolete);
            BEAST_EXPECTS(
                feature.isMember(jss::enabled) &&
                    !feature[jss::enabled].asBool(),
                feature[jss::name].asString() + " enabled");
            BEAST_EXPECTS(
                feature.isMember(jss::vetoed) &&
                    feature[jss::vetoed].isBool() == !expectObsolete &&
                    (!feature[jss::vetoed].isBool() ||
                     feature[jss::vetoed].asBool() == expectVeto) &&
                    (feature[jss::vetoed].isBool() ||
                     feature[jss::vetoed].asString() == "Obsolete"),
                feature[jss::name].asString() + " vetoed");
            BEAST_EXPECTS(
                feature.isMember(jss::supported) &&
                    feature[jss::supported].asBool(),
                feature[jss::name].asString() + " supported");
        }
    }

    void
    testSomeEnabled(FeatureBitset features)
    {
        testcase("No Params, Some Enabled");

        using namespace test::jtx;
        Env env{
            *this, FeatureBitset(featureDepositAuth, featureDepositPreauth)};

        std::map<std::string, VoteBehavior> const& votes =
            ripple::detail::supportedAmendments();

        auto jrr = env.rpc("server_definitions")[jss::result];
        if (!BEAST_EXPECT(jrr.isMember(jss::features)))
            return;
        for (auto it = jrr[jss::features].begin();
             it != jrr[jss::features].end();
             ++it)
        {
            uint256 id;
            (void)id.parseHex(it.key().asString().c_str());
            if (!BEAST_EXPECT((*it).isMember(jss::name)))
                return;
            bool expectEnabled = env.app().getAmendmentTable().isEnabled(id);
            bool expectSupported =
                env.app().getAmendmentTable().isSupported(id);
            bool expectVeto =
                (votes.at((*it)[jss::name].asString()) ==
                 VoteBehavior::DefaultNo);
            bool expectObsolete =
                (votes.at((*it)[jss::name].asString()) ==
                 VoteBehavior::Obsolete);
            BEAST_EXPECTS(
                (*it).isMember(jss::enabled) &&
                    (*it)[jss::enabled].asBool() == expectEnabled,
                (*it)[jss::name].asString() + " enabled");
            if (expectEnabled)
                BEAST_EXPECTS(
                    !(*it).isMember(jss::vetoed),
                    (*it)[jss::name].asString() + " vetoed");
            else
                BEAST_EXPECTS(
                    (*it).isMember(jss::vetoed) &&
                        (*it)[jss::vetoed].isBool() == !expectObsolete &&
                        (!(*it)[jss::vetoed].isBool() ||
                         (*it)[jss::vetoed].asBool() == expectVeto) &&
                        ((*it)[jss::vetoed].isBool() ||
                         (*it)[jss::vetoed].asString() == "Obsolete"),
                    (*it)[jss::name].asString() + " vetoed");
            BEAST_EXPECTS(
                (*it).isMember(jss::supported) &&
                    (*it)[jss::supported].asBool() == expectSupported,
                (*it)[jss::name].asString() + " supported");
        }
    }

    void
    testWithMajorities(FeatureBitset features)
    {
        testcase("With Majorities");

        using namespace test::jtx;
        Env env{*this, envconfig(validator, "")};

        auto jrr = env.rpc("server_definitions")[jss::result];
        if (!BEAST_EXPECT(jrr.isMember(jss::features)))
            return;

        // at this point, there are no majorities so no fields related to
        // amendment voting
        for (auto const& feature : jrr[jss::features])
        {
            if (!BEAST_EXPECT(feature.isMember(jss::name)))
                return;
            BEAST_EXPECTS(
                !feature.isMember(jss::majority),
                feature[jss::name].asString() + " majority");
            BEAST_EXPECTS(
                !feature.isMember(jss::count),
                feature[jss::name].asString() + " count");
            BEAST_EXPECTS(
                !feature.isMember(jss::threshold),
                feature[jss::name].asString() + " threshold");
            BEAST_EXPECTS(
                !feature.isMember(jss::validations),
                feature[jss::name].asString() + " validations");
            BEAST_EXPECTS(
                !feature.isMember(jss::vote),
                feature[jss::name].asString() + " vote");
        }

        auto majorities = getMajorityAmendments(*env.closed());
        if (!BEAST_EXPECT(majorities.empty()))
            return;

        // close ledgers until the amendments show up.
        for (auto i = 0; i <= 256; ++i)
        {
            env.close();
            majorities = getMajorityAmendments(*env.closed());
            if (!majorities.empty())
                break;
        }

        // There should be at least 5 amendments.  Don't do exact comparison
        // to avoid maintenance as more amendments are added in the future.
        BEAST_EXPECT(majorities.size() >= 5);
        std::map<std::string, VoteBehavior> const& votes =
            ripple::detail::supportedAmendments();

        jrr = env.rpc("server_definitions")[jss::result];
        if (!BEAST_EXPECT(jrr.isMember(jss::features)))
            return;
        for (auto const& feature : jrr[jss::features])
        {
            if (!BEAST_EXPECT(feature.isMember(jss::name)))
                return;
            bool expectVeto =
                (votes.at(feature[jss::name].asString()) ==
                 VoteBehavior::DefaultNo);
            bool expectObsolete =
                (votes.at(feature[jss::name].asString()) ==
                 VoteBehavior::Obsolete);
            BEAST_EXPECTS(
                (expectVeto || expectObsolete) ^
                    feature.isMember(jss::majority),
                feature[jss::name].asString() + " majority");
            BEAST_EXPECTS(
                feature.isMember(jss::vetoed) &&
                    feature[jss::vetoed].isBool() == !expectObsolete &&
                    (!feature[jss::vetoed].isBool() ||
                     feature[jss::vetoed].asBool() == expectVeto) &&
                    (feature[jss::vetoed].isBool() ||
                     feature[jss::vetoed].asString() == "Obsolete"),
                feature[jss::name].asString() + " vetoed");
            BEAST_EXPECTS(
                feature.isMember(jss::count),
                feature[jss::name].asString() + " count");
            BEAST_EXPECTS(
                feature.isMember(jss::threshold),
                feature[jss::name].asString() + " threshold");
            BEAST_EXPECTS(
                feature.isMember(jss::validations),
                feature[jss::name].asString() + " validations");
            BEAST_EXPECT(
                feature[jss::count] ==
                ((expectVeto || expectObsolete) ? 0 : 1));
            BEAST_EXPECT(feature[jss::threshold] == 1);
            BEAST_EXPECT(feature[jss::validations] == 1);
            BEAST_EXPECTS(
                expectVeto || expectObsolete || feature[jss::majority] == 2540,
                "Majority: " + feature[jss::majority].asString());
        }
    }

    void
    testServerFeatures(FeatureBitset features)
    {
        testNoParams(features);
        testSomeEnabled(features);
        testWithMajorities(features);
    }

    void
    testServerDefinitions(FeatureBitset features)
    {
        testDefinitions(features);
        testDefinitionsHash(features);
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testServerDefinitions(sa);
        testServerFeatures(sa);
    }
};

BEAST_DEFINE_TESTSUITE(ServerDefinitions, rpc, ripple);

}  // namespace test
}  // namespace ripple
