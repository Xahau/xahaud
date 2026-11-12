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

#include <test/jtx.h>
#include <xrpld/app/consensus/RCLEntropySet.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace ripple {
namespace test {

/** featureRNG: what consensus does with the entropy sets validators push
    (TMEntropySet), from NetworkOPs::processEntropySet down.

    A standalone server never stays in the establish phase, where it has a
    position to build other sets from. These tests put it there the way a
    networked server gets there: a round is started on the last closed
    ledger, and the consensus timer closes the open ledger.
*/
class RCLConsensusEntropySet_test : public beast::unit_test::suite
{
    using Txns = entropy_set::Txns;

    // The entropy sets RCLConsensus remembers (maxPendingEntropySets).
    static constexpr std::size_t maxPending = 256;

    // What RCLConsensus::Adaptor::makeEntropyTx builds, for validator n.
    static boost::intrusive_ptr<SHAMapItem const>
    entropyTx(std::uint32_t seq, std::uint32_t n)
    {
        auto const pk =
            generateKeyPair(
                KeyType::ed25519, generateSeed("validator" + std::to_string(n)))
                .first;
        Serializer s;
        STTx(ttENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setFieldVL(sfPublicKey, pk.slice());
            obj.setFieldH256(sfNextRandomDigest, sha512Half(n, seq));
            obj.setFieldH256(sfRandomData, sha512Half(seq, n));
        }).add(s);
        return entropy_set::makeItem(s.slice());
    }

