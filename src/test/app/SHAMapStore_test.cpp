//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

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
#include <test/jtx/envconfig.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/main/NodeStoreScheduler.h>
#include <xrpld/app/misc/SHAMapStore.h>
#include <xrpld/app/misc/SHAMapStoreImp.h>
#include <xrpld/app/misc/detail/OnlineDeleteRanges.h>
#include <xrpld/app/rdb/backend/SQLiteDatabase.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/nodestore/NodeObject.h>
#include <xrpld/nodestore/detail/DatabaseRotatingImp.h>
#include <xrpl/beast/utility/temp_dir.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <boost/filesystem.hpp>

#include <exception>
#include <limits>
#include <string>

namespace ripple {
namespace test {

class SHAMapStore_test : public beast::unit_test::suite
{
    static auto const deleteInterval = 8;

    static auto
    onlineDelete(std::unique_ptr<Config> cfg)
    {
        cfg->LEDGER_HISTORY = deleteInterval;
        auto& section = cfg->section(ConfigSection::nodeDatabase());
        section.set("online_delete", std::to_string(deleteInterval));
        return cfg;
    }

    // Same as onlineDelete, but on a node store that persists nodes, so
    // SHAMapStoreImp builds DatabaseRotatingImp and really rotates.
    // envconfig defaults to type=rwdb, which is the null node store.
    static auto
    onlineDeleteStoring(std::unique_ptr<Config> cfg, std::string const& path)
    {
        cfg = onlineDelete(std::move(cfg));
        auto& section = cfg->section(ConfigSection::nodeDatabase());
        section.set("type", "memory");
        section.set("path", path);
        return cfg;
    }

    static auto
    advisoryDelete(std::unique_ptr<Config> cfg)
    {
        cfg = onlineDelete(std::move(cfg));
        cfg->section(ConfigSection::nodeDatabase()).set("advisory_delete", "1");
        return cfg;
    }

    static auto
    pinnedPersistent(
        std::unique_ptr<Config> cfg,
        std::string const& dbPath,
        std::string const& nodePath,
        std::string const& pinnedPath)
    {
        cfg = onlineDelete(std::move(cfg));
        cfg->legacy("database_path", dbPath);
        cfg->overwrite(SECTION_RELATIONAL_DB, "backend", "sqlite");

        auto& section = cfg->section(ConfigSection::nodeDatabase());
        section.set("type", "rwdb");
        section.set("path", nodePath);
        section.set("pinned_type", "nudb");
        section.set("pinned_path", pinnedPath);
        return cfg;
    }

    bool
    goodLedger(
        jtx::Env& env,
        Json::Value const& json,
        std::string ledgerID,
        bool checkDB = false)
    {
        auto good = json.isMember(jss::result) &&
            !RPC::contains_error(json[jss::result]) &&
            json[jss::result][jss::ledger][jss::ledger_index] == ledgerID;
        if (!good || !checkDB)
            return good;

        auto const seq = json[jss::result][jss::ledger_index].asUInt();

        std::optional<LedgerInfo> oinfo =
            env.app().getRelationalDatabase().getLedgerInfoByIndex(seq);
        if (!oinfo)
            return false;
        const LedgerInfo& info = oinfo.value();

        const std::string outHash = to_string(info.hash);
        const LedgerIndex outSeq = info.seq;
        const std::string outParentHash = to_string(info.parentHash);
        const std::string outDrops = to_string(info.drops);
        const std::uint64_t outCloseTime =
            info.closeTime.time_since_epoch().count();
        const std::uint64_t outParentCloseTime =
            info.parentCloseTime.time_since_epoch().count();
        const std::uint64_t outCloseTimeResolution =
            info.closeTimeResolution.count();
        const std::uint64_t outCloseFlags = info.closeFlags;
        const std::string outAccountHash = to_string(info.accountHash);
        const std::string outTxHash = to_string(info.txHash);

        auto const& ledger = json[jss::result][jss::ledger];
        return outHash == ledger[jss::ledger_hash].asString() &&
            outSeq == seq &&
            outParentHash == ledger[jss::parent_hash].asString() &&
            outDrops == ledger[jss::total_coins].asString() &&
            outCloseTime == ledger[jss::close_time].asUInt() &&
            outParentCloseTime == ledger[jss::parent_close_time].asUInt() &&
            outCloseTimeResolution ==
            ledger[jss::close_time_resolution].asUInt() &&
            outCloseFlags == ledger[jss::close_flags].asUInt() &&
            outAccountHash == ledger[jss::account_hash].asString() &&
            outTxHash == ledger[jss::transaction_hash].asString();
    }

    bool
    bad(Json::Value const& json, error_code_i error = rpcLGR_NOT_FOUND)
    {
        return json.isMember(jss::result) &&
            RPC::contains_error(json[jss::result]) &&
            json[jss::result][jss::error_code] == error;
    }

