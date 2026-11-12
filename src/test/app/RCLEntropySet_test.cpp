//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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
#include <xrpld/app/consensus/RCLEntropySet.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace ripple {
namespace test {

/** The helpers behind TMEntropySet (featureRNG): choosing the ttENTROPY
    transactions of a transaction set, and building a validator's transaction
    set from them and the rest of our own.
*/
class RCLEntropySet_test : public beast::unit_test::suite
{
    static Blob
    serialize(STTx const& tx)
    {
        Serializer s;
        tx.add(s);
        return s.getData();
    }

    // What RCLConsensus::Adaptor::makeEntropyTx builds for validator n.
    static Blob
    entropyTx(std::uint32_t seq, std::uint32_t n, bool reveal = true)
    {
        auto const pk =
            generateKeyPair(
                KeyType::ed25519, generateSeed("validator" + std::to_string(n)))
                .first;
        return serialize(STTx(ttENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setFieldVL(sfPublicKey, pk.slice());
            obj.setFieldH256(sfNextRandomDigest, sha512Half(n, seq));
            if (reveal)
                obj.setFieldH256(sfRandomData, sha512Half(seq, n));
        }));
    }

    // Any other transaction a position may hold.
    static Blob
    otherTx(std::uint32_t seq, std::uint32_t n)
    {
        return serialize(STTx(ttAMENDMENT, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID());
            obj.setFieldH256(sfAmendment, sha512Half(n));
            obj.setFieldU32(sfLedgerSequence, seq);
        }));
    }

    static boost::intrusive_ptr<SHAMapItem const>
    item(Blob const& tx)
    {
        return make_shamapitem(entropy_set::txID(makeSlice(tx)), makeSlice(tx));
    }

    // A transaction set, built the way RCLConsensus::Adaptor::onClose does.
    static std::shared_ptr<SHAMap>
    makeSet(Family& f, std::vector<Blob> const& txs)
    {
        auto set = std::make_shared<SHAMap>(SHAMapType::TRANSACTION, f);
        set->setUnbacked();
        for (auto const& tx : txs)
            set->addItem(SHAMapNodeType::tnTRANSACTION_NM, item(tx));
        return set->snapShot(false);
    }

    static uint256
    hashOf(std::shared_ptr<SHAMap> const& set)
    {
        return set->getHash().as_uint256();
    }

    void
    testTransactions()
    {
        testcase("Transactions");
        using namespace entropy_set;

        Blob const tx = entropyTx(7, 1);

        // looksLikeEntropyTx hard-codes how a serialized ttENTROPY starts.
        {
            Serializer s;
            s.addFieldID(
                sfTransactionType.fieldType, sfTransactionType.fieldValue);
            s.add16(ttENTROPY);
            BEAST_EXPECT(s.size() == 3);
            BEAST_EXPECT(
                tx.size() > 3 && std::memcmp(tx.data(), s.data(), 3) == 0);
        }

        for (bool const reveal : {true, false})
        {
            Blob const e = entropyTx(7, 2, reveal);
            BEAST_EXPECT(e.size() <= maxTxnBytes);
            BEAST_EXPECT(looksLikeEntropyTx(makeSlice(e)));

            auto const made = makeItem(makeSlice(e));
            if (BEAST_EXPECT(made))
            {
                SerialIter sit(makeSlice(e));
                BEAST_EXPECT(made->key() == STTx(sit).getTransactionID());
                BEAST_EXPECT(made->key() == txID(makeSlice(e)));
                BEAST_EXPECT(made->slice() == makeSlice(e));
                BEAST_EXPECT(isEntropyItem(*made));
                BEAST_EXPECT(ledgerSeq(*made) == 7);
            }
        }

        // another transaction type
        {
            Blob const other = otherTx(7, 1);
            BEAST_EXPECT(!looksLikeEntropyTx(makeSlice(other)));
            BEAST_EXPECT(!makeItem(makeSlice(other)));
            BEAST_EXPECT(!isEntropyItem(*item(other)));
        }

        // too short to tell
        BEAST_EXPECT(!looksLikeEntropyTx(Slice(tx.data(), 2)));
        BEAST_EXPECT(!makeItem(Slice{}));

        // starts like one, but does not deserialize
        {
            Blob truncated(tx.begin(), tx.end() - 1);
            BEAST_EXPECT(looksLikeEntropyTx(makeSlice(truncated)));
            BEAST_EXPECT(!makeItem(makeSlice(truncated)));
            BEAST_EXPECT(!isEntropyItem(*item(truncated)));
        }

        // bytes after the transaction
        {
            Blob padded = tx;
            padded.push_back(0);
            BEAST_EXPECT(!makeItem(makeSlice(padded)));
        }

        // larger than an entropy set may carry
        {
            Blob large = tx;
            large.resize(maxTxnBytes + 1);
            BEAST_EXPECT(!makeItem(makeSlice(large)));
        }

        // an item filed under some other key is not one makeItem made
        BEAST_EXPECT(
            !isEntropyItem(*make_shamapitem(uint256{1}, makeSlice(tx))));
    }

    void
    testSets()
    {
        testcase("Transaction sets");
        using namespace entropy_set;

        test::SuiteJournal journal("RCLEntropySet_test", *this);
        tests::TestNodeFamily f(journal);

        std::uint32_t const seq = 7;
        std::vector<Blob> const others{
            otherTx(seq, 1), otherTx(seq, 2), otherTx(seq, 3)};
        Blob const e1 = entropyTx(seq, 1);
        Blob const e2 = entropyTx(seq, 2);
        Blob const e3 = entropyTx(seq, 3, false);

        auto const with = [&](std::vector<Blob> txs,
                              std::vector<Blob> const& entropy) {
            txs.insert(txs.end(), entropy.begin(), entropy.end());
            return makeSet(f, txs);
        };

        // A validator's initial position holds its own contribution, a
        // merged position everybody's, an observer's none.
        auto const observer = makeSet(f, others);
        auto const ours = with(others, {e1});
        auto const theirs = with(others, {e2});
        auto const merged = with(others, {e1, e2, e3});

        // entropyTxns: only the ttENTROPY transactions, ordered by ID
        {
            BEAST_EXPECT(entropyTxns(*observer).empty());

            auto const txns = entropyTxns(*merged);
            if (BEAST_EXPECT(txns.size() == 3))
            {
                BEAST_EXPECT(txns[0]->key() < txns[1]->key());
                BEAST_EXPECT(txns[1]->key() < txns[2]->key());
                for (Blob const& e : {e1, e2, e3})
                {
                    BEAST_EXPECT(
                        std::count_if(
                            txns.begin(), txns.end(), [&e](auto const& tx) {
                                return tx->slice() == makeSlice(e);
                            }) == 1);
                }
            }
        }

        // withoutEntropy: the same base whichever position we start from
        auto const base = withoutEntropy(*ours);
        BEAST_EXPECT(hashOf(base) == hashOf(observer));
        BEAST_EXPECT(hashOf(withoutEntropy(*observer)) == hashOf(observer));
        BEAST_EXPECT(hashOf(withoutEntropy(*merged)) == hashOf(observer));
        // and the position it came from is left alone
        BEAST_EXPECT(hashOf(ours) == hashOf(with(others, {e1})));

        // rebuild: another validator's initial position, and a merged one
        for (auto const& target : {theirs, merged, ours})
        {
            auto const set =
                rebuild(*base, entropyTxns(*target), hashOf(target));
            if (BEAST_EXPECT(set))
            {
                BEAST_EXPECT(hashOf(set) == hashOf(target));
                BEAST_EXPECT(
                    entropyTxns(*set).size() == entropyTxns(*target).size());
            }
            // the base can be used again
            BEAST_EXPECT(hashOf(base) == hashOf(observer));
        }

        // not the set that was named
        BEAST_EXPECT(!rebuild(*base, entropyTxns(*theirs), hashOf(merged)));
        BEAST_EXPECT(!rebuild(*base, entropyTxns(*merged), hashOf(theirs)));
        BEAST_EXPECT(!rebuild(*base, {}, hashOf(theirs)));

        // our other transactions differ from the validator's
        {
            auto const fewer = makeSet(f, {others[0], others[1]});
            BEAST_EXPECT(
                !rebuild(*fewer, entropyTxns(*theirs), hashOf(theirs)));

            std::vector<Blob> more = others;
            more.push_back(otherTx(seq, 4));
            BEAST_EXPECT(!rebuild(
                *makeSet(f, more), entropyTxns(*theirs), hashOf(theirs)));
        }

        // a base that still holds one of the transactions
        BEAST_EXPECT(!rebuild(*ours, entropyTxns(*merged), hashOf(merged)));

        // a quiet ledger: nothing but entropy
        {
            auto const empty = makeSet(f, {});
            auto const quiet = makeSet(f, {e1, e2});
            BEAST_EXPECT(hashOf(empty).isZero());
            BEAST_EXPECT(hashOf(withoutEntropy(*quiet)).isZero());

            auto const set =
                rebuild(*empty, entropyTxns(*quiet), hashOf(quiet));
            BEAST_EXPECT(set && hashOf(set) == hashOf(quiet));
        }

        // Something that only starts like a ttENTROPY is not one: both
        // sides leave it with the other transactions.
        {
            Blob junk(e1.begin(), e1.end() - 1);
            std::vector<Blob> othersAndJunk = others;
            othersAndJunk.push_back(junk);

            auto const target = with(othersAndJunk, {e2});
            auto const txns = entropyTxns(*target);
            BEAST_EXPECT(txns.size() == 1);

            auto const junkBase = withoutEntropy(*with(othersAndJunk, {e1}));
            BEAST_EXPECT(hashOf(junkBase) == hashOf(makeSet(f, othersAndJunk)));
            BEAST_EXPECT(rebuild(*junkBase, txns, hashOf(target)));
            BEAST_EXPECT(!rebuild(*base, txns, hashOf(target)));
        }
    }

    void
    testDigest()
    {
        testcase("Digest");
        using namespace entropy_set;

        uint256 const prev = sha512Half(1);
        uint256 const set = sha512Half(2);

        Txns const txns{
            makeItem(makeSlice(entropyTx(7, 1))),
            makeItem(makeSlice(entropyTx(7, 2)))};
        if (!BEAST_EXPECT(txns[0] && txns[1]))
            return;
        std::vector<uint256> const ids{txns[0]->key(), txns[1]->key()};

        uint256 const d = digest(prev, set, ids);
        BEAST_EXPECT(d == digest(prev, set, txns));

        // every field counts, and so does the order
        BEAST_EXPECT(d != digest(set, prev, ids));
        BEAST_EXPECT(d != digest(sha512Half(3), set, ids));
        BEAST_EXPECT(d != digest(prev, sha512Half(3), ids));
        BEAST_EXPECT(d != digest(prev, set, std::vector<uint256>{ids[0]}));
        BEAST_EXPECT(
            d != digest(prev, set, std::vector<uint256>{ids[1], ids[0]}));

        // signed and checked like a proposal
        auto const [pk, sk] = randomKeyPair(KeyType::secp256k1);
        auto const sig = signDigest(pk, sk, d);
        BEAST_EXPECT(verifyDigest(pk, d, sig, false));
        BEAST_EXPECT(!verifyDigest(pk, digest(set, prev, ids), sig, false));

        // one message, one ID: the content alone does not identify it
        auto const [pk2, sk2] = randomKeyPair(KeyType::secp256k1);
        auto const sig2 = signDigest(pk2, sk2, d);
        uint256 const id = uniqueId(d, pk.slice(), sig);
        BEAST_EXPECT(id == uniqueId(d, pk.slice(), sig));
        BEAST_EXPECT(id != uniqueId(d, pk2.slice(), sig2));
        BEAST_EXPECT(id != uniqueId(d, pk.slice(), sig2));
        BEAST_EXPECT(id != uniqueId(digest(set, prev, ids), pk.slice(), sig));
    }

public:
    void
    run() override
    {
        testTransactions();
        testSets();
        testDigest();
    }
};

BEAST_DEFINE_TESTSUITE(RCLEntropySet, app, ripple);

}  // namespace test
}  // namespace ripple