    // Any other transaction a position may hold.
    static boost::intrusive_ptr<SHAMapItem const>
    otherTx(std::uint32_t seq, std::uint32_t n)
    {
        Serializer s;
        STTx(ttAMENDMENT, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID());
            obj.setFieldH256(sfAmendment, sha512Half(n));
            obj.setFieldU32(sfLedgerSequence, seq);
        }).add(s);
        return make_shamapitem(entropy_set::txID(s.slice()), s.slice());
    }

    // The transactions of the open ledger, as RCLConsensus::Adaptor::onClose
    // puts them into our position.
    static Txns
    openTxns(jtx::Env& env)
    {
        Txns txns;
        for (auto const& tx : env.current()->txs)
        {
            Serializer s;
            tx.first->add(s);
            txns.push_back(
                make_shamapitem(tx.first->getTransactionID(), s.slice()));
        }
        return txns;
    }

    static Txns
    sorted(Txns txns)
    {
        std::sort(txns.begin(), txns.end(), [](auto const& a, auto const& b) {
            return a->key() < b->key();
        });
        return txns;
    }

    static Txns
    with(Txns txns, Txns const& more)
    {
        txns.insert(txns.end(), more.begin(), more.end());
        return txns;
    }

    // A transaction set, built the way RCLConsensus::Adaptor::onClose does.
    static std::shared_ptr<SHAMap>
    makeSet(jtx::Env& env, Txns const& txns)
    {
        auto set = std::make_shared<SHAMap>(
            SHAMapType::TRANSACTION, env.app().getNodeFamily());
        set->setUnbacked();
        for (auto const& tx : txns)
            set->addItem(SHAMapNodeType::tnTRANSACTION_NM, tx);
        return set->snapShot(false);
    }

    static uint256
    hashOf(std::shared_ptr<SHAMap> const& set)
    {
        return set->getHash().as_uint256();
    }

    // Whether consensus has the set, without asking a peer for it.
    static bool
    have(jtx::Env& env, uint256 const& setHash)
    {
        auto const set =
            env.app().getInboundTransactions().getSet(setHash, false);
        return set && hashOf(set) == setHash;
    }

    static ConsensusPhase
    phase(jtx::Env& env)
    {
        return env.app().getOPs().getConsensus().phase();
    }

    // Start the round that builds on the last closed ledger, as NetworkOPs
    // does once the previous round is over.
    void
    startRound(jtx::Env& env)
    {
        BEAST_EXPECT(
            env.app().getOPs().beginConsensus(env.closed()->info().hash, {}));
        BEAST_EXPECT(phase(env) == ConsensusPhase::open);
    }

    // The consensus timer, long enough after the previous close that the
    // open ledger is closed whatever else is going on. That is when we take
    // our position.
    void
    closeOpenLedger(jtx::Env& env)
    {
        using namespace std::chrono_literals;
        env.app().getOPs().getConsensus().timerEntry(
            env.app().timeKeeper().closeTime() + 20min);
        BEAST_EXPECT(phase(env) == ConsensusPhase::establish);
    }

    // The transaction set of our position.
    std::shared_ptr<SHAMap>
    ourSet(jtx::Env& env)
    {
        auto const info = env.app().getOPs().getConsensusInfo();
        uint256 hash;
        if (!BEAST_EXPECT(
                info.isMember("our_position") &&
                hash.parseHex(
                    info["our_position"][jss::transaction_hash].asString())))
            return {};
        auto set = env.app().getInboundTransactions().getSet(hash, false);
        BEAST_EXPECT(set);
        return set;
    }

    // A validating server with a closed ledger behind it and one transaction
    // in its open ledger.
    void
    prepare(jtx::Env& env)
    {
        using namespace jtx;
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(pay(alice, bob, XRP(1)));
        BEAST_EXPECT(openTxns(env).size() == 1);
    }

    void
    testBeforeOurPosition()
    {
        testcase("Before we have a position");
        using namespace jtx;

        Env env{*this, envconfig(validator, ""), supported_amendments()};
        prepare(env);

        auto& ops = env.app().getOPs();
        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        auto const base = openTxns(env);

        // The initial positions of two other validators: what we hold, and
        // an entropy contribution each.
        auto const e1 = entropyTx(seq, 1);
        auto const e2 = entropyTx(seq, 2);
        if (!BEAST_EXPECT(e1 && e2))
            return;
        auto const h1 = hashOf(makeSet(env, with(base, {e1})));
        auto const h2 = hashOf(makeSet(env, with(base, {e2})));

        // One validator is ahead of us: we have not started the round its
        // position is for. Its entropy set is not relayed, but it is kept.
        BEAST_EXPECT(phase(env) == ConsensusPhase::accepted);
        BEAST_EXPECT(ops.getConsensus().prevLedgerID() != prev);
        BEAST_EXPECT(!ops.processEntropySet(prev, h1, {e1}));
        BEAST_EXPECT(!have(env, h1));

        // The other one's arrives while our ledger is still open. It is for
        // our round, but there is nothing to build its set from yet.
        startRound(env);
        BEAST_EXPECT(ops.getConsensus().prevLedgerID() == prev);
        BEAST_EXPECT(ops.processEntropySet(prev, h2, {e2}));
        BEAST_EXPECT(!have(env, h1));
        BEAST_EXPECT(!have(env, h2));

        // Once we close, both sets are built from our own position.
        closeOpenLedger(env);
        BEAST_EXPECT(have(env, h1));
        BEAST_EXPECT(have(env, h2));

        // The round carries on as usual from here.
        env.close();
        BEAST_EXPECT(env.closed()->info().seq == seq);
        BEAST_EXPECT(env.closed()->info().parentHash == prev);
    }

    void
    testWithOurPosition()
    {
        testcase("With a position");
        using namespace jtx;

        Env env{*this, envconfig(validator, ""), supported_amendments()};
        prepare(env);

        auto& ops = env.app().getOPs();
        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        auto const base = openTxns(env);

        startRound(env);
        closeOpenLedger(env);

        // Our position is the open ledger and our own entropy contribution.
        auto const ours = ourSet(env);
        if (!ours)
            return;
        auto const ourEntropy = entropy_set::entropyTxns(*ours);
        if (!BEAST_EXPECT(ourEntropy.size() == 1))
            return;
        BEAST_EXPECT(
            hashOf(ours) == hashOf(makeSet(env, with(base, ourEntropy))));

        auto const e1 = entropyTx(seq, 1);
        auto const e2 = entropyTx(seq, 2);
        auto const e3 = entropyTx(seq, 3);
        auto const e4 = entropyTx(seq, 4);
        if (!BEAST_EXPECT(e1 && e2 && e3 && e4))
            return;

        // another validator's initial position
        {
            auto const h = hashOf(makeSet(env, with(base, {e1})));
            BEAST_EXPECT(!have(env, h));
            BEAST_EXPECT(ops.processEntropySet(prev, h, {e1}));
            BEAST_EXPECT(have(env, h));

            // and the same content again, from a validator that got there
            // later
            BEAST_EXPECT(ops.processEntropySet(prev, h, {e1}));
            BEAST_EXPECT(have(env, h));
        }

        // a merged position that includes our contribution
        {
            auto const txns = sorted(with(ourEntropy, {e1, e2}));
            auto const h = hashOf(makeSet(env, with(base, txns)));
            BEAST_EXPECT(!have(env, h));
            BEAST_EXPECT(ops.processEntropySet(prev, h, txns));
            BEAST_EXPECT(have(env, h));
        }

        // and one that does not
        {
            auto const txns = sorted({e1, e2});
            auto const h = hashOf(makeSet(env, with(base, txns)));
            BEAST_EXPECT(!have(env, h));
            BEAST_EXPECT(ops.processEntropySet(prev, h, txns));
            BEAST_EXPECT(have(env, h));
        }

        // our own position, which we have had all along
        BEAST_EXPECT(ops.processEntropySet(prev, hashOf(ours), ourEntropy));
        BEAST_EXPECT(have(env, hashOf(ours)));

        // A validator whose other transactions are not ours. Its set cannot
        // be built, however often we try, and is left to be acquired.
        auto const theirs = makeSet(env, with(base, {otherTx(seq, 1), e3}));
        BEAST_EXPECT(ops.processEntropySet(prev, hashOf(theirs), {e3}));
        BEAST_EXPECT(!have(env, hashOf(theirs)));

        // A set that is not the one named is not handed over under either
        // hash.
        {
            auto const built = hashOf(makeSet(env, with(base, {e4})));
            uint256 const named{sha512Half(built)};
            BEAST_EXPECT(ops.processEntropySet(prev, named, {e4}));
            BEAST_EXPECT(!have(env, named));
            BEAST_EXPECT(!have(env, built));
            BEAST_EXPECT(!have(env, hashOf(theirs)));
        }

        // An entropy set for some other round is not relayed, and is not
        // used in this one even though it would fit.
        {
            auto const h = hashOf(makeSet(env, with(base, {e2})));
            BEAST_EXPECT(!ops.processEntropySet(sha512Half(prev), h, {e2}));
            BEAST_EXPECT(!have(env, h));

            // the validator's set for this round is
            BEAST_EXPECT(ops.processEntropySet(prev, h, {e2}));
            BEAST_EXPECT(have(env, h));
        }

        // The set we could not build arrives from a peer in the end.
        env.app().getInboundTransactions().giveSet(
            hashOf(theirs), theirs, true);
        BEAST_EXPECT(have(env, hashOf(theirs)));
        BEAST_EXPECT(ops.processEntropySet(prev, hashOf(theirs), {e3}));

        // None of this got in the way of the round.
        BEAST_EXPECT(phase(env) == ConsensusPhase::establish);
        auto const still = ourSet(env);
        BEAST_EXPECT(still && hashOf(still) == hashOf(ours));
        env.close();
        BEAST_EXPECT(env.closed()->info().seq == seq);
        BEAST_EXPECT(env.closed()->info().parentHash == prev);
    }

    void
    testBounded()
    {
        testcase("Bounded");
        using namespace jtx;

        // Entropy sets we cannot use yet are remembered oldest first, up to
        // a limit: with `others` of them in between, is the first still
        // there when we take our position?
        auto const survives = [this](std::size_t others) {
            Env env{*this, envconfig(validator, ""), supported_amendments()};
            prepare(env);

            auto& ops = env.app().getOPs();
            auto const prev = env.closed()->info().hash;
            auto const seq = env.closed()->info().seq + 1;
            auto const base = openTxns(env);

            auto const e1 = entropyTx(seq, 1);
            auto const e2 = entropyTx(seq, 2);
            auto const h1 = hashOf(makeSet(env, with(base, {e1})));
            auto const h2 = hashOf(makeSet(env, with(base, {e2})));

            BEAST_EXPECT(!ops.processEntropySet(prev, h1, {e1}));

            // entropy sets of rounds we will never see
            for (std::size_t i = 0; i < others; ++i)
            {
                BEAST_EXPECT(!ops.processEntropySet(
                    sha512Half(i, prev), sha512Half(prev, i), {e1}));
            }

            BEAST_EXPECT(!ops.processEntropySet(prev, h2, {e2}));

            startRound(env);
            closeOpenLedger(env);

            // the newest one is always there
            BEAST_EXPECT(have(env, h2));
            return have(env, h1);
        };

        BEAST_EXPECT(survives(maxPending - 2));
        BEAST_EXPECT(!survives(maxPending - 1));
    }

    void
    testNotValidating()
    {
        testcase("Not validating");
        using namespace jtx;

        // A server that does not validate has no entropy contribution of its
        // own, and builds the validators' sets all the same.
        Env env{*this, supported_amendments()};
        prepare(env);

        auto& ops = env.app().getOPs();
        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        auto const base = openTxns(env);

        startRound(env);
        closeOpenLedger(env);

        auto const ours = ourSet(env);
        if (!ours)
            return;
        BEAST_EXPECT(entropy_set::entropyTxns(*ours).empty());
        BEAST_EXPECT(hashOf(ours) == hashOf(makeSet(env, base)));

        auto const txns = sorted({entropyTx(seq, 1), entropyTx(seq, 2)});
        auto const h = hashOf(makeSet(env, with(base, txns)));
        BEAST_EXPECT(ops.processEntropySet(prev, h, txns));
        BEAST_EXPECT(have(env, h));
    }

public:
    void
    run() override
    {
        testBeforeOurPosition();
        testWithOurPosition();
        testBounded();
        testNotValidating();
    }
};

BEAST_DEFINE_TESTSUITE(RCLConsensusEntropySet, app, ripple);

}  // namespace test
}  // namespace ripple
