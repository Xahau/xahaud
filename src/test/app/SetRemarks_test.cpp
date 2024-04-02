//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <ripple/core/ConfigSections.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {
struct SetRemarks_test : public beast::unit_test::suite
{
    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        for (bool const withRemarks : {false, true})
        {
            // If the Remarks amendment is not enabled, you cannot add remarks
            auto const amend =
                withRemarks ? features : features - featureRemarks;
            Env env{*this, amend};

            env.fund(XRP(1000), alice, bob);
            env.close();
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
            };
            auto const txResult = withRemarks ? ter(tesSUCCESS) : ter(temDISABLED);
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), txResult);
            env.close();
        }
    }

    void
    testPreflightInvalid(FeatureBitset features)
    {
        testcase("preflight invalid");
        using namespace jtx;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        Env env{*this, features};

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED
        // DA: testEnabled()

        // temMALFORMED: SetRemarks: Invalid flags set.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), txflags(tfClose), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Cannot set more than 32 remarks (or fewer than 1) in a txn.
        {
            std::vector<remarks::remark> marks;
            for (int i = 0; i < 0; ++i) {
                marks.push_back({"CAFE", "DEADBEEF", 0});
            }
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Cannot set more than 32 remarks (or fewer than 1) in a txn.
        {
            std::vector<remarks::remark> marks;
            for (int i = 0; i < 33; ++i) {
                marks.push_back({"CAFE", "DEADBEEF", 0});
            }
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: contained non-sfRemark field.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
            };
            Json::Value jv;
            jv[jss::TransactionType] = jss::SetRemarks;
            jv[jss::Account] = alice.human();
            jv[sfObjectID.jsonName] = strHex(keylet::account(alice).key);
            auto& ja = jv[sfRemarks.getJsonName()];
            for (std::size_t i = 0; i < 1; ++i)
            {
                ja[i][sfGenesisMint.jsonName] = Json::Value{};
                ja[i][sfGenesisMint.jsonName][jss::Amount] = STAmount(1).getJson(JsonOptions::none);
                ja[i][sfGenesisMint.jsonName][jss::Destination] = bob.human();
            }
            jv[sfRemarks.jsonName] = ja;
            env(jv, ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: duplicate RemarkName entry.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
                {"CAFE", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkName cannot be empty or larger than 256 chars.
        {
            std::vector<remarks::remark> marks = {
                {"", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkName cannot be empty or larger than 256 chars.
        {
            std::string const name((256 * 2) + 1, 'A');
            std::vector<remarks::remark> marks = {
                {name, "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Flags must be either tfImmutable or 0
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 2},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: A remark deletion cannot be marked immutable.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", std::nullopt, 1},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkValue cannot be empty or larger than 256 chars.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkValue cannot be empty or larger than 256 chars.
        {
            std::string const value((256 * 2) + 1, 'A');
            std::vector<remarks::remark> marks = {
                {"CAFE", value, 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(temMALFORMED));
            env.close();
        }
    }

    void
    testPreclaimInvalid(FeatureBitset features)
    {
        testcase("preclaim invalid");
        using namespace jtx;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        // Env env{*this, features};

        Env env{*this, envconfig(), features, nullptr,
            beast::severities::kWarning
        };

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED
        // terNO_ACCOUNT
        // tecNO_TARGET
        // getRemarksIssuer: ltACCOUNT_ROOT
        // getRemarksIssuer: ltOFFER
        // getRemarksIssuer: ltESCROW
        // getRemarksIssuer: ltTICKET
        // getRemarksIssuer: ltPAYCHAN
        // getRemarksIssuer: ltCHECK
        // getRemarksIssuer: ltDEPOSIT_PREAUTH
        // getRemarksIssuer: ltNFTOKEN_OFFER
        // getRemarksIssuer: ltURI_TOKEN
        // getRemarksIssuer: ltRIPPLE_STATE: bal < 0
        // getRemarksIssuer: ltRIPPLE_STATE: bal > 0
        // getRemarksIssuer: ltRIPPLE_STATE: !high & !low
        // getRemarksIssuer: ltRIPPLE_STATE: high & low
        // tecNO_PERMISSION: !issuer
        // tecNO_PERMISSION: issuer != _account
        // tecIMMUTABLE: SetRemarks: attempt to mutate an immutable remark.
        // tecCLAIM: SetRemarks: insane remarks accounting.
        // tecTOO_MANY_REMARKS: SetRemarks: an object may have at most 32 remarks.
    }

    void
    testDoApplyInvalid(FeatureBitset features)
    {
        testcase("doApply invalid");
        using namespace jtx;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        // Env env{*this, features};

        Env env{*this, envconfig(), features, nullptr,
            beast::severities::kWarning
        };

        env.fund(XRP(1000), alice, bob);
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // tefINTERNAL
        // tecNO_TARGET
        // tecNO_PERMISSION
        // tecINTERNAL
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testPreflightInvalid(features);
        // testPreclaimInvalid(features);
        // testDoApplyInvalid(features);
        // testCreate(features);
        // testDelete(features);
        // testUpdateIssuer(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetRemarks, app, ripple);
}  // namespace test
}  // namespace ripple