    std::string
    getHash(Json::Value const& json)
    {
        BEAST_EXPECT(
            json.isMember(jss::result) &&
            json[jss::result].isMember(jss::ledger) &&
            json[jss::result][jss::ledger].isMember(jss::ledger_hash) &&
            json[jss::result][jss::ledger][jss::ledger_hash].isString());
        return json[jss::result][jss::ledger][jss::ledger_hash].asString();
    }

    void
    ledgerCheck(jtx::Env& env, int const rows, int const first)
    {
        const auto [actualRows, actualFirst, actualLast] =
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase())
                ->getLedgerCountMinMax();

        BEAST_EXPECT(actualRows == rows);
        BEAST_EXPECT(actualFirst == first);
        BEAST_EXPECT(actualLast == first + rows - 1);
    }

    void
    transactionCheck(jtx::Env& env, int const rows)
    {
        BEAST_EXPECT(
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase())
                ->getTransactionCount() == rows);
    }

    void
    accountTransactionCheck(jtx::Env& env, int const rows)
    {
        BEAST_EXPECT(
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase())
                ->getAccountTransactionCount() == rows);
    }

    int
    waitForReady(jtx::Env& env)
    {
        using namespace std::chrono_literals;

        auto& store = env.app().getSHAMapStore();

        int ledgerSeq = 3;
        store.rendezvous();
        BEAST_EXPECT(!store.getLastRotated());

        env.close();
        store.rendezvous();

        auto ledger = env.rpc("ledger", "validated");
        BEAST_EXPECT(goodLedger(env, ledger, std::to_string(ledgerSeq++)));

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        return ledgerSeq;
    }

