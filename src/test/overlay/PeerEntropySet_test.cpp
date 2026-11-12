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
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/overlay/Compression.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/overlay/detail/TrafficCount.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/messages.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace ripple {
namespace test {

/** featureRNG: how a peer's TMEntropySet is checked, used and relayed
    (PeerImp::onMessage and PeerImp::checkEntropySet).
*/
class PeerEntropySet_test : public beast::unit_test::suite
{
    using socket_type = boost::asio::ip::tcp::socket;
    using middle_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<middle_type>;
    using Txns = entropy_set::Txns;
    using Packet = std::shared_ptr<protocol::TMEntropySet>;

    // A peer that records what it is sent instead of sending it.
    class PeerTest : public PeerImp
    {
    public:
        std::vector<std::shared_ptr<Message>> sent;

        PeerTest(
            Application& app,
            id_t id,
            std::shared_ptr<PeerFinder::Slot> const& slot,
            http_request_type&& request,
            PublicKey const& publicKey,
            ProtocolVersion protocol,
            Resource::Consumer consumer,
            std::unique_ptr<PeerEntropySet_test::stream_type>&& stream_ptr,
            OverlayImpl& overlay)
            : PeerImp(
                  app,
                  id,
                  slot,
                  std::move(request),
                  publicKey,
                  protocol,
                  consumer,
                  std::move(stream_ptr),
                  overlay)
        {
        }

        void
        run() override
        {
        }

        void
        send(std::shared_ptr<Message> const& m) override
        {
            sent.push_back(m);
        }
    };

    // A validator, signing with its secp256k1 ephemeral key.
    struct Validator
    {
        PublicKey pk;
        SecretKey sk;

        explicit Validator(std::string const& name)
            : Validator(generateKeyPair(KeyType::secp256k1, generateSeed(name)))
        {
        }

    private:
        explicit Validator(std::pair<PublicKey, SecretKey> const& keys)
            : pk(keys.first), sk(keys.second)
        {
        }
    };

    Validator const v1_{"entropy set validator 1"};
    Validator const v2_{"entropy set validator 2"};
    Validator const stranger_{"entropy set stranger"};

    std::shared_ptr<boost::asio::ssl::context> context_ = make_SSLContext("");

    // A validating server that trusts v1_ and v2_.
    std::unique_ptr<Config>
    config()
    {
        using namespace jtx;
        auto cfg = envconfig(validator, "");
        for (auto const& v : {v1_, v2_})
            cfg->section(SECTION_VALIDATORS)
                .append(toBase58(TokenType::NodePublic, v.pk));
        return cfg;
    }

    std::shared_ptr<PeerTest>
    makePeer(jtx::Env& env, Peer::id_t id)
    {
        auto& overlay = dynamic_cast<OverlayImpl&>(env.app().overlay());
        http_request_type request;
        auto stream_ptr = std::make_unique<stream_type>(
            socket_type(env.app().getIOContext()), *context_);
        beast::IP::Endpoint const local(
            boost::asio::ip::make_address("172.16.0.1"));
        beast::IP::Endpoint const remote(
            boost::asio::ip::make_address("172.16.1." + std::to_string(id)));
        PublicKey const key(std::get<0>(randomKeyPair(KeyType::ed25519)));
        auto consumer = overlay.resourceManager().newInboundEndpoint(remote);
        auto slot = overlay.peerFinder().new_inbound_slot(local, remote);
        auto peer = std::make_shared<PeerTest>(
            env.app(),
            id,
            slot,
            std::move(request),
            key,
            ProtocolVersion{1, 7},
            consumer,
            std::move(stream_ptr),
            overlay);
        overlay.add_active(peer);
        return peer;
    }

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

    // The hash of the transaction set that holds txns.
    static uint256
    setHash(jtx::Env& env, Txns txns, Txns const& more)
    {
        txns.insert(txns.end(), more.begin(), more.end());
        SHAMap set(SHAMapType::TRANSACTION, env.app().getNodeFamily());
        set.setUnbacked();
        for (auto const& tx : txns)
            set.addItem(SHAMapNodeType::tnTRANSACTION_NM, tx);
        return set.getHash().as_uint256();
    }

    static Txns
    sorted(Txns txns)
    {
        std::sort(txns.begin(), txns.end(), [](auto const& a, auto const& b) {
            return a->key() < b->key();
        });
        return txns;
    }

    // What RCLConsensus::Adaptor::shareEntropySet sends.
    static Packet
    makePacket(
        Validator const& v,
        uint256 const& prevLedger,
        uint256 const& set,
        Txns const& txns)
    {
        auto m = std::make_shared<protocol::TMEntropySet>();
        m->set_previousledger(prevLedger.begin(), prevLedger.size());
        m->set_sethash(set.begin(), set.size());
        for (auto const& tx : txns)
            m->add_transactions(tx->data(), tx->size());
        m->set_nodepubkey(v.pk.data(), v.pk.size());
        auto const sig =
            signDigest(v.pk, v.sk, entropy_set::digest(prevLedger, set, txns));
        m->set_signature(sig.data(), sig.size());
        return m;
    }

    // Sign whatever the packet holds now.
    static void
    sign(protocol::TMEntropySet& m, Validator const& v)
    {
        std::vector<uint256> ids;
        for (auto const& tx : m.transactions())
            ids.push_back(entropy_set::txID(makeSlice(tx)));
        auto const sig = signDigest(
            v.pk,
            v.sk,
            entropy_set::digest(
                uint256{m.previousledger()}, uint256{m.sethash()}, ids));
        m.set_signature(sig.data(), sig.size());
    }

    // The peer receives the packet, and everything that follows from that
    // has happened by the time this returns.
    static void
    receive(jtx::Env& env, PeerTest& peer, Packet const& m)
    {
        auto const size = m->ByteSizeLong();
        peer.onMessageBegin(protocol::mtENTROPY_SET, m, size, size, false);
        peer.onMessage(m);
        peer.onMessageEnd(protocol::mtENTROPY_SET, m);
        env.app().getJobQueue().rendezvous();
    }

    // Whether consensus has the set, without asking a peer for it.
    static bool
    have(jtx::Env& env, uint256 const& set)
    {
        return env.app().getInboundTransactions().getSet(set, false) != nullptr;
    }

    // The entropy sets the peer was sent since this was last asked. Anything
    // else it was sent, such as the notice that we have a new transaction
    // set, is of no interest here.
    std::vector<protocol::TMEntropySet>
    relayed(PeerTest& peer)
    {
        std::vector<protocol::TMEntropySet> sets;
        for (auto const& message : peer.sent)
        {
            if (message->getCategory() != TrafficCount::category::entropy_set)
                continue;

            auto const& buf = message->getBuffer(compression::Compressed::Off);
            protocol::TMEntropySet set;
            if (BEAST_EXPECT(
                    buf.size() > compression::headerBytes &&
                    set.ParseFromArray(
                        buf.data() + compression::headerBytes,
                        static_cast<int>(
                            buf.size() - compression::headerBytes))))
                sets.push_back(set);
        }
        peer.sent.clear();
        return sets;
    }

    // A server with a closed ledger behind it and one transaction in its
    // open ledger.
    void
    prepare(jtx::Env& env)
    {
        using namespace jtx;

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(pay(alice, bob, XRP(1)));
    }

    // Start the round after the last closed ledger and close the open
    // ledger, which is when the server takes its position and pushes it.
    void
    startRound(jtx::Env& env)
    {
        using namespace std::chrono_literals;

        auto& ops = env.app().getOPs();
        BEAST_EXPECT(ops.beginConsensus(env.closed()->info().hash, {}));
        ops.getConsensus().timerEntry(
            env.app().timeKeeper().closeTime() + 20min);
        BEAST_EXPECT(ops.getConsensus().phase() == ConsensusPhase::establish);
    }

    // prepare, and then the establish phase of the round.
    void
    establish(jtx::Env& env)
    {
        prepare(env);
        startRound(env);
    }

    void
    testRelay()
    {
        testcase("Used and relayed once");
        using namespace jtx;

        Env env{*this, config(), supported_amendments()};
        establish(env);
        BEAST_EXPECT(env.app().validators().trusted(v1_.pk));
        BEAST_EXPECT(env.app().validators().trusted(v2_.pk));
        BEAST_EXPECT(!env.app().validators().trusted(stranger_.pk));

        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        auto const base = openTxns(env);
        auto const a = makePeer(env, 1);
        auto const b = makePeer(env, 2);
        auto const c = makePeer(env, 3);

        // The position both validators end up with.
        auto const txns = sorted({entropyTx(seq, 1), entropyTx(seq, 2)});
        auto const set = setHash(env, base, txns);
        auto const first = makePacket(v1_, prev, set, txns);
        auto const second = makePacket(v2_, prev, set, txns);
        BEAST_EXPECT(first->signature() != second->signature());

        // A stranger's copy does nothing, and does not stand in the way of
        // the validators' copies.
        receive(env, *a, makePacket(stranger_, prev, set, txns));
        BEAST_EXPECT(!have(env, set));
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).empty());
        BEAST_EXPECT(relayed(*c).empty());

        // Neither does a copy with a signature that does not verify.
        {
            auto const forged =
                std::make_shared<protocol::TMEntropySet>(*first);
            forged->set_signature(second->signature());
            receive(env, *b, forged);
            BEAST_EXPECT(!have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).empty());
            BEAST_EXPECT(relayed(*c).empty());
        }

        // The first validator's copy builds the set. It goes to every peer
        // that has not shown us that content, as it arrived.
        receive(env, *a, first);
        BEAST_EXPECT(have(env, set));
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).empty());
        {
            auto const sets = relayed(*c);
            if (BEAST_EXPECT(sets.size() == 1))
            {
                BEAST_EXPECT(
                    sets[0].SerializeAsString() == first->SerializeAsString());
            }
        }

        // The same message again, from another peer
        receive(env, *c, first);
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).empty());
        BEAST_EXPECT(relayed(*c).empty());

        // The other validator's copy is valid, and has nothing to add.
        receive(env, *c, second);
        BEAST_EXPECT(have(env, set));
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).empty());
        BEAST_EXPECT(relayed(*c).empty());
    }

    void
    testNotRelayed()
    {
        testcase("Not relayed");
        using namespace jtx;

        // for a round we are not in
        {
            Env env{*this, config(), supported_amendments()};
            establish(env);

            auto const prev = env.closed()->info().hash;
            auto const seq = env.closed()->info().seq + 1;
            auto const a = makePeer(env, 1);
            auto const b = makePeer(env, 2);

            Txns const txns{entropyTx(seq, 1)};
            auto const set = setHash(env, openTxns(env), txns);

            receive(env, *a, makePacket(v1_, sha512Half(prev), set, txns));
            BEAST_EXPECT(!have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).empty());

            // unlike the one for the round we are in
            receive(env, *a, makePacket(v1_, prev, set, txns));
            BEAST_EXPECT(have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).size() == 1);
        }

        // without featureRNG
        {
            Env env{*this, config(), supported_amendments() - featureRNG};
            establish(env);

            auto const prev = env.closed()->info().hash;
            auto const seq = env.closed()->info().seq + 1;
            auto const a = makePeer(env, 1);
            auto const b = makePeer(env, 2);

            Txns const txns{entropyTx(seq, 1)};
            auto const set = setHash(env, openTxns(env), txns);

            receive(env, *a, makePacket(v1_, prev, set, txns));
            BEAST_EXPECT(!have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).empty());
        }
    }

    void
    testRelayedInRound()
    {
        testcase("Relayed once we are in the round");
        using namespace jtx;

        Env env{*this, config(), supported_amendments()};
        prepare(env);

        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        BEAST_EXPECT(env.app().getOPs().getConsensus().prevLedgerID() != prev);

        auto const a = makePeer(env, 1);
        auto const b = makePeer(env, 2);
        auto const c = makePeer(env, 3);

        auto const txns = sorted({entropyTx(seq, 1), entropyTx(seq, 2)});
        auto const set = setHash(env, openTxns(env), txns);

        // A validator that is ahead of us. It is not for our round yet, so
        // it is neither used nor relayed.
        receive(env, *a, makePacket(v1_, prev, set, txns));
        BEAST_EXPECT(!have(env, set));
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).empty());
        BEAST_EXPECT(relayed(*c).empty());

        // Now we are in the round. We push our own position, and the set
        // is built from what we were sent.
        startRound(env);
        relayed(*a);
        relayed(*b);
        relayed(*c);
        BEAST_EXPECT(have(env, set));

        // Another validator's copy has not been relayed yet, and is for our
        // round. It goes to the peer that has not shown us the content.
        auto const second = makePacket(v2_, prev, set, txns);
        receive(env, *b, second);
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).empty());
        auto const sets = relayed(*c);
        if (BEAST_EXPECT(sets.size() == 1))
        {
            BEAST_EXPECT(
                sets[0].SerializeAsString() == second->SerializeAsString());
        }
    }

    void
    testPushed()
    {
        testcase("Pushed with our proposal");
        using namespace jtx;

        Env env{*this, config(), supported_amendments()};
        prepare(env);

        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        auto const a = makePeer(env, 1);
        auto const b = makePeer(env, 2);

        startRound(env);

        auto const sentA = relayed(*a);
        auto const sentB = relayed(*b);
        if (!BEAST_EXPECT(sentA.size() == 1 && sentB.size() == 1))
            return;
        auto const& m = sentA[0];
        BEAST_EXPECT(m.SerializeAsString() == sentB[0].SerializeAsString());

        BEAST_EXPECT(uint256{m.previousledger()} == prev);
        PublicKey const pk{makeSlice(m.nodepubkey())};
        BEAST_EXPECT(pk == env.app().getValidationPublicKey());

        if (!BEAST_EXPECT(m.transactions_size() == 1))
            return;
        auto const item = entropy_set::makeItem(makeSlice(m.transactions(0)));
        if (!BEAST_EXPECT(item))
            return;
        BEAST_EXPECT(entropy_set::ledgerSeq(*item) == seq);

        uint256 const set{m.sethash()};
        BEAST_EXPECT(set == setHash(env, openTxns(env), {item}));
        BEAST_EXPECT(have(env, set));

        BEAST_EXPECT(verifyDigest(
            pk,
            entropy_set::digest(prev, set, std::vector<uint256>{item->key()}),
            makeSlice(m.signature()),
            false));
    }

    void
    testStale()
    {
        testcase("Stale");
        using namespace jtx;

        Env env{*this, config(), supported_amendments()};
        establish(env);

        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq;
        BEAST_EXPECT(env.app().getLedgerMaster().getValidLedgerIndex() == seq);

        auto const a = makePeer(env, 1);
        auto const b = makePeer(env, 2);
        auto const base = openTxns(env);

        // Signed for the round we are in, so it would be used and relayed,
        // but the ledger it is for is one we have already validated.
        {
            Txns const txns{entropyTx(seq, 1)};
            auto const set = setHash(env, base, txns);
            receive(env, *a, makePacket(v1_, prev, set, txns));
            BEAST_EXPECT(!have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).empty());
        }

        // The same, for the next ledger.
        {
            Txns const txns{entropyTx(seq + 1, 1)};
            auto const set = setHash(env, base, txns);
            receive(env, *a, makePacket(v1_, prev, set, txns));
            BEAST_EXPECT(have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).size() == 1);
        }
    }

    void
    testMalformed()
    {
        testcase("Malformed");
        using namespace jtx;

        Env env{*this, config(), supported_amendments()};
        establish(env);

        auto const prev = env.closed()->info().hash;
        auto const seq = env.closed()->info().seq + 1;
        auto const base = openTxns(env);
        auto const a = makePeer(env, 1);
        auto const b = makePeer(env, 2);

        auto const txns = sorted({entropyTx(seq, 1), entropyTx(seq, 2)});
        auto const set = setHash(env, base, txns);
        auto const good = makePacket(v1_, prev, set, txns);

        // Each of these is the good packet with one thing wrong with it.
        // None of them is used or relayed.
        auto const rejected = [&](auto&& damage, bool resign = true) {
            auto const m = std::make_shared<protocol::TMEntropySet>(*good);
            damage(*m);
            if (resign)
                sign(*m, v1_);
            receive(env, *a, m);
            BEAST_EXPECT(!have(env, set));
            BEAST_EXPECT(relayed(*a).empty());
            BEAST_EXPECT(relayed(*b).empty());
        };

        // a signature of the wrong size
        rejected(
            [](auto& m) { m.set_signature(std::string(63, '\x30')); }, false);
        rejected(
            [](auto& m) { m.set_signature(std::string(73, '\x30')); }, false);

        // a key that is not a secp256k1 key
        rejected(
            [](auto& m) {
                auto const pk = randomKeyPair(KeyType::ed25519).first;
                m.set_nodepubkey(pk.data(), pk.size());
            },
            false);
        rejected([](auto& m) { m.set_nodepubkey("not a key"); }, false);

        // hashes of the wrong size
        rejected(
            [](auto& m) { m.set_previousledger(std::string(31, '\x01')); },
            false);
        rejected(
            [](auto& m) { m.set_sethash(std::string(33, '\x01')); }, false);

        // no transactions, and too many
        rejected([](auto& m) { m.clear_transactions(); }, false);
        rejected(
            [&](auto& m) {
                for (std::uint32_t n = 3; n <= entropy_set::maxTxns + 1; ++n)
                {
                    auto const tx = entropyTx(seq, n);
                    m.add_transactions(tx->data(), tx->size());
                }
            },
            false);

        // transactions out of order, and one twice
        rejected([](auto& m) { m.mutable_transactions()->SwapElements(0, 1); });
        rejected([](auto& m) {
            std::string const last = m.transactions(1);
            m.add_transactions(last);
        });

        // something that is not a ttENTROPY
        rejected([&](auto& m) {
            Serializer s;
            STTx(ttAMENDMENT, [&](auto& obj) {
                obj.setAccountID(sfAccount, AccountID());
                obj.setFieldH256(sfAmendment, sha512Half(seq));
                obj.setFieldU32(sfLedgerSequence, seq);
            }).add(s);
            m.set_transactions(0, s.data(), s.size());
        });

        // something larger than a ttENTROPY may be
        rejected([](auto& m) {
            m.mutable_transactions(0)->resize(entropy_set::maxTxnBytes + 1);
        });

        // Something that starts like a ttENTROPY and cannot be read. It is
        // on its own, so that it is in order whatever its ID turns out to be
        // and is only found out once the signature has been verified.
        rejected([](auto& m) {
            m.mutable_transactions()->RemoveLast();
            m.mutable_transactions(0)->pop_back();
        });

        // After all that, the good packet still gets through.
        receive(env, *a, good);
        BEAST_EXPECT(have(env, set));
        BEAST_EXPECT(relayed(*a).empty());
        BEAST_EXPECT(relayed(*b).size() == 1);
    }

public:
    void
    run() override
    {
        testRelay();
        testNotRelayed();
        testRelayedInRound();
        testPushed();
        testStale();
        testMalformed();
    }
};

BEAST_DEFINE_TESTSUITE(PeerEntropySet, overlay, ripple);

}  // namespace test
}  // namespace ripple
