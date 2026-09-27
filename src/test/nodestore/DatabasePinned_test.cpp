//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Ripple Labs Inc.

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
#include <xrpld/core/ConfigSections.h>
#include <xrpld/nodestore/Database.h>
#include <xrpld/nodestore/NodeObject.h>
#include <xrpld/nodestore/detail/DatabasePinnedImp.h>
#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/digest.h>

namespace ripple {
namespace test {

class DatabasePinned_test : public beast::unit_test::suite
{
    // Build an Env whose nodestore is wired up as DatabasePinnedImp.
    // pinned_type=rwdb is allowed in standalone mode (the catalogue
    // tests do the same).
    static std::unique_ptr<Config>
    pinnedEnvconfig()
    {
        auto cfg = jtx::envconfig();
        auto& nodeDb = cfg->section(ConfigSection::nodeDatabase());
        nodeDb.set("pinned_type", "rwdb");
        nodeDb.set("online_delete", "256");
        return cfg;
    }

    // Make a NodeObject with deterministic but unique-per-call hash so
    // we can store it and fetch it back.
    static std::shared_ptr<NodeObject>
    makeObject(NodeObjectType type, std::uint8_t marker)
    {
        Blob data(64, marker);
        // Hash is just a stable function of (type, marker) so each
        // (type, marker) pair has a distinct key.
        Serializer s;
        s.add32(static_cast<std::uint32_t>(type));
        s.add8(marker);
        auto const hash = sha512Half(makeSlice(s.peekData()));
        return NodeObject::createObject(type, std::move(data), hash);
    }

    // Borrow the running app's nodestore. With pinned_type configured
    // it must be a DatabasePinnedImp; otherwise the test environment
    // is set up wrong and we can't proceed.
    NodeStore::DatabasePinnedImp*
    getPinnedDb(jtx::Env& env)
    {
        auto* db = dynamic_cast<NodeStore::DatabasePinnedImp*>(
            &env.app().getNodeStore());
        BEAST_EXPECT(db != nullptr);
        return db;
    }

    void
    testStoreRoutingByType()
    {
        // The architectural core of DatabasePinnedImp: store() routes
        // by NodeObjectType. pinned* types go to the persistent
        // backend (which never rotates); everything else goes to the
        // rotating backend (which is rotated by online_delete). If
        // routing breaks, pinned data ends up rotating away and the
        // whole feature fails silently.
        testcase("store routing by NodeObjectType");

        using namespace jtx;
        Env env{*this, pinnedEnvconfig()};
        auto* db = getPinnedDb(env);
        if (!db)
            return;

        auto const pinnedBefore = db->pinnedStoreCount();
        auto const hotBefore = db->hotStoreCount();

        // Pinned types: should each increment pinnedStoreCount.
        // The store() implementation also reduces them to their hot
        // equivalents before serialization (single-byte on-disk type
        // field), but the routing decision happens first.
        for (auto const type :
             {pinnedLEDGER, pinnedACCOUNT_NODE, pinnedTRANSACTION_NODE})
        {
            db->store(
                type,
                Blob(64, 0xAB),
                sha512Half(
                    makeSlice(std::string{"pin-"} + std::to_string(type))),
                /*ledgerSeq=*/100);
        }

        // Hot types: should each increment hotStoreCount.
        for (auto const type :
             {hotLEDGER, hotACCOUNT_NODE, hotTRANSACTION_NODE})
        {
            db->store(
                type,
                Blob(64, 0xCD),
                sha512Half(
                    makeSlice(std::string{"hot-"} + std::to_string(type))),
                /*ledgerSeq=*/100);
        }

        // 3 pinned stores routed to persistent_, 3 hot to rotating_.
        BEAST_EXPECT(db->pinnedStoreCount() - pinnedBefore == 3u);
        BEAST_EXPECT(db->hotStoreCount() - hotBefore == 3u);
    }

    void
    testStoreFetchRoundTrip()
    {
        // Verifies that data routed to either backend is retrievable
        // through the unified Database interface (fetchNodeObject
        // returns the right object regardless of which backend it
        // landed in). This is the read-side counterpart to the
        // routing test — confirms callers don't need to know about
        // the dual-backend split.
        testcase("store/fetch round-trip across both backends");

        using namespace jtx;
        Env env{*this, pinnedEnvconfig()};
        auto& nodeStore = env.app().getNodeStore();

        // Store one object of each type with distinct content so the
        // fetched data is unambiguously the one we stored.
        struct Item
        {
            NodeObjectType type;
            std::uint8_t marker;
            std::shared_ptr<NodeObject> obj;
        };

        std::vector<Item> items{
            {pinnedLEDGER, 0x01, nullptr},
            {pinnedACCOUNT_NODE, 0x02, nullptr},
            {pinnedTRANSACTION_NODE, 0x03, nullptr},
            {hotLEDGER, 0x04, nullptr},
            {hotACCOUNT_NODE, 0x05, nullptr},
            {hotTRANSACTION_NODE, 0x06, nullptr},
        };

        for (auto& it : items)
        {
            it.obj = makeObject(it.type, it.marker);
            // Database::store takes (type, data, hash, ledgerSeq).
            // Pass a copy of the data because store() takes Blob&&.
            nodeStore.store(
                it.type,
                Blob(it.obj->getData()),
                it.obj->getHash(),
                /*ledgerSeq=*/100);
        }

        for (auto const& it : items)
        {
            auto const fetched =
                nodeStore.fetchNodeObject(it.obj->getHash(), 100);
            BEAST_EXPECT(fetched != nullptr);
            if (!fetched)
                continue;
            BEAST_EXPECT(fetched->getData() == it.obj->getData());
        }
    }

public:
    void
    run() override
    {
        testStoreRoutingByType();
        testStoreFetchRoundTrip();
    }
};

BEAST_DEFINE_TESTSUITE(DatabasePinned, nodestore, ripple);

}  // namespace test
}  // namespace ripple
