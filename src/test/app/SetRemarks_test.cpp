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
#include <sstream>

namespace ripple {
namespace test {
struct SetRemarks_test : public beast::unit_test::suite
{

    // debugRemarks(env, keylet::account(alice).key);
    void
    debugRemarks(jtx::Env& env, uint256 const& id)
    {
        Json::Value params;
        params[jss::index] = strHex(id);
        auto const info = env.rpc("json", "ledger_entry", to_string(params));
        std::cout << "Remarks: " << info[jss::result][jss::node][sfRemarks.jsonName] << "\n";
    }

    void
    validateRemarks(
        ReadView const& view,
        uint256 const& id,
        std::vector<jtx::remarks::remark> const& marks)
    {
        using namespace jtx;
        auto const slep = view.read(keylet::unchecked(id));
        if (slep->isFieldPresent(sfRemarks))
        {
            auto const& remarksObj = slep->getFieldArray(sfRemarks);
            for (int i = 0; i < marks.size(); ++i)
            {
                remarks::remark const expectedMark = marks[i];
                STObject const remark = remarksObj[i];
                
                Blob name = remark.getFieldVL(sfRemarkName);
                // BEAST_EXPECT(expectedMark.name == name);
                
                uint32_t flags = remark.isFieldPresent(sfFlags)
                    ? remark.getFieldU32(sfFlags)
                    : 0;
                BEAST_EXPECT(expectedMark.flags == flags);

                std::optional<Blob> val;
                if (remark.isFieldPresent(sfRemarkValue))
                    val = remark.getFieldVL(sfRemarkValue);
                    // BEAST_EXPECT(expectedMark.value == val);
            }
        }
    }

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
            auto const txResult =
                withRemarks ? ter(tesSUCCESS) : ter(temDISABLED);
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                txResult);
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
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                txflags(tfClose),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Cannot set more than 32 remarks (or fewer
        // than 1) in a txn.
        {
            std::vector<remarks::remark> marks;
            for (int i = 0; i < 0; ++i)
            {
                marks.push_back({"CAFE", "DEADBEEF", 0});
            }
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Cannot set more than 32 remarks (or fewer
        // than 1) in a txn.
        {
            std::vector<remarks::remark> marks;
            for (int i = 0; i < 33; ++i)
            {
                marks.push_back({"CAFE", "DEADBEEF", 0});
            }
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
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
                ja[i][sfGenesisMint.jsonName][jss::Amount] =
                    STAmount(1).getJson(JsonOptions::none);
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
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkName cannot be empty or larger than
        // 256 chars.
        {
            std::vector<remarks::remark> marks = {
                {"", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkName cannot be empty or larger than
        // 256 chars.
        {
            std::string const name((256 * 2) + 1, 'A');
            std::vector<remarks::remark> marks = {
                {name, "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Flags must be either tfImmutable or 0
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 2},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: A remark deletion cannot be marked
        // immutable.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", std::nullopt, 1},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkValue cannot be empty or larger than
        // 256 chars.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: RemarkValue cannot be empty or larger than
        // 256 chars.
        {
            std::string const value((256 * 2) + 1, 'A');
            std::vector<remarks::remark> marks = {
                {"CAFE", value, 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                ter(temMALFORMED));
            env.close();
        }
    }

    void
    testPreclaimInvalid(FeatureBitset features)
    {
        testcase("preclaim invalid");
        using namespace jtx;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        env.memoize(carol);
        env.fund(XRP(1000), alice, bob);
        env.close();

        std::vector<remarks::remark> marks = {
            {"CAFE", "DEADBEEF", 0},
        };

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED
        // DA: testEnabled()

        // terNO_ACCOUNT - account doesnt exist
        {
            auto const carol = Account("carol");
            env.memoize(carol);
            auto tx = remarks::setRemarks(carol, keylet::account(carol).key, marks);
            tx[jss::Sequence] = 0;
            env(tx, carol, ter(terNO_ACCOUNT));
            env.close();
        }

        // tecNO_TARGET - object doesnt exist
        {
            env(remarks::setRemarks(alice, keylet::account(carol).key, marks), ter(tecNO_TARGET));
            env.close();
        }

        // tecNO_PERMISSION: !issuer
        {
            env(deposit::auth(bob, alice));
            env(remarks::setRemarks(alice, keylet::depositPreauth(bob, alice).key, marks), ter(tecNO_PERMISSION));
            env.close();
        }
        // tecNO_PERMISSION: issuer != _account
        {
            env(remarks::setRemarks(alice, keylet::account(bob).key, marks), ter(tecNO_PERMISSION));
            env.close();
        }
        // tecIMMUTABLE: SetRemarks: attempt to mutate an immutable remark.
        {
            // alice creates immutable remark
            std::vector<remarks::remark> immutableMarks = {
                {"CAFF", "DEAD", tfImmutable},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, immutableMarks), ter(tesSUCCESS));
            env.close();

            // alice cannot update immutable remark
            std::vector<remarks::remark> badMarks = {
                {"CAFF", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, badMarks), ter(tecIMMUTABLE));
            env.close();
        }
        // tecCLAIM: SetRemarks: insane remarks accounting.
        {

        }
        // tecTOO_MANY_REMARKS: SetRemarks: an object may have at most 32
        // remarks.
        {
            std::vector<remarks::remark> _marks;
            unsigned int hexValue = 0xEFAC;
            for (int i = 0; i < 31; ++i)
            {
                std::stringstream ss;
                ss << std::hex << std::uppercase << hexValue;
                _marks.push_back({ss.str(), "DEADBEEF", 0});
                hexValue++;
            }
            env(remarks::setRemarks(alice, keylet::account(alice).key, _marks), ter(tesSUCCESS));
            env.close();
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks), ter(tecTOO_MANY_REMARKS));
            env.close();
        }
    }

    void
    testDoApplyInvalid(FeatureBitset features)
    {
        testcase("doApply invalid");
        using namespace jtx;

        //----------------------------------------------------------------------
        // doApply

        // terNO_ACCOUNT
        // tecNO_TARGET
        {
            // setup env
            Env env{*this, features};
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

        }
        // tecNO_PERMISSION
        // tecTOO_MANY_REMARKS
    }

    void
    testLedgerObjects(FeatureBitset features)
    {
        testcase("ledger objects");
        using namespace jtx;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        std::vector<remarks::remark> marks = {
            {"CAFE", "DEADBEEF", 0},
        };

        // getRemarksIssuer: ltACCOUNT_ROOT
        {
            auto const id = keylet::account(alice).key;
            env(remarks::setRemarks(alice, id, marks));
            env.close();
            debugRemarks(env, id);
            validateRemarks(*env.current(), id, marks);
        }
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
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testEnabled(features);
        // testPreflightInvalid(features);
        // testPreclaimInvalid(features);
        // testDoApplyInvalid(features);
        // testDelete(features);
        testLedgerObjects(features);
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
