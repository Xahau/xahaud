//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <test/shamap/common.h>
#include <test/unit_test/SuiteJournal.h>
#include <xrpld/app/ledger/ManifestSync.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/ManifestLedger.h>
#include <xrpld/ledger/RawView.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/basics/contract.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>
#include <limits>
#include <map>

namespace ripple {
namespace test {

namespace {

std::uint32_t constexpr revocation = std::numeric_limits<std::uint32_t>::max();

struct Validator
{
    SecretKey masterSecret;
    PublicKey master;
    SecretKey signingSecret;
    PublicKey signing;

    AccountID
    account() const
    {
        return calcAccountID(master);
    }
};

Validator
makeValidator()
{
    auto const ms = randomSecretKey();
    auto const ss = randomSecretKey();
    return {
        ms,
        derivePublicKey(KeyType::ed25519, ms),
        ss,
        derivePublicKey(KeyType::secp256k1, ss)};
}

/** The same master key with a fresh ephemeral key. */
Validator
rotate(Validator const& v)
{
    auto const ss = randomSecretKey();
    return {
        v.masterSecret, v.master, ss, derivePublicKey(KeyType::secp256k1, ss)};
}

STObject
signManifest(Validator const& v, std::uint32_t seq)
{
    STObject st(sfGeneric);
    st[sfSequence] = seq;
    st[sfPublicKey] = v.master;

    if (seq != revocation)
    {
        st[sfSigningPubKey] = v.signing;
        sign(st, HashPrefix::manifest, KeyType::secp256k1, v.signingSecret);
    }

    sign(
        st,
        HashPrefix::manifest,
        KeyType::ed25519,
        v.masterSecret,
        sfMasterSignature);
    return st;
}

/** Ledger state in a std::map.

    Enough of a ReadView to build on with a Sandbox, and a RawView to apply the
    Sandbox to, which is all writeManifestObjects() needs.
*/
class MemoryState : public ReadView, public RawView
{
    std::map<uint256, std::shared_ptr<SLE>> state_;
    LedgerInfo info_;
    Fees fees_;
    // Rules keeps a reference to its presets, so they live here.
    std::unordered_set<uint256, beast::uhash<>> presets_;
    Rules rules_{presets_};

public:
    MemoryState()
    {
        info_.seq = 2;
    }

    std::map<uint256, std::shared_ptr<SLE>> const&
    state() const
    {
        return state_;
    }

    LedgerInfo const&
    info() const override
    {
        return info_;
    }

    bool
    open() const override
    {
        return false;
    }

    Fees const&
    fees() const override
    {
        return fees_;
    }

    Rules const&
    rules() const override
    {
        return rules_;
    }

    bool
    exists(Keylet const& k) const override
    {
        return static_cast<bool>(read(k));
    }

    std::optional<key_type>
    succ(key_type const& key, std::optional<key_type> const& last)
        const override
    {
        auto const iter = state_.upper_bound(key);
        if (iter == state_.end() || (last && iter->first >= *last))
            return std::nullopt;
        return iter->first;
    }

    std::shared_ptr<SLE const>
    read(Keylet const& k) const override
    {
        auto const iter = state_.find(k.key);
        if (iter == state_.end() || !k.check(*iter->second))
            return nullptr;

        // A copy, as a ledger hands out.
        return std::make_shared<SLE const>(*iter->second);
    }

    std::unique_ptr<sles_type::iter_base>
    slesBegin() const override
    {
        Throw<std::logic_error>("MemoryState: not iterable");
    }

    std::unique_ptr<sles_type::iter_base>
    slesEnd() const override
    {
        Throw<std::logic_error>("MemoryState: not iterable");
    }

    std::unique_ptr<sles_type::iter_base>
    slesUpperBound(key_type const&) const override
    {
        Throw<std::logic_error>("MemoryState: not iterable");
    }

    std::unique_ptr<txs_type::iter_base>
    txsBegin() const override
    {
        Throw<std::logic_error>("MemoryState: no transactions");
    }

    std::unique_ptr<txs_type::iter_base>
    txsEnd() const override
    {
        Throw<std::logic_error>("MemoryState: no transactions");
    }

    bool
    txExists(key_type const&) const override
    {
        return false;
    }

    tx_type
    txRead(key_type const&) const override
    {
        return {};
    }

    void
    rawErase(std::shared_ptr<SLE> const& sle) override
    {
        state_.erase(sle->key());
    }

    void
    rawInsert(std::shared_ptr<SLE> const& sle) override
    {
        state_[sle->key()] = std::make_shared<SLE>(*sle);
    }

    void
    rawReplace(std::shared_ptr<SLE> const& sle) override
    {
        state_[sle->key()] = std::make_shared<SLE>(*sle);
    }

    void
    rawDestroyXRP(XRPAmount const&) override
    {
    }
};

/** A read-only view over a state SHAMap, which may be incomplete.

    Reading a key whose path is not held throws SHAMapMissingNode, as a Ledger
    would, so a test reading through it proves the nodes it needs are there.
*/
class MapState : public MemoryState
{
    SHAMap const& map_;

public:
    explicit MapState(SHAMap const& map) : map_(map)
    {
    }