public:
    void
    testClear()
    {
        using namespace std::chrono_literals;

        testcase("clearPrior");
        using namespace jtx;

        Env env(*this, envconfig(onlineDelete));

        auto& store = env.app().getSHAMapStore();
        env.fund(XRP(10000), noripple("alice"));

        ledgerCheck(env, 1, 2);
        transactionCheck(env, 0);
        accountTransactionCheck(env, 0);

        std::map<std::uint32_t, Json::Value const> ledgers;

        auto ledgerTmp = env.rpc("ledger", "0");
        BEAST_EXPECT(bad(ledgerTmp));

        ledgers.emplace(std::make_pair(1, env.rpc("ledger", "1")));
        BEAST_EXPECT(goodLedger(env, ledgers[1], "1"));

        ledgers.emplace(std::make_pair(2, env.rpc("ledger", "2")));
        BEAST_EXPECT(goodLedger(env, ledgers[2], "2"));

        ledgerTmp = env.rpc("ledger", "current");
        BEAST_EXPECT(goodLedger(env, ledgerTmp, "3"));

        ledgerTmp = env.rpc("ledger", "4");
        BEAST_EXPECT(bad(ledgerTmp));

        ledgerTmp = env.rpc("ledger", "100");
        BEAST_EXPECT(bad(ledgerTmp));

        auto const firstSeq = waitForReady(env);
        auto lastRotated = firstSeq - 1;

        for (auto i = firstSeq + 1; i < deleteInterval + firstSeq; ++i)
        {
            env.fund(XRP(10000), noripple("test" + std::to_string(i)));
            env.close();

            ledgerTmp = env.rpc("ledger", "current");
            BEAST_EXPECT(goodLedger(env, ledgerTmp, std::to_string(i)));
        }
        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        SQLiteDatabase* const db =
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase());
        BEAST_EXPECT(*db->getTransactionsMinLedgerSeq() == 3);

        for (auto i = 3; i < deleteInterval + lastRotated; ++i)
        {
            ledgers.emplace(
                std::make_pair(i, env.rpc("ledger", std::to_string(i))));
            BEAST_EXPECT(
                goodLedger(env, ledgers[i], std::to_string(i), true) &&
                getHash(ledgers[i]).length());
        }

        ledgerCheck(env, deleteInterval + 1, 2);
        transactionCheck(env, deleteInterval);
        accountTransactionCheck(env, 2 * deleteInterval);

        {
            // Closing one more ledger triggers a rotate
            env.close();

            auto ledger = env.rpc("ledger", "current");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(deleteInterval + 4)));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == deleteInterval + 3);
        lastRotated = store.getLastRotated();
        BEAST_EXPECT(lastRotated == 11);

        // That took care of the fake hashes
        ledgerCheck(env, deleteInterval + 1, 3);
        transactionCheck(env, deleteInterval);
        accountTransactionCheck(env, 2 * deleteInterval);

        // The last iteration of this loop should trigger a rotate
        for (auto i = lastRotated - 1; i < lastRotated + deleteInterval - 1;
             ++i)
        {
            env.close();

            ledgerTmp = env.rpc("ledger", "current");
            BEAST_EXPECT(goodLedger(env, ledgerTmp, std::to_string(i + 3)));

            ledgers.emplace(
                std::make_pair(i, env.rpc("ledger", std::to_string(i))));
            BEAST_EXPECT(
                store.getLastRotated() == lastRotated ||
                i == lastRotated + deleteInterval - 2);
            BEAST_EXPECT(
                goodLedger(env, ledgers[i], std::to_string(i), true) &&
                getHash(ledgers[i]).length());
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == deleteInterval + lastRotated);

        ledgerCheck(env, deleteInterval + 1, lastRotated);
        transactionCheck(env, 0);
        accountTransactionCheck(env, 0);
    }

    void
    testAutomatic(bool storing)
    {
        testcase(
            storing ? "automatic online_delete (rotating node store)"
                    : "automatic online_delete (null node store)");
        using namespace jtx;
        using namespace std::chrono_literals;

        beast::temp_dir nodeDir;
        Env env(
            *this,
            storing ? envconfig(onlineDeleteStoring, nodeDir.path())
                    : envconfig(onlineDelete));
        BEAST_EXPECT(
            storing ==
            (dynamic_cast<NodeStore::DatabaseRotating*>(
                 &env.app().getNodeStore()) != nullptr));
        auto& store = env.app().getSHAMapStore();

        auto ledgerSeq = waitForReady(env);
        auto lastRotated = ledgerSeq - 1;
        BEAST_EXPECT(store.getLastRotated() == lastRotated);
        BEAST_EXPECT(lastRotated != 2);

        // Because advisory_delete is unset,
        // "can_delete" is disabled.
        auto const canDelete = env.rpc("can_delete");
        BEAST_EXPECT(bad(canDelete, rpcNOT_ENABLED));

        // Close ledgers without triggering a rotate
        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        // The database will always have back to ledger 2,
        // regardless of lastRotated.
        ledgerCheck(env, ledgerSeq - 2, 2);
        BEAST_EXPECT(lastRotated == store.getLastRotated());

        {
            // Closing one more ledger triggers a rotate
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);
        BEAST_EXPECT(lastRotated != store.getLastRotated());

        lastRotated = store.getLastRotated();

        // Close enough ledgers to trigger another rotate
        for (; ledgerSeq < lastRotated + deleteInterval + 1; ++ledgerSeq)
        {
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        ledgerCheck(env, deleteInterval + 1, lastRotated);
        BEAST_EXPECT(lastRotated != store.getLastRotated());
    }

    void
    testCanDelete()
    {
        testcase("online_delete with advisory_delete");
        using namespace jtx;
        using namespace std::chrono_literals;

        // Same config with advisory_delete enabled
        Env env(*this, envconfig(advisoryDelete));
        auto& store = env.app().getSHAMapStore();

        auto ledgerSeq = waitForReady(env);
        auto lastRotated = ledgerSeq - 1;
        BEAST_EXPECT(store.getLastRotated() == lastRotated);
        BEAST_EXPECT(lastRotated != 2);

        auto canDelete = env.rpc("can_delete");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(canDelete[jss::result][jss::can_delete] == 0);

        canDelete = env.rpc("can_delete", "never");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(canDelete[jss::result][jss::can_delete] == 0);

        auto const firstBatch = deleteInterval + ledgerSeq;
        for (; ledgerSeq < firstBatch; ++ledgerSeq)
        {
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - 2, 2);
        BEAST_EXPECT(lastRotated == store.getLastRotated());

        // This does not kick off a cleanup
        canDelete = env.rpc(
            "can_delete", std::to_string(ledgerSeq + deleteInterval / 2));
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(
            canDelete[jss::result][jss::can_delete] ==
            ledgerSeq + deleteInterval / 2);

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - 2, 2);
        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off a cleanup, but it stays small.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;

        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            // No cleanups in this loop.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off another cleanup.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - firstBatch, firstBatch);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;

        // This does not kick off a cleanup
        canDelete = env.rpc("can_delete", "always");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(
            canDelete[jss::result][jss::can_delete] ==
            std::numeric_limits<unsigned int>::max());

        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            // No cleanups in this loop.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off another cleanup.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;

        // This does not kick off a cleanup
        canDelete = env.rpc("can_delete", "now");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(canDelete[jss::result][jss::can_delete] == ledgerSeq - 1);

        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            // No cleanups in this loop.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off another cleanup.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;
    }

    std::unique_ptr<NodeStore::Backend>
    makeBackendRotating(
        jtx::Env& env,
        NodeStoreScheduler& scheduler,
        std::string path)
    {
        Section section{
            env.app().config().section(ConfigSection::nodeDatabase())};
        boost::filesystem::path newPath;

        if (!BEAST_EXPECT(path.size()))
            return {};
        newPath = path;
        section.set("path", newPath.string());

        auto backend{NodeStore::Manager::instance().make_Backend(
            section,
            megabytes(env.app().config().getValueFor(
                SizedItem::burstSize, std::nullopt)),
            scheduler,
            env.app().logs().journal("NodeStoreTest"))};
        backend->open();
        return backend;
    }

    std::unique_ptr<NodeStore::Backend>
    makeMemoryBackend(
        jtx::Env& env,
        NodeStoreScheduler& scheduler,
        std::string const& name)
    {
        Section section;
        section.set("type", "memory");
        section.set("path", name);
        auto backend = NodeStore::Manager::instance().make_Backend(
            section,
            megabytes(env.app().config().getValueFor(
                SizedItem::burstSize, std::nullopt)),
            scheduler,
            env.app().logs().journal("NodeStoreTest"));
        backend->open();
        return backend;
    }

    // Ordinary fetch (duplicate == false) must copy an archived object
    // into the writable backend. The following rotation deletes the
    // archive. Without that copy, a transaction node read between
    // rotations disappears while callers can still name it.
    void
    testArchiveReadSurvivesRotation()
    {
        testcase("archive read survives rotation");

        using namespace jtx;
        Env env(*this, envconfig(onlineDelete));
        NodeStoreScheduler scheduler(env.app().getJobQueue());

        Section nscfg;
        nscfg.set("type", "memory");
        nscfg.set("path", "rotating");

        auto dbr = std::make_unique<NodeStore::DatabaseRotatingImp>(
            env.app(),
            scheduler,
            1,
            makeMemoryBackend(env, scheduler, "writable"),
            makeMemoryBackend(env, scheduler, "archive"),
            nscfg,
            env.app().logs().journal("NodeStoreTest"));

        Blob const expected(32, 0xab);
        auto const hash = sha512Half(makeSlice(expected));
        {
            Blob data = expected;
            dbr->store(hotTRANSACTION_NODE, std::move(data), hash, 1);
        }

        auto const matches = [&](std::shared_ptr<NodeObject> const& object) {
            return object && object->getType() == hotTRANSACTION_NODE &&
                object->getData() == expected;
        };

        // Public fetch defaults duplicate to false. The private override
        // is the rotation copy pass.
        NodeStore::Database& db = *dbr;
        BEAST_EXPECT(matches(db.fetchNodeObject(hash)));

        auto const nop = [](std::string const&, std::string const&) {};
        // Writable becomes the archive. The object now lives only there.
        dbr->rotate(makeMemoryBackend(env, scheduler, "writable-2"), nop);

        BEAST_EXPECT(matches(db.fetchNodeObject(hash)));

        dbr->rotate(makeMemoryBackend(env, scheduler, "writable-3"), nop);
        BEAST_EXPECT(matches(db.fetchNodeObject(hash)));
    }

    // getWriteLoad, sync, storeLedger, importDatabase and for_each changed
    // lock type. In null mode no Env builds a DatabaseRotatingImp, so
    // drive them directly.
    void
    testRotatingAccessors()
    {
        testcase("rotating database accessors");

        using namespace jtx;
        // Source ledger must come from a node store that holds its nodes.
        Env env(*this, envconfig([](std::unique_ptr<Config> cfg) {
            auto& section = cfg->section(ConfigSection::nodeDatabase());
            section.set("type", "memory");
            section.set("path", "rotating-accessors-app");
            return cfg;
        }));
        env.fund(XRP(10000), Account{"alice"});
        env.close();

        NodeStoreScheduler scheduler(env.app().getJobQueue());
        Section nscfg;
        nscfg.set("type", "memory");
        nscfg.set("path", "rotating-accessors");
        auto makeDb = [&](std::string const& prefix) {
            return std::make_unique<NodeStore::DatabaseRotatingImp>(
                env.app(),
                scheduler,
                1,
                makeMemoryBackend(env, scheduler, prefix + "-writable"),
                makeMemoryBackend(env, scheduler, prefix + "-archive"),
                nscfg,
                env.app().logs().journal("NodeStoreTest"));
        };

        auto src = makeDb("rotating-accessors-src");
        BEAST_EXPECT(src->getName() == "rotating-accessors-src-writable");
        BEAST_EXPECT(src->getWriteLoad() >= 0);
        src->sync();

        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        if (!BEAST_EXPECT(ledger))
            return;
        BEAST_EXPECT(src->storeLedger(ledger));

        NodeStore::Database& srcDb = *src;
        auto const stateRoot = ledger->stateMap().getHash().as_uint256();
        BEAST_EXPECT(srcDb.fetchNodeObject(ledger->info().hash));
        BEAST_EXPECT(srcDb.fetchNodeObject(stateRoot));

        // importDatabase walks the source with for_each.
        auto dst = makeDb("rotating-accessors-dst");
        dst->importDatabase(*src);
        NodeStore::Database& dstDb = *dst;
        BEAST_EXPECT(dstDb.fetchNodeObject(ledger->info().hash));
        BEAST_EXPECT(dstDb.fetchNodeObject(stateRoot));
    }

    static auto
    nullBackend(std::unique_ptr<Config> cfg)
    {
        cfg->LEDGER_HISTORY = 8;
        auto& section = cfg->section(ConfigSection::nodeDatabase());
        section.set("type", "rwdb");
        section.set("path", "main");
        return cfg;
    }

    static auto
    nullBackendFullHistory(std::unique_ptr<Config> cfg)
    {
        cfg = nullBackend(std::move(cfg));
        cfg->LEDGER_HISTORY = std::numeric_limits<std::uint32_t>::max();
        return cfg;
    }

    static auto
    nullBackendDeleteOff(std::unique_ptr<Config> cfg)
    {
        cfg = nullBackend(std::move(cfg));
        cfg->LEDGER_HISTORY = 256;
        cfg->section(ConfigSection::nodeDatabase()).set("online_delete", "0");
        return cfg;
    }

    // Payments keep applying after more closes than ledger_history,
    // with no node-store record of the state tree. Close time comes
    // from the resident ledger, not a node-store header walk.
    void
    testNullModeLedgerProgression()
    {
        testcase("null mode ledger progression");

        using namespace jtx;
        Env env(*this, envconfig(nullBackend));

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const bobBefore = env.balance(bob);
        constexpr int payments = 12;
        for (int i = 0; i < payments; ++i)
        {
            env(pay(alice, bob, XRP(1)));
            env.close();
        }

        BEAST_EXPECT(env.balance(bob) == bobBefore + XRP(payments));
        BEAST_EXPECT(env.seq(alice) > 1);

        auto const info = env.rpc(
            "json",
            "account_info",
            std::string{"{\"account\": \""} + alice.human() + "\"}");
        BEAST_EXPECT(
            info[jss::result].isMember(jss::account_data) &&
            info[jss::result][jss::account_data][jss::Account] ==
                alice.human());

        auto const ledger = env.app().getLedgerMaster().getClosedLedger();
        if (!BEAST_EXPECT(ledger))
            return;
        BEAST_EXPECT(ledger->read(keylet::account(alice.id())));

        auto const closeTime = env.app().getLedgerMaster().getCloseTimeByHash(
            ledger->info().hash, ledger->info().seq);
        BEAST_EXPECT(closeTime && *closeTime == ledger->info().closeTime);

        auto const validated = env.rpc("ledger", "validated");
        BEAST_EXPECT(goodLedger(
            env, validated, std::to_string(ledger->info().seq), true));
    }

    // ledger_history=full would make the retained-ledger window grow
    // without bound, since nothing else can drop a null-mode tree.
    void
    testNullModeRejectsFullHistory()
    {
        testcase("RWDB null mode rejects ledger_history=full");

        using namespace jtx;
        try
        {
            Env env(*this, envconfig(nullBackendFullHistory));
            fail("Env should throw when ledger_history is full");
        }
        catch (std::exception const& e)
        {
            BEAST_EXPECT(
                std::string(e.what()).find("ledger_history") !=
                std::string::npos);
        }
    }

    // online_delete=0 is an explicit disable. It must not be treated
    // as a missing key and replaced with ledger_history.
    void
    testExplicitOnlineDeleteZero()
    {
        testcase("explicit online_delete zero stays off");

        using namespace jtx;
        Env env(*this, envconfig(nullBackendDeleteOff));

        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        auto const balance = env.balance(alice);
        env.close();

        BEAST_EXPECT(env.app().getSHAMapStore().getLastRotated() == 0);
        BEAST_EXPECT(env.balance(alice) == balance);
    }

    void
    testNullModeRequiresHistory()
    {
        testcase("RWDB null mode requires ledger_history > 0");

        using namespace jtx;
        try
        {
            Env env(*this, envconfig([](std::unique_ptr<Config> cfg) {
                cfg->LEDGER_HISTORY = 0;
                auto& section = cfg->section(ConfigSection::nodeDatabase());
                section.set("type", "rwdb");
                section.set("path", "main");
                return cfg;
            }));
            fail("Env should throw when ledger_history is 0");
        }
        catch (std::exception const& e)
        {
            BEAST_EXPECT(
                std::string(e.what()).find("ledger_history") !=
                std::string::npos);
        }
    }

    void
    testRetainedLedgerCloseTime()
    {
        testcase("retained ledger close time survives later closes");

        using namespace jtx;
        Env env(*this, envconfig(nullBackend));

        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const first = env.app().getLedgerMaster().getClosedLedger();
        if (!BEAST_EXPECT(first))
            return;
        auto const firstHash = first->info().hash;
        auto const firstSeq = first->info().seq;
        auto const firstClose = first->info().closeTime;

        for (int i = 0; i < 12; ++i)
        {
            env(pay(alice, env.master, XRP(1)));
            env.close();
        }

        auto const closeTime =
            env.app().getLedgerMaster().getCloseTimeByHash(firstHash, firstSeq);
        BEAST_EXPECT(closeTime && *closeTime == firstClose);
        BEAST_EXPECT(env.balance(alice) < XRP(10000));
    }

    void
    testNullBackendIsPerConfig()
    {
        testcase("null backend flag is per configuration");

        Config rwdbCfg;
        Config upperCfg;
        Config nudbCfg;
        Config emptyCfg;
        rwdbCfg.section(ConfigSection::nodeDatabase()).set("type", "rwdb");
        upperCfg.section(ConfigSection::nodeDatabase()).set("type", "RWDB");
        nudbCfg.section(ConfigSection::nodeDatabase()).set("type", "NuDB");

        // Evaluated per Config object, not latched process-wide.
        BEAST_EXPECT(rwdbCfg.nullBackend());
        BEAST_EXPECT(upperCfg.nullBackend());
        BEAST_EXPECT(!nudbCfg.nullBackend());
        BEAST_EXPECT(!emptyCfg.nullBackend());
        BEAST_EXPECT(rwdbCfg.nullBackend());
    }

    // Resident lookups are used with peer-supplied values. They must find
    // in-memory ledgers but never load or edit the complete-ledger set.
    void
    testResidentLedgerLookup()
    {
        testcase("resident ledger lookup does not load or clear");

        using namespace jtx;
        Env env(*this, envconfig(nullBackend));

        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();
        for (int i = 0; i < 4; ++i)
            env.close();

        auto& lm = env.app().getLedgerMaster();
        auto const closed = lm.getClosedLedger();
        if (!BEAST_EXPECT(closed))
            return;

        auto const complete = lm.getCompleteLedgers();

        auto const bySeq = lm.getResidentLedgerBySeq(closed->info().seq);
        BEAST_EXPECT(bySeq && bySeq->info().hash == closed->info().hash);

        auto const byHash = lm.getResidentLedgerByHash(closed->info().hash);
        BEAST_EXPECT(byHash && byHash->info().seq == closed->info().seq);

        // A recent ledger still inside the retained window.
        auto const prior = lm.getResidentLedgerBySeq(closed->info().seq - 2);
        BEAST_EXPECT(prior && prior->info().seq == closed->info().seq - 2);

        BEAST_EXPECT(!lm.getResidentLedgerByHash(uint256{12345}));
        BEAST_EXPECT(!lm.getResidentLedgerBySeq(closed->info().seq + 1000));

        BEAST_EXPECT(lm.getCompleteLedgers() == complete);

        // Drop every ledger from the history cache. Null mode must still
        // reach recent ledgers through the closed/validated holders and
        // the retained window, without loading anything.
        env.app().getJobQueue().rendezvous();
        auto const older = lm.getResidentLedgerBySeq(closed->info().seq - 3);
        if (!BEAST_EXPECT(older))
            return;
        lm.clearLedgerCachePrior(closed->info().seq + 1);

        auto const closedAgain = lm.getResidentLedgerBySeq(closed->info().seq);
        BEAST_EXPECT(
            closedAgain && closedAgain->info().hash == closed->info().hash);
        auto const closedByHash =
            lm.getResidentLedgerByHash(closed->info().hash);
        BEAST_EXPECT(
            closedByHash && closedByHash->info().seq == closed->info().seq);

        auto const retainedBySeq = lm.getResidentLedgerBySeq(older->info().seq);
        BEAST_EXPECT(
            retainedBySeq && retainedBySeq->info().hash == older->info().hash);
        auto const retainedByHash =
            lm.getResidentLedgerByHash(older->info().hash);
        BEAST_EXPECT(
            retainedByHash && retainedByHash->info().seq == older->info().seq);

        BEAST_EXPECT(!lm.getResidentLedgerByHash(uint256{12345}));
        BEAST_EXPECT(lm.getCompleteLedgers() == complete);
    }

    void
    testNullFactoryDropsWrites()
    {
        testcase("type=rwdb uses NullFactory and drops node-store writes");

        using namespace jtx;
        Env env(*this, envconfig(nullBackend));

        auto& db = env.app().getNodeStore();
        uint256 const hash{2};
        Blob data(32, 7);
        db.store(hotACCOUNT_NODE, std::move(data), hash, 1);
        BEAST_EXPECT(!db.fetchNodeObject(hash, 1));
    }

    void
    testRotate()
    {
        // The only purpose of this test is to ensure that if something that
        // should never happen happens, we don't get a deadlock.
        testcase("rotate with lock contention");

        using namespace jtx;
        Env env(*this, envconfig(onlineDelete));

        /////////////////////////////////////////////////////////////
        // Create the backend. Normally, SHAMapStoreImp handles all these
        // details
        auto nscfg = env.app().config().section(ConfigSection::nodeDatabase());

        // Provide default values:
        if (!nscfg.exists("cache_size"))
            nscfg.set(
                "cache_size",
                std::to_string(env.app().config().getValueFor(
                    SizedItem::treeCacheSize, std::nullopt)));

        if (!nscfg.exists("cache_age"))
            nscfg.set(
                "cache_age",
                std::to_string(env.app().config().getValueFor(
                    SizedItem::treeCacheAge, std::nullopt)));

        NodeStoreScheduler scheduler(env.app().getJobQueue());

        std::string const writableDb = "write";
        std::string const archiveDb = "archive";
        auto writableBackend = makeBackendRotating(env, scheduler, writableDb);
        auto archiveBackend = makeBackendRotating(env, scheduler, archiveDb);

        // Create NodeStore with two backends to allow online deletion of
        // data
        constexpr int readThreads = 4;
        auto dbr = std::make_unique<NodeStore::DatabaseRotatingImp>(
            env.app(),
            scheduler,
            readThreads,
            std::move(writableBackend),
            std::move(archiveBackend),
            nscfg,
            env.app().logs().journal("NodeStoreTest"));

        /////////////////////////////////////////////////////////////
        // Check basic functionality
        using namespace std::chrono_literals;
        std::atomic<int> threadNum = 0;

        {
            auto newBackend = makeBackendRotating(
                env, scheduler, std::to_string(++threadNum));

            auto const cb = [&](std::string const& writableName,
                                std::string const& archiveName) {
                BEAST_EXPECT(writableName == "1");
                BEAST_EXPECT(archiveName == "write");
                // Ensure that dbr functions can be called from within the
                // callback
                BEAST_EXPECT(dbr->getName() == "1");
            };

            dbr->rotate(std::move(newBackend), cb);
        }
        BEAST_EXPECT(threadNum == 1);
        BEAST_EXPECT(dbr->getName() == "1");

        /////////////////////////////////////////////////////////////
        // Do something stupid. Try to re-enter rotate from inside the callback.
        {
            auto const cb = [&](std::string const& writableName,
                                std::string const& archiveName) {
                BEAST_EXPECT(writableName == "3");
                BEAST_EXPECT(archiveName == "2");
                // Ensure that dbr functions can be called from within the
                // callback
                BEAST_EXPECT(dbr->getName() == "3");
            };
            auto const cbReentrant = [&](std::string const& writableName,
                                         std::string const& archiveName) {
                BEAST_EXPECT(writableName == "2");
                BEAST_EXPECT(archiveName == "1");
                auto newBackend = makeBackendRotating(
                    env, scheduler, std::to_string(++threadNum));
                // Reminder: doing this is stupid and should never happen
                dbr->rotate(std::move(newBackend), cb);
            };
            auto newBackend = makeBackendRotating(
                env, scheduler, std::to_string(++threadNum));
            dbr->rotate(std::move(newBackend), cbReentrant);
        }

        BEAST_EXPECT(threadNum == 3);
        BEAST_EXPECT(dbr->getName() == "3");
    }

    void
    testPinnedRangeRestoreRequiresPinnedData()
    {
        testcase("pinned range restore requires pinned data");

        using namespace jtx;

        beast::temp_dir tempDir;
        auto const tempPath = boost::filesystem::path(tempDir.path());
        auto const dbPath = (tempPath / "db").string();
        auto const nodePath = (tempPath / "node").string();
        auto const pinnedPath = (tempPath / "pinned").string();

        boost::filesystem::create_directories(dbPath);
        boost::filesystem::create_directories(nodePath);
        boost::filesystem::create_directories(pinnedPath);

        {
            Env env(
                *this,
                envconfig(pinnedPersistent, dbPath, nodePath, pinnedPath));

            RangeSet<std::uint32_t> ranges;
            ranges.insert(range(100u, 200u));
            env.app().getSHAMapStore().setPinnedRanges(ranges);

            NodeStoreScheduler scheduler(env.app().getJobQueue());
            auto const journal = env.app().journal("SHAMapStoreTest");

            try
            {
                SHAMapStoreImp restoreCheck(env.app(), scheduler, journal);
                restoreCheck.start();
                fail(
                    "Expected startup failure for stale persisted pinned "
                    "ranges");
            }
            catch (std::runtime_error const& e)
            {
                BEAST_EXPECT(
                    std::string(e.what()).find(
                        "Persisted pinned interval 100-200") !=
                    std::string::npos);
            }
        }
    }

    // Helper: compare two RangeSet<uint32_t> for set-equality. The
    // test cares about which sequences are in the result, not the
    // particular interval representation.
    static bool
    sameRanges(
        RangeSet<std::uint32_t> const& a,
        RangeSet<std::uint32_t> const& b)
    {
        return a == b;
    }

    void
    testComputeOnlineDeleteTargets()
    {
        // detail::computeOnlineDeleteTargets is the lifted interval
        // arithmetic used by SHAMapStoreImp::clearSqlRanges to decide
        // which ledger sequences to actually delete during online
        // delete. The base window is [minSeq, lastRotated - 1] and
        // pinned ranges are subtracted from it. The result must:
        //   (a) be empty when the base is empty or fully pinned,
        //   (b) preserve all pinned seqs (they survive rotation),
        //   (c) cover every non-pinned seq in the base.
        // Lifting the math out keeps the contract testable without
        // standing up a full SHAMapStoreImp + database fixture.
        testcase("detail::computeOnlineDeleteTargets");

        using detail::computeOnlineDeleteTargets;

        // Empty base window (minSeq >= lastRotated): no work.
        {
            RangeSet<std::uint32_t> empty;
            BEAST_EXPECT(computeOnlineDeleteTargets(100, 100, empty).empty());
            BEAST_EXPECT(computeOnlineDeleteTargets(101, 100, empty).empty());
        }

        // No pins: the entire base window is the target.
        {
            RangeSet<std::uint32_t> empty;
            RangeSet<std::uint32_t> expected;
            expected.insert(range(50u, 99u));  // [50, 100-1]
            BEAST_EXPECT(sameRanges(
                computeOnlineDeleteTargets(50, 100, empty), expected));
        }

        // Pins fully cover the base window: nothing to delete. This
        // is the bug the pinning feature exists to prevent — pinned
        // ranges getting silently rotated away.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(50u, 99u));
            BEAST_EXPECT(computeOnlineDeleteTargets(50, 100, pinned).empty());
        }

        // Pins extend beyond the base window: still nothing to delete.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(0u, 1000u));
            BEAST_EXPECT(computeOnlineDeleteTargets(50, 100, pinned).empty());
        }

        // Pins cover only the leading section of the base. Result is
        // the trailing remainder.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(50u, 70u));
            RangeSet<std::uint32_t> expected;
            expected.insert(range(71u, 99u));
            BEAST_EXPECT(sameRanges(
                computeOnlineDeleteTargets(50, 100, pinned), expected));
        }

        // Pins cover only the trailing section. Result is leading.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(80u, 99u));
            RangeSet<std::uint32_t> expected;
            expected.insert(range(50u, 79u));
            BEAST_EXPECT(sameRanges(
                computeOnlineDeleteTargets(50, 100, pinned), expected));
        }

        // Pins cut a hole in the middle. Result is two disjoint
        // intervals — both get deleted, the hole is preserved.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(60u, 80u));
            RangeSet<std::uint32_t> expected;
            expected.insert(range(50u, 59u));
            expected.insert(range(81u, 99u));
            BEAST_EXPECT(sameRanges(
                computeOnlineDeleteTargets(50, 100, pinned), expected));
        }

        // Multiple disjoint pinned ranges all inside the base. Result
        // is the base minus all pinned holes.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(55u, 60u));
            pinned.insert(range(70u, 75u));
            pinned.insert(range(90u, 95u));
            RangeSet<std::uint32_t> expected;
            expected.insert(range(50u, 54u));
            expected.insert(range(61u, 69u));
            expected.insert(range(76u, 89u));
            expected.insert(range(96u, 99u));
            BEAST_EXPECT(sameRanges(
                computeOnlineDeleteTargets(50, 100, pinned), expected));
        }

        // Pinned ranges entirely outside the base window: no effect.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(0u, 49u));
            pinned.insert(range(100u, 200u));
            RangeSet<std::uint32_t> expected;
            expected.insert(range(50u, 99u));
            BEAST_EXPECT(sameRanges(
                computeOnlineDeleteTargets(50, 100, pinned), expected));
        }

        // Single-sequence base window with a single-sequence pin
        // matching exactly: empty result.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(99u, 99u));
            BEAST_EXPECT(computeOnlineDeleteTargets(99, 100, pinned).empty());
        }
    }

    void
    run() override
    {
        testComputeOnlineDeleteTargets();
        testClear();
        testAutomatic(false);
        testAutomatic(true);
        testRotatingAccessors();
        testCanDelete();
        testArchiveReadSurvivesRotation();
        testNullModeLedgerProgression();
        testNullModeRejectsFullHistory();
        testExplicitOnlineDeleteZero();
        testNullModeRequiresHistory();
        testRetainedLedgerCloseTime();
        testNullBackendIsPerConfig();
        testResidentLedgerLookup();
        testNullFactoryDropsWrites();
        testRotate();
        testPinnedRangeRestoreRequiresPinnedData();
    }
};

// VFALCO This test fails because of thread asynchronous issues
BEAST_DEFINE_TESTSUITE(SHAMapStore, app, ripple);

}  // namespace test
}  // namespace ripple
