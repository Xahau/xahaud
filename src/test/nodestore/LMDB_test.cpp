//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/misc/Manifest.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/app/rdb/Wallet.h>
#include <ripple/app/rdb/backend/LMDBDatabase.h>
#include <ripple/core/DatabaseCon.h>
#include <ripple/protocol/SecretKey.h>
#include <test/jtx.h>
#include <test/jtx/envconfig.h>
#include <test/nodestore/TestBase.h>
#include <test/unit_test/SuiteJournal.h>

namespace ripple {
namespace test {
struct LMDB_test : public beast::unit_test::suite
{
    void
    testNodeIdentity(FeatureBitset features)
    {
        testcase("getNodeIdentity");

        using namespace jtx;
        Env env = [&]() {
            auto p = test::jtx::envconfig();
            {
                auto& section = p->section("relational_db");
                section.set("backend", "lmdb");
            }
            p->RELATIONAL_DB = 1;

            return Env(*this, std::move(p), features);
        }();
        auto const s = setup_DatabaseCon(env.app().config());
        auto db = env.app().getWalletDB().checkoutLMDB();
        std::pair<PublicKey, SecretKey> pair = getNodeIdentity(db.get());
        BEAST_EXPECT(pair.first.size() == 33);
        BEAST_EXPECT(pair.second.size() == 32);
        clearNodeIdentity(db.get());
        std::pair<PublicKey, SecretKey> pair2 = getNodeIdentity(db.get());
        BEAST_EXPECT(pair.first != pair2.first);
        BEAST_EXPECT(pair.second != pair2.second);
    }

    void
    testGetHashByIndex(FeatureBitset features)
    {
        testcase("getHashByIndex");

        using namespace jtx;
        Env env = [&]() {
            auto p = test::jtx::envconfig();
            {
                auto& section = p->section("relational_db");
                section.set("backend", "lmdb");
            }
            p->RELATIONAL_DB = 1;

            return Env(*this, std::move(p), features);
        }();
        auto const s = setup_DatabaseCon(env.app().config());
        auto const db = dynamic_cast<LMDBDatabase*>(&env.app().getRelationalDatabase());
        env.close();

        auto const resHash = db->getHashByIndex(env.current()->info().seq - 1);
        BEAST_EXPECT(resHash == env.current()->info().hash);
    }

    void
    testGetLedgerInfoByHash(FeatureBitset features)
    {
        testcase("getLedgerInfoByHash");

        using namespace jtx;
        Env env = [&]() {
            auto p = test::jtx::envconfig();
            {
                auto& section = p->section("relational_db");
                section.set("backend", "lmdb");
            }
            p->RELATIONAL_DB = 1;

            return Env(*this, std::move(p), features);
        }();
        auto const s = setup_DatabaseCon(env.app().config());
        auto const db = dynamic_cast<LMDBDatabase*>(&env.app().getRelationalDatabase());
        env.close();

        auto const info = db->getLedgerInfoByHash(env.current()->info().hash);
        BEAST_EXPECT(info->hash == env.current()->info().hash);
        BEAST_EXPECT(info->seq == env.current()->info().seq - 1);
    }

    void
    testGetLedgerInfoByIndex(FeatureBitset features)
    {
        testcase("getLedgerInfoByIndex");

        using namespace jtx;
        Env env = [&]() {
            auto p = test::jtx::envconfig();
            {
                auto& section = p->section("relational_db");
                section.set("backend", "lmdb");
            }
            p->RELATIONAL_DB = 1;

            return Env(*this, std::move(p), features);
        }();
        auto const s = setup_DatabaseCon(env.app().config());
        auto const db = dynamic_cast<LMDBDatabase*>(&env.app().getRelationalDatabase());
        env.close();

        auto const info = db->getLedgerInfoByIndex(env.current()->info().seq - 1);
        BEAST_EXPECT(info->hash == env.current()->info().hash);
        BEAST_EXPECT(info->seq == env.current()->info().seq - 1);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testNodeIdentity(features);
        testGetHashByIndex(features);
        testGetLedgerInfoByHash(features);
        testGetLedgerInfoByIndex(features);
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

BEAST_DEFINE_TESTSUITE(LMDB, NodeStore, ripple);

}  // namespace test
}  // namespace ripple