    std::shared_ptr<SLE const>
    read(Keylet const& k) const override
    {
        auto const& item = map_.peekItem(k.key);
        if (!item)
            return nullptr;

        auto sle =
            std::make_shared<SLE const>(SerialIter{item->slice()}, item->key());
        if (!k.check(*sle))
            return nullptr;
        return sle;
    }
};

/** Publish a manifest the way SetManifest does. */
TER
publish(MemoryState& state, Validator const& v, std::uint32_t seq)
{
    auto const id = v.account();
    if (!state.exists(keylet::account(id)))
    {
        auto const account = std::make_shared<SLE>(keylet::account(id));
        account->setAccountID(sfAccount, id);
        state.rawInsert(account);
    }

    auto const st = signManifest(v, seq);
    auto const parsed = deserializeManifest(st);
    if (!parsed)
        return temMALFORMED;

    Sandbox sb(&state, tapNONE);
    auto const ter = writeManifestObjects(
        sb,
        sb.peek(keylet::account(id)),
        st,
        *parsed,
        beast::Journal{beast::Journal::getNullSink()});

    if (isTesSuccess(ter))
        sb.apply(state);

    return ter;
}

/** Every entry in the manifest directory, with the page it is on. */
std::vector<std::pair<std::uint64_t, uint256>>
entries(ReadView const& view)
{
    std::vector<std::pair<std::uint64_t, uint256>> out;

    auto const& root = keylet::manifestDir();
    std::uint64_t index = 0;
    for (auto page = view.read(root); page;)
    {
        for (auto const& key : page->getFieldV256(sfIndexes))
            out.emplace_back(index, key);

        index = page->getFieldU64(sfIndexNext);
        if (index == 0)
            break;
        page = view.read(keylet::page(root, index));
    }

    return out;
}

/** The latest sequence on-ledger for each master key, read via the walk. */
std::map<PublicKey, std::uint32_t>
walked(ReadView const& view)
{
    std::map<PublicKey, std::uint32_t> out;
    forEachLedgerManifest(view, [&](std::shared_ptr<SLE const> const& sle) {
        PublicKey const master{makeSlice(sle->getFieldVL(sfPublicKey))};
        out.emplace(master, sle->getFieldU32(sfSequence));
    });
    return out;
}

uint256
fillerKey(std::uint64_t i)
{
    return sha512Half(std::string("filler"), i);
}

/** The state as a SHAMap, padded with unrelated items. */
std::unique_ptr<SHAMap>
toMap(MemoryState const& state, Family& family, std::uint64_t filler)
{
    auto map = std::make_unique<SHAMap>(SHAMapType::FREE, family);

    for (auto const& [key, sle] : state.state())
    {
        Serializer s;
        sle->add(s);
        map->addItem(
            SHAMapNodeType::tnACCOUNT_STATE, make_shamapitem(key, s.slice()));
    }

    for (std::uint64_t i = 0; i < filler; ++i)
    {
        Serializer s;
        s.add64(i);
        s.add64(~i);
        map->addItem(
            SHAMapNodeType::tnACCOUNT_STATE,
            make_shamapitem(fillerKey(i), s.slice()));
    }

    map->getHash();
    return map;
}

}  // namespace

class ManifestDirectory_test : public beast::unit_test::suite
{
    /** A destination map holding only the source's root node. */
    std::unique_ptr<SHAMap>
    rootOnly(SHAMap const& source, Family& family)
    {
        auto map = std::make_unique<SHAMap>(SHAMapType::FREE, family);
        map->setSynching();

        std::vector<std::pair<SHAMapNodeID, Blob>> root;
        BEAST_EXPECT(source.getNodeFat(SHAMapNodeID{}, root, false, 0));
        BEAST_EXPECT(
            map->addRootNode(
                   source.getHash(), makeSlice(root[0].second), nullptr)
                .isGood());
        return map;
    }

    struct SyncStats
    {
        std::size_t rounds = 0;
        std::size_t nodes = 0;
    };

    /** Fetch what missingManifestNodes() asks for, as a peer would serve it,
        until it asks for nothing more.
    */
    SyncStats
    syncManifests(
        SHAMap const& source,
        SHAMap& destination,
        std::vector<uint256> const& keys = {},
        std::uint64_t maxPages = dirNodeMaxPages)
    {
        SyncStats stats;

        for (;;)
        {
            auto const missing =
                missingManifestNodes(destination, 256, keys, maxPages);
            if (missing.empty())
                return stats;

            if (!BEAST_EXPECT(++stats.rounds < 64))
                return stats;

            for (auto const& [id, hash] : missing)
            {
                std::vector<std::pair<SHAMapNodeID, Blob>> data;
                BEAST_EXPECT(source.getNodeFat(id, data, false, 1));
                BEAST_EXPECT(!data.empty() && data.front().first == id);

                for (auto const& [nodeID, blob] : data)
                {
                    auto const added = destination.addKnownNode(
                        nodeID, makeSlice(blob), nullptr);
                    BEAST_EXPECT(!added.isInvalid());
                    stats.nodes += added.getGood();
                }
            }
        }
    }

