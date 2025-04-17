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
#include <sstream>
#include <test/jtx.h>

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
        std::cout << "INFO: " << info << "\n";
    }

    void
    validateRemarks(
        ReadView const& view,
        uint256 const& id,
        std::vector<jtx::remarks::remark> const& marks)
    {
        using namespace jtx;
        auto const slep = view.read(keylet::unchecked(id));
        if (slep && slep->isFieldPresent(sfRemarks))
        {
            auto const& remarksObj = slep->getFieldArray(sfRemarks);
            BEAST_EXPECT(remarksObj.size() == marks.size());
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
                fee(XRP(1)),
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

        // temINVALID_FLAG: SetRemarks: Invalid flags set.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                txflags(tfClose),
                fee(XRP(1)),
                ter(temINVALID_FLAG));
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
                fee(XRP(1)),
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
                fee(XRP(1)),
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
            env(jv, fee(XRP(1)), ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: duplicate RemarkName entry.
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
                {"CAFE", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                fee(XRP(1)),
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
                fee(XRP(1)),
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
                fee(XRP(1)),
                ter(temMALFORMED));
            env.close();
        }
        // temMALFORMED: SetRemarks: Flags must be either tfImmutable or 0
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 2},
            };
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                fee(XRP(1)),
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
                fee(XRP(1)),
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
                fee(XRP(1)),
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
                fee(XRP(1)),
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
            auto tx =
                remarks::setRemarks(carol, keylet::account(carol).key, marks);
            tx[jss::Sequence] = 0;
            env(tx, carol, fee(XRP(1)), ter(terNO_ACCOUNT));
            env.close();
        }

        // tecNO_TARGET - object doesnt exist
        {
            env(remarks::setRemarks(alice, keylet::account(carol).key, marks),
                fee(XRP(1)),
                ter(tecNO_TARGET));
            env.close();
        }

        // tecNO_PERMISSION: !issuer
        {
            env(deposit::auth(bob, alice));
            env(remarks::setRemarks(
                    alice, keylet::depositPreauth(bob, alice).key, marks),
                fee(XRP(1)),
                ter(tecNO_PERMISSION));
            env.close();
        }
        // tecNO_PERMISSION: issuer != _account
        {
            env(remarks::setRemarks(alice, keylet::account(bob).key, marks),
                fee(XRP(1)),
                ter(tecNO_PERMISSION));
            env.close();
        }
        // tecIMMUTABLE: SetRemarks: attempt to mutate an immutable remark.
        {
            // alice creates immutable remark
            std::vector<remarks::remark> immutableMarks = {
                {"CAFF", "DEAD", tfImmutable},
            };
            env(remarks::setRemarks(
                    alice, keylet::account(alice).key, immutableMarks),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // alice cannot update immutable remark
            std::vector<remarks::remark> badMarks = {
                {"CAFF", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(
                    alice, keylet::account(alice).key, badMarks),
                fee(XRP(1)),
                ter(tecIMMUTABLE));
            env.close();
        }
        // tecCLAIM: SetRemarks: insane remarks accounting.
        {}  // tecTOO_MANY_REMARKS: SetRemarks: an object may have at most 32
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
            env(remarks::setRemarks(alice, keylet::account(alice).key, _marks),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();
            env(remarks::setRemarks(alice, keylet::account(alice).key, marks),
                fee(XRP(1)),
                ter(tecTOO_MANY_REMARKS));
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

        // All checks in doApply are done in preclaim.
        BEAST_EXPECT(1);
    }

    void
    testDelete(FeatureBitset features)
    {
        testcase("delete");
        using namespace jtx;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const id = keylet::account(alice).key;

        // Set Remarks
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", "DEADBEEF", 0},
            };
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }

        // Delete Remarks
        {
            std::vector<remarks::remark> marks = {
                {"CAFE", std::nullopt, 0},
            };
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, {});
        }
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
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, bob, gw);
        env.close();
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env.close();

        std::vector<remarks::remark> marks = {
            {"CAFE", "DEADBEEF", 0},
        };

        // ltACCOUNT_ROOT
        {
            auto const id = keylet::account(alice).key;
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltOFFER
        {
            auto const id = keylet::offer(alice, env.seq(alice)).key;
            env(offer(alice, XRP(10), USD(10)), fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltESCROW
        {
            using namespace std::literals::chrono_literals;
            auto const id = keylet::escrow(alice, env.seq(alice)).key;
            env(escrow::create(alice, bob, XRP(10)),
                escrow::finish_time(env.now() + 1s),
                fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltTICKET
        {
            auto const id = keylet::ticket(alice, env.seq(alice) + 1).key;
            env(ticket::create(alice, 10), fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltPAYCHAN
        {
            using namespace std::literals::chrono_literals;
            auto const id = keylet::payChan(alice, bob, env.seq(alice)).key;
            auto const pk = alice.pk();
            auto const settleDelay = 100s;
            env(paychan::create(alice, bob, XRP(10), settleDelay, pk),
                fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltCHECK
        {
            auto const id = keylet::check(alice, env.seq(alice)).key;
            env(check::create(alice, bob, XRP(10)), fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltDEPOSIT_PREAUTH
        {
            env(fset(bob, asfDepositAuth));
            auto const id = keylet::depositPreauth(alice, bob).key;
            env(deposit::auth(alice, bob), fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltURI_TOKEN
        {
            std::string const uri(256, 'A');
            auto const id =
                keylet::uritoken(alice, Blob(uri.begin(), uri.end())).key;
            env(uritoken::mint(alice, uri), fee(XRP(1)));
            env(remarks::setRemarks(alice, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltRIPPLE_STATE: bal < 0
        {
            auto const alice2 = Account("alice2");
            env.fund(XRP(1000), alice2);
            env.close();
            env.trust(USD(10000), alice2);
            auto const id = keylet::line(alice2, USD).key;
            env(pay(gw, alice2, USD(1000)));
            env(remarks::setRemarks(gw, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltRIPPLE_STATE: bal > 0
        {
            auto const carol0 = Account("carol0");
            env.fund(XRP(1000), carol0);
            env.close();
            env.trust(USD(10000), carol0);
            auto const id = keylet::line(carol0, USD).key;
            env(pay(gw, carol0, USD(1000)));
            env(remarks::setRemarks(gw, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltRIPPLE_STATE: highReserve
        {
            auto const dan1 = Account("dan1");
            env.fund(XRP(1000), dan1);
            env.close();
            env.trust(USD(1000), dan1);
            auto const id = keylet::line(dan1, USD).key;
            env(remarks::setRemarks(gw, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
        // ltRIPPLE_STATE: lowReserve
        {
            auto const bob0 = Account("bob0");
            env.fund(XRP(1000), bob0);
            env.close();
            env.trust(USD(1000), bob0);
            auto const id = keylet::line(bob0, USD).key;
            env(remarks::setRemarks(gw, id, marks), fee(XRP(1)));
            env.close();
            validateRemarks(*env.current(), id, marks);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testPreflightInvalid(features);
        testPreclaimInvalid(features);
        testDoApplyInvalid(features);
        testDelete(features);
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