    static std::size_t
    nodeCount(SHAMap const& map)
    {
        std::size_t n = 0;
        map.visitNodes([&n](SHAMapTreeNode&) {
            ++n;
            return true;
        });
        return n;
    }

    void
    checkObjects(
        MemoryState const& state,
        Validator const& v,
        std::uint32_t seq)
    {
        auto const& root = keylet::manifestDir();

        auto const obj1 = state.read(keylet::manifest(v.master));
        if (!BEAST_EXPECT(obj1))
            return;

        BEAST_EXPECT(obj1->getFieldU32(sfSequence) == seq);

        // Listed exactly once, on the page it records.
        if (!BEAST_EXPECT(obj1->isFieldPresent(sfOwnerNode)))
            return;
        auto const page =
            state.read(keylet::page(root, obj1->getFieldU64(sfOwnerNode)));
        if (BEAST_EXPECT(page))
        {
            auto const& keys = page->getFieldV256(sfIndexes);
            BEAST_EXPECT(
                std::count(keys.begin(), keys.end(), obj1->key()) == 1);
        }
        auto const all = entries(state);
        BEAST_EXPECT(std::count_if(all.begin(), all.end(), [&](auto const& e) {
                         return e.second == obj1->key();
                     }) == 1);

        auto const account = state.read(keylet::account(v.account()));
        BEAST_EXPECT(
            account && account->getFieldH256(sfManifestID) == obj1->key());

        if (seq == revocation)
        {
            BEAST_EXPECT(!obj1->isFieldPresent(sfManifestID));
            BEAST_EXPECT(!obj1->isFieldPresent(sfSigningPubKey));
            return;
        }

        // The ephemeral copy points back, and is not listed.
        auto const obj2 = state.read(keylet::manifest(v.signing));
        if (!BEAST_EXPECT(obj2))
            return;
        BEAST_EXPECT(!obj2->isFieldPresent(sfOwnerNode));
        BEAST_EXPECT(obj2->getFieldH256(sfManifestID) == obj1->key());
        BEAST_EXPECT(obj1->getFieldH256(sfManifestID) == obj2->key());
        BEAST_EXPECT(obj2->getFieldU32(sfSequence) == seq);
    }

    void
    testWrite()
    {
        testcase("directory maintenance");

        auto const& root = keylet::manifestDir();
        MemoryState state;

        BEAST_EXPECT(!state.exists(root));

        std::vector<Validator> vs;
        std::vector<std::uint32_t> seqs;
        for (int i = 0; i < 40; ++i)
        {
            vs.push_back(makeValidator());
            seqs.push_back(1);
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
        }

        // 32 to a page.
        BEAST_EXPECT(entries(state).size() == 40);
        BEAST_EXPECT(state.read(root)->getFieldU64(sfIndexPrevious) == 1);
        for (std::size_t i = 0; i < vs.size(); ++i)
            checkObjects(state, vs[i], seqs[i]);

        std::map<PublicKey, std::uint64_t> pages;
        for (auto const& v : vs)
            pages[v.master] = state.read(keylet::manifest(v.master))
                                  ->getFieldU64(sfOwnerNode);

        // Replaced with a new ephemeral key: the entry stays where it was, and
        // the old ephemeral object goes.
        for (int i = 0; i < 10; ++i)
        {
            auto const old = vs[i];
            vs[i] = rotate(vs[i]);
            seqs[i] = 2;
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], seqs[i])));
            BEAST_EXPECT(!state.exists(keylet::manifest(old.signing)));
        }

        // Again, skipping sequences.
        for (int i = 0; i < 5; ++i)
        {
            vs[i] = rotate(vs[i]);
            seqs[i] = 7;
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], seqs[i])));
        }

        // Revoked: still listed, which is how a node reading the directory
        // finds the revocation. The ephemeral object goes.
        for (int i = 10; i < 15; ++i)
        {
            seqs[i] = revocation;
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], seqs[i])));
            BEAST_EXPECT(!state.exists(keylet::manifest(vs[i].signing)));
        }

        // None of which grew the directory or moved an entry.
        BEAST_EXPECT(entries(state).size() == 40);
        BEAST_EXPECT(state.read(root)->getFieldU64(sfIndexPrevious) == 1);
        for (std::size_t i = 0; i < vs.size(); ++i)
        {
            checkObjects(state, vs[i], seqs[i]);
            BEAST_EXPECT(
                state.read(keylet::manifest(vs[i].master))
                    ->getFieldU64(sfOwnerNode) == pages[vs[i].master]);
        }

        // New master keys go on the end.
        for (int i = 0; i < 30; ++i)
        {
            vs.push_back(makeValidator());
            seqs.push_back(1);
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
        }
        BEAST_EXPECT(entries(state).size() == 70);
        BEAST_EXPECT(state.read(root)->getFieldU64(sfIndexPrevious) == 2);
        for (std::size_t i = 0; i < vs.size(); ++i)
            checkObjects(state, vs[i], seqs[i]);

        // An account pointing anywhere but at its own master object is a
        // corrupt ledger: nothing is written, and the directory is untouched.
        {
            auto const original = state.read(keylet::account(vs[20].account()));
            auto bad = std::make_shared<SLE>(*original);
            bad->setFieldH256(
                sfManifestID, keylet::manifest(vs[21].master).key);
            state.rawReplace(bad);

            BEAST_EXPECT(
                publish(state, rotate(vs[20]), 3) == TER{tefBAD_LEDGER});
            BEAST_EXPECT(entries(state).size() == 70);

            state.rawReplace(std::make_shared<SLE>(*original));
            checkObjects(state, vs[20], seqs[20]);
        }

        // A master object with no page, as one written before the directory
        // existed would be, is listed by its next manifest, once.
        {
            auto obj1 = std::make_shared<SLE>(
                *state.read(keylet::manifest(vs[30].master)));
            auto const pageNo = obj1->getFieldU64(sfOwnerNode);
            obj1->makeFieldAbsent(sfOwnerNode);
            state.rawReplace(obj1);

            auto page =
                std::make_shared<SLE>(*state.read(keylet::page(root, pageNo)));
            auto keys = page->getFieldV256(sfIndexes);
            keys.erase(std::find(keys.begin(), keys.end(), obj1->key()));
            page->setFieldV256(sfIndexes, keys);
            state.rawReplace(page);
            BEAST_EXPECT(entries(state).size() == 69);

            vs[30] = rotate(vs[30]);
            seqs[30] = 2;
            BEAST_EXPECT(isTesSuccess(publish(state, vs[30], seqs[30])));
            BEAST_EXPECT(entries(state).size() == 70);
            checkObjects(state, vs[30], seqs[30]);
        }
    }

    void
    testWalk()
    {
        testcase("directory walk");

        MemoryState state;
        BEAST_EXPECT(walked(state).empty());

        std::map<PublicKey, std::uint32_t> expected;
        std::vector<Validator> vs;
        for (int i = 0; i < 100; ++i)
        {
            vs.push_back(makeValidator());
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
            expected[vs.back().master] = 1;
        }
        for (int i = 0; i < 20; ++i)
        {
            vs[i] = rotate(vs[i]);
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], 4)));
            expected[vs[i].master] = 4;
        }
        for (int i = 20; i < 25; ++i)
        {
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], revocation)));
            expected[vs[i].master] = revocation;
        }

        // Each master key once, at its latest, revocations included.
        BEAST_EXPECT(walked(state) == expected);

        // Listed keys that are not master objects: an account root is not
        // even visited, and the ephemeral copy of a manifest, though visited,
        // is not ingested as a master key.
        {
            Sandbox sb(&state, tapNONE);
            auto const add = [&](uint256 const& key) {
                return sb.dirInsert(
                    keylet::manifestDir(),
                    key,
                    [](std::shared_ptr<SLE> const&) {});
            };
            BEAST_EXPECT(add(keylet::account(vs[50].account()).key));
            BEAST_EXPECT(add(keylet::manifest(vs[51].signing).key));
            sb.apply(state);
        }

        std::size_t visited = 0;
        forEachLedgerManifest(
            state, [&](std::shared_ptr<SLE const> const&) { ++visited; });
        BEAST_EXPECT(visited == 101);

        TestStopwatch clock;
        ManifestCache cache{
            beast::Journal{beast::Journal::getNullSink()}, clock};
        BEAST_EXPECT(cache.applyLedgerDirectory(state) == 100);
    }

    void
    testIngest()
    {
        testcase("cache ingest");

        MemoryState state;
        std::vector<Validator> vs;
        std::vector<std::uint32_t> seqs;
        for (int i = 0; i < 60; ++i)
        {
            vs.push_back(makeValidator());
            seqs.push_back(1);
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
        }
        for (int i = 0; i < 10; ++i)
        {
            vs[i] = rotate(vs[i]);
            seqs[i] = 3;
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], 3)));
        }
        for (int i = 10; i < 15; ++i)
        {
            seqs[i] = revocation;
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], revocation)));
        }

        TestStopwatch clock;
        ManifestCache cache{
            beast::Journal{beast::Journal::getNullSink()}, clock};

        BEAST_EXPECT(cache.applyLedgerDirectory(state) == vs.size());
        for (std::size_t i = 0; i < vs.size(); ++i)
        {
            BEAST_EXPECT(cache.getTier(vs[i].master) == ManifestSource::ledger);
            if (seqs[i] == revocation)
            {
                BEAST_EXPECT(cache.revoked(vs[i].master));
                BEAST_EXPECT(
                    cache.getMasterKey(vs[i].signing) == vs[i].signing);
            }
            else
            {
                BEAST_EXPECT(cache.getSequence(vs[i].master) == seqs[i]);
                BEAST_EXPECT(cache.getMasterKey(vs[i].signing) == vs[i].master);
            }
        }

        // Nothing new: nothing accepted, and nothing rebuilt to find that out.
        BEAST_EXPECT(cache.applyLedgerDirectory(state) == 0);

        // A rotation and a revocation published since are picked up.
        vs[20] = rotate(vs[20]);
        BEAST_EXPECT(isTesSuccess(publish(state, vs[20], 2)));
        BEAST_EXPECT(isTesSuccess(publish(state, vs[21], revocation)));
        BEAST_EXPECT(cache.applyLedgerDirectory(state) == 2);
        BEAST_EXPECT(cache.getMasterKey(vs[20].signing) == vs[20].master);
        BEAST_EXPECT(cache.revoked(vs[21].master));

        // Gossip delivered first is adopted into the lookup cache.
        {
            TestStopwatch clock2;
            ManifestCache gossiped{
                beast::Journal{beast::Journal::getNullSink()}, clock2};
            gossiped.noteValidation(vs[30].signing);
            auto mo = deserializeManifest(signManifest(vs[30], 1));
            BEAST_EXPECT(
                gossiped.applyManifest(
                    std::move(*mo), ManifestSource::gossip) ==
                ManifestDisposition::accepted);
            BEAST_EXPECT(
                gossiped.getTier(vs[30].master) == ManifestSource::gossip);
            gossiped.applyLedgerDirectory(state);
            BEAST_EXPECT(
                gossiped.getTier(vs[30].master) == ManifestSource::ledger);
        }
    }

    void
    testIngestCapacity()
    {
        testcase("cache ingest capacity");

        MemoryState state;
        std::size_t const total = ManifestCache::ledgerCapacity + 88;
        for (std::size_t i = 0; i < total; ++i)
            BEAST_EXPECT(isTesSuccess(publish(state, makeValidator(), 1)));

        // The last few the walk reaches, pinned, are taken however full the
        // lookup cache is by then.
        std::vector<PublicKey> order;
        forEachLedgerManifest(
            state, [&](std::shared_ptr<SLE const> const& sle) {
                order.emplace_back(makeSlice(sle->getFieldVL(sfPublicKey)));
            });
        BEAST_EXPECT(order.size() == total);
        hash_set<PublicKey> pinned(order.end() - 5, order.end());

        TestStopwatch clock;
        ManifestCache cache{
            beast::Journal{beast::Journal::getNullSink()}, clock};
        cache.pin(pinned);

        BEAST_EXPECT(
            cache.applyLedgerDirectory(state) ==
            ManifestCache::ledgerCapacity + 5);

        std::size_t list = 0, ledger = 0, absent = 0;
        for (auto const& master : order)
        {
            auto const tier = cache.getTier(master);
            if (!tier)
                ++absent;
            else if (*tier == ManifestSource::list)
                ++list;
            else if (*tier == ManifestSource::ledger)
                ++ledger;
        }
        BEAST_EXPECT(list == 5);
        BEAST_EXPECT(ledger == ManifestCache::ledgerCapacity);
        BEAST_EXPECT(absent == total - ManifestCache::ledgerCapacity - 5);
        for (auto const& master : pinned)
            BEAST_EXPECT(cache.getTier(master) == ManifestSource::list);

        // A second pass fills nothing and churns nothing.
        BEAST_EXPECT(cache.applyLedgerDirectory(state) == 0);
    }

    void
    testPartialLookup()
    {
        testcase("SHAMap partial lookup");

        SuiteJournal journal("ManifestDirectory_test", *this);
        tests::TestNodeFamily sf{journal};
        tests::TestNodeFamily df{journal};

        MemoryState empty;
        auto const source = toMap(empty, sf, 2000);
        auto const destination = rootOnly(*source, df);

        // Present: each step names the next node down the key's path, until
        // the item is reached.
        auto const key = fillerKey(1234);
        std::optional<std::pair<SHAMapNodeID, uint256>> missing;
        SHAMapNodeID expected;
        int steps = 0;
        for (;;)
        {
            auto const item = destination->peekItemPartial(key, missing);
            if (item)
            {
                BEAST_EXPECT(!missing);
                BEAST_EXPECT(item->key() == key);
                BEAST_EXPECT(
                    item == source->peekItem(key) ||
                    item->slice() == source->peekItem(key)->slice());
                break;
            }

            if (!BEAST_EXPECT(missing) || !BEAST_EXPECT(++steps < 64))
                break;

            expected = expected.getChildNodeID(selectBranch(expected, key));
            BEAST_EXPECT(missing->first == expected);

            std::vector<std::pair<SHAMapNodeID, Blob>> data;
            BEAST_EXPECT(source->getNodeFat(missing->first, data, false, 0));
            BEAST_EXPECT(data.size() == 1);
            BEAST_EXPECT(
                destination
                    ->addKnownNode(
                        data[0].first, makeSlice(data[0].second), nullptr)
                    .isGood());
        }
        BEAST_EXPECT(steps >= 2);

        // Absent: resolves to nothing once the path to where it would be is
        // held, without reporting a node.
        auto const absent = fillerKey(999999);
        for (int i = 0; i < 64; ++i)
        {
            auto const item = destination->peekItemPartial(absent, missing);
            BEAST_EXPECT(!item);
            if (!missing)
                break;

            std::vector<std::pair<SHAMapNodeID, Blob>> data;
            BEAST_EXPECT(source->getNodeFat(missing->first, data, false, 0));
            destination->addKnownNode(
                data[0].first, makeSlice(data[0].second), nullptr);
        }
        BEAST_EXPECT(!missing);

        // A complete map reports nothing missing, ever.
        for (std::uint64_t i = 0; i < 2000; i += 97)
        {
            BEAST_EXPECT(source->peekItemPartial(fillerKey(i), missing));
            BEAST_EXPECT(!missing);
        }
    }

    void
    testPartialSync()
    {
        testcase("partial sync");

        SuiteJournal journal("ManifestDirectory_test", *this);

        MemoryState state;
        std::vector<Validator> vs;
        std::map<PublicKey, std::uint32_t> expected;
        for (int i = 0; i < 150; ++i)
        {
            vs.push_back(makeValidator());
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
            expected[vs.back().master] = 1;
        }
        for (int i = 0; i < 30; ++i)
        {
            vs[i] = rotate(vs[i]);
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], 2)));
            expected[vs[i].master] = 2;
        }
        for (int i = 30; i < 40; ++i)
        {
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], revocation)));
            expected[vs[i].master] = revocation;
        }

        // Five pages, so the speculative page fetch is exercised.
        BEAST_EXPECT(
            state.read(keylet::manifestDir())->getFieldU64(sfIndexPrevious) ==
            4);

        std::uint64_t const filler = 20000;
        tests::TestNodeFamily sf{journal};
        tests::TestNodeFamily df{journal};
        auto const source = toMap(state, sf, filler);
        auto const destination = rootOnly(*source, df);

        auto const stats = syncManifests(*source, *destination);
        auto const total = nodeCount(*source);
        log << "partial sync: " << stats.rounds << " rounds, " << stats.nodes
            << " of " << total << " nodes" << std::endl;

        // Converged in a handful of rounds, on a small part of the map.
        BEAST_EXPECT(stats.rounds <= 16);
        BEAST_EXPECT(stats.nodes * 4 < total);
        BEAST_EXPECT(missingManifestNodes(*destination, 256).empty());

        // Most of the map is not held.
        std::size_t unreadable = 0;
        std::optional<std::pair<SHAMapNodeID, uint256>> missing;
        for (std::uint64_t i = 0; i < filler; i += 7)
        {
            destination->peekItemPartial(fillerKey(i), missing);
            unreadable += missing ? 1 : 0;
        }
        BEAST_EXPECT(unreadable * 4 > (filler / 7) * 3);

        // But every manifest read succeeds, at its latest, through the same
        // functions a node uses on a full ledger.
        MapState view{*destination};
        BEAST_EXPECT(walked(view) == expected);
        for (auto const& v : vs)
            if (expected[v.master] != revocation)
                BEAST_EXPECT(view.read(keylet::manifest(v.signing)));

        TestStopwatch clock;
        ManifestCache cache{
            beast::Journal{beast::Journal::getNullSink()}, clock};
        BEAST_EXPECT(cache.applyLedgerDirectory(view) == vs.size());
        for (auto const& v : vs)
        {
            if (expected[v.master] == revocation)
                BEAST_EXPECT(cache.revoked(v.master));
            else
                BEAST_EXPECT(cache.getMasterKey(v.signing) == v.master);
        }

        // And the ledger tier's miss path works on it too.
        TestStopwatch clock2;
        ManifestCache cold{
            beast::Journal{beast::Journal::getNullSink()}, clock2};
        BEAST_EXPECT(
            cold.applyLedgerSigningKey(view, vs[100].signing) ==
            vs[100].master);

        // Nodes already held locally are not asked for again: a later ledger
        // sharing them, here the same one, needs nothing fetched.
        auto const again = rootOnly(*source, df);
        BEAST_EXPECT(missingManifestNodes(*again, 256).empty());

        // The report is bounded.
        tests::TestNodeFamily ef{journal};
        auto const fresh = rootOnly(*source, ef);
        BEAST_EXPECT(missingManifestNodes(*fresh, 1).size() == 1);
    }

    void
    testPartialSyncNoDirectory()
    {
        testcase("partial sync without a directory");

        SuiteJournal journal("ManifestDirectory_test", *this);
        tests::TestNodeFamily sf{journal};
        tests::TestNodeFamily df{journal};

        MemoryState empty;
        auto const source = toMap(empty, sf, 5000);
        auto const destination = rootOnly(*source, df);

        auto const stats = syncManifests(*source, *destination);
        BEAST_EXPECT(stats.rounds <= 4);
        BEAST_EXPECT(stats.nodes * 10 < nodeCount(*source));

        MapState view{*destination};
        BEAST_EXPECT(walked(view).empty());

        ManifestCache cache;
        BEAST_EXPECT(cache.applyLedgerDirectory(view) == 0);
    }

    void
    testPartialSyncByKey()
    {
        testcase("partial sync by key");

        SuiteJournal journal("ManifestDirectory_test", *this);

        // Manifests on-ledger with no directory listing them: the directory
        // pages are dropped. A node that knows its validators' master keys
        // needs no directory to find their manifests.
        MemoryState state;
        std::vector<Validator> vs;
        for (int i = 0; i < 40; ++i)
        {
            vs.push_back(makeValidator());
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
        }
        for (int i = 0; i < 5; ++i)
        {
            vs[i] = rotate(vs[i]);
            BEAST_EXPECT(isTesSuccess(publish(state, vs[i], 2)));
        }
        BEAST_EXPECT(isTesSuccess(publish(state, vs[5], revocation)));

        for (std::uint64_t i = 0; i < 4; ++i)
            if (auto const page =
                    state.read(keylet::page(keylet::manifestDir(), i)))
                state.rawErase(std::make_shared<SLE>(*page));
        BEAST_EXPECT(walked(state).empty());

        // Only the first ten are on this node's lists.
        std::vector<uint256> keys;
        hash_set<PublicKey> listed;
        for (int i = 0; i < 10; ++i)
        {
            keys.push_back(keylet::manifest(vs[i].master).key);
            listed.insert(vs[i].master);
        }
        // And one more key that has no manifest at all.
        auto const absent = makeValidator();
        keys.push_back(keylet::manifest(absent.master).key);
        listed.insert(absent.master);

        tests::TestNodeFamily sf{journal};
        tests::TestNodeFamily df{journal};
        auto const source = toMap(state, sf, 20000);
        auto const destination = rootOnly(*source, df);

        auto const stats = syncManifests(*source, *destination, keys);
        log << "partial sync by key: " << stats.rounds << " rounds, "
            << stats.nodes << " of " << nodeCount(*source) << " nodes"
            << std::endl;
        BEAST_EXPECT(stats.rounds <= 16);
        BEAST_EXPECT(stats.nodes * 10 < nodeCount(*source));
        BEAST_EXPECT(missingManifestNodes(*destination, 256, keys).empty());

        // Read the way InboundLedger reads them, through the cache.
        MapState view{*destination};
        TestStopwatch clock;
        ManifestCache cache{
            beast::Journal{beast::Journal::getNullSink()}, clock};
        BEAST_EXPECT(cache.applyLedger(view, listed) == 10);
        for (int i = 0; i < 10; ++i)
        {
            if (i == 5)
                BEAST_EXPECT(cache.revoked(vs[i].master));
            else
                BEAST_EXPECT(cache.getMasterKey(vs[i].signing) == vs[i].master);
        }
        BEAST_EXPECT(!cache.getTier(absent.master));

        // Their ephemeral copies were fetched as well, so the miss path works.
        TestStopwatch clock2;
        ManifestCache cold{
            beast::Journal{beast::Journal::getNullSink()}, clock2};
        BEAST_EXPECT(
            cold.applyLedgerSigningKey(view, vs[7].signing) == vs[7].master);

        // Keys not asked for were not fetched.
        bool threw = false;
        try
        {
            view.read(keylet::manifest(vs[30].master));
        }
        catch (SHAMapMissingNode const&)
        {
            threw = true;
        }
        BEAST_EXPECT(threw);
    }

    void
    testPartialSyncPageCap()
    {
        testcase("partial sync page cap");

        SuiteJournal journal("ManifestDirectory_test", *this);

        MemoryState state;
        std::vector<Validator> vs;
        for (int i = 0; i < 150; ++i)
        {
            vs.push_back(makeValidator());
            BEAST_EXPECT(isTesSuccess(publish(state, vs.back(), 1)));
        }
        BEAST_EXPECT(
            state.read(keylet::manifestDir())->getFieldU64(sfIndexPrevious) ==
            4);

        // The order the directory lists them in, page by page.
        std::vector<PublicKey> order;
        forEachLedgerManifest(
            state, [&](std::shared_ptr<SLE const> const& sle) {
                order.emplace_back(makeSlice(sle->getFieldVL(sfPublicKey)));
            });
        BEAST_EXPECT(order.size() == vs.size());

        std::uint64_t const pages = 2;
        std::size_t const capped = pages * dirNodeMaxEntries;

        // Capped walks see the first pages only.
        std::size_t seen = 0;
        forEachLedgerManifest(
            state, [&](std::shared_ptr<SLE const> const&) { ++seen; }, pages);
        BEAST_EXPECT(seen == capped);
        seen = 0;
        forEachLedgerManifest(
            state, [&](std::shared_ptr<SLE const> const&) { ++seen; }, 1);
        BEAST_EXPECT(seen == dirNodeMaxEntries);

        // One key listed far past the cap is still fetched, by key.
        auto const& late = order.back();
        std::vector<uint256> keys{keylet::manifest(late).key};

        tests::TestNodeFamily sf{journal};
        tests::TestNodeFamily df{journal};
        auto const source = toMap(state, sf, 20000);
        auto const destination = rootOnly(*source, df);

        auto const full = [&] {
            tests::TestNodeFamily ff{journal};
            auto const map = rootOnly(*source, ff);
            return syncManifests(*source, *map);
        }();
        auto const stats = syncManifests(*source, *destination, keys, pages);
        BEAST_EXPECT(stats.nodes < full.nodes);
        BEAST_EXPECT(
            missingManifestNodes(*destination, 256, keys, pages).empty());

        MapState view{*destination};
        TestStopwatch clock;
        ManifestCache cache{
            beast::Journal{beast::Journal::getNullSink()}, clock};
        BEAST_EXPECT(cache.applyLedger(view, {late}) == 1);
        BEAST_EXPECT(cache.applyLedgerDirectory(view, pages) == capped);
        for (std::size_t i = 0; i < capped; ++i)
            BEAST_EXPECT(cache.getTier(order[i]) == ManifestSource::ledger);
        BEAST_EXPECT(!cache.getTier(order[capped]));

        // Uncapped, the walk needs pages this map does not hold.
        bool threw = false;
        try
        {
            forEachLedgerManifest(
                view, [](std::shared_ptr<SLE const> const&) {});
        }
        catch (SHAMapMissingNode const&)
        {
            threw = true;
        }
        BEAST_EXPECT(threw);
    }

    void
    testCandidates()
    {
        testcase("candidate ledgers");

        auto const h = [](std::uint64_t i) {
            return sha512Half(std::string("ledger"), i);
        };

        // Most reported first; zero ignored.
        std::vector<uint256> reported{
            h(1), h(2), h(2), h(3), h(3), h(3), uint256{}, uint256{}};
        hash_set<uint256> skip;
        auto picked = pickManifestCandidates(reported, skip, 3);
        if (BEAST_EXPECT(picked.size() == 3))
        {
            BEAST_EXPECT(picked[0] == h(3));
            BEAST_EXPECT(picked[1] == h(2));
            BEAST_EXPECT(picked[2] == h(1));
        }

        // Bounded.
        picked = pickManifestCandidates(reported, skip, 1);
        BEAST_EXPECT(picked.size() == 1 && picked[0] == h(3));
        BEAST_EXPECT(pickManifestCandidates(reported, skip, 0).empty());

        // Skipped ones make way for the rest.
        skip.insert(h(3));
        picked = pickManifestCandidates(reported, skip, 3);
        BEAST_EXPECT(picked.size() == 2 && picked[0] == h(2));

        // Ties go to the larger hash, as NetworkOPs picks by peer count.
        auto const a = h(10), b = h(11);
        picked = pickManifestCandidates({a, b}, {}, 2);
        if (BEAST_EXPECT(picked.size() == 2))
            BEAST_EXPECT(picked[0] == std::max(a, b));

        // Nothing reported, nothing picked.
        BEAST_EXPECT(pickManifestCandidates({}, {}, 3).empty());
        BEAST_EXPECT(pickManifestCandidates({uint256{}}, {}, 3).empty());
    }

    void
    testQuorum()
    {
        testcase("manifest quorum");

        // Before the trusted set exists: 80% of those listed, rounded up.
        BEAST_EXPECT(manifestQuorum(0, false, 1) == 0);
        BEAST_EXPECT(manifestQuorum(1, false, 1) == 1);
        BEAST_EXPECT(manifestQuorum(4, false, 1) == 4);
        BEAST_EXPECT(manifestQuorum(5, false, 1) == 4);
        BEAST_EXPECT(manifestQuorum(10, false, 1) == 8);
        BEAST_EXPECT(manifestQuorum(11, false, 1) == 9);
        BEAST_EXPECT(manifestQuorum(35, false, 1) == 28);

        // After: the quorum ValidatorList worked out, which a negative UNL
        // can lower, never more than there are validators.
        BEAST_EXPECT(manifestQuorum(35, true, 28) == 28);
        BEAST_EXPECT(manifestQuorum(35, true, 21) == 21);
        BEAST_EXPECT(manifestQuorum(3, true, 5) == 3);
        BEAST_EXPECT(
            manifestQuorum(3, true, std::numeric_limits<std::size_t>::max()) ==
            3);
    }

public:
    void
    run() override
    {
        testWrite();
        testWalk();
        testIngest();
        testIngestCapacity();
        testPartialLookup();
        testPartialSync();
        testPartialSyncNoDirectory();
        testPartialSyncByKey();
        testPartialSyncPageCap();
        testCandidates();
        testQuorum();
    }
};

BEAST_DEFINE_TESTSUITE(ManifestDirectory, app, ripple);

}  // namespace test
}  // namespace ripple
