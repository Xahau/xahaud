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

#include <test/jtx.h>
#include <test/shamap/common.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/Ingest.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpld/shamap/SHAMapLeafNode.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <algorithm>
#include <cctype>
#include <functional>

namespace ripple {
namespace test {

/** Ingest: proofs of what transactions on the other network pay */
class Ingest_test : public beast::unit_test::suite
{
    using KeyPair = std::pair<PublicKey, SecretKey>;

    static std::string
    manifest(KeyPair const& master, KeyPair const& signing)
    {
        STObject st(sfGeneric);
        st[sfSequence] = 1;
        st[sfPublicKey] = master.first;
        st[sfSigningPubKey] = signing.first;
        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(signing.first),
            signing.second);
        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(master.first),
            master.second,
            sfMasterSignature);
        Serializer s;
        st.add(s);
        return base64_encode(
            std::string(static_cast<char const*>(s.data()), s.size()));
    }

    // a transaction there paying `fee`, multisigned by `signers`
    static Blob
    txn(std::uint32_t n, std::int64_t fee, int signers)
    {
        STObject t(sfGeneric);
        t[sfTransactionType] = static_cast<std::uint16_t>(ttPAYMENT);
        t[sfAccount] = AccountID{n + 1};
        t[sfSequence] = n;
        t.setFieldAmount(sfFee, XRPAmount{fee});
        t[sfSigningPubKey] = Slice{};
        if (signers)
        {
            STArray a(sfSigners);
            for (int i = 0; i < signers; ++i)
            {
                a.push_back(STObject::makeInnerObject(sfSigner));
                a.back()[sfAccount] = AccountID{1000u + i};
            }
            t.setFieldArray(sfSigners, a);
        }
        Serializer s;
        t.add(s);
        return s.peekData();
    }

    /** An XRPL ledger and the XPOF proving it, from one trusted validator */
    struct OtherNetwork
    {
        KeyPair vlMaster = randomKeyPair(KeyType::secp256k1);
        KeyPair vlSigning = randomKeyPair(KeyType::secp256k1);
        KeyPair valMaster = randomKeyPair(KeyType::ed25519);
        KeyPair valSigning = randomKeyPair(KeyType::secp256k1);
        beast::Journal j{beast::Journal::getNullSink()};
        std::uint32_t effective = 0;  // of the lists its ledgers are given

        std::string
        vlKeyHex() const
        {
            return strHex(vlMaster.first);
        }

        struct Ledger
        {
            LedgerInfo info;
            Ingest::Nodes inner;  // the complete tree, as the relay sends it
            std::map<uint256, Blob> leaves;  // leaf hash -> VL(tx) VL(meta)
            std::map<uint256, std::int64_t> unit;  // leaf hash -> drops/sig
            Json::Value xpof;  // without `tree`: see proof()
            std::uint64_t burn = 0;
            std::uint32_t median = 0;
        };

        // ledger `seq` after `parent`, closing at `closeTime`, holding one
        // transaction per entry of `fees` (drops, signers)
        Ledger
        ledger(
            LedgerInfo const& parent,
            std::uint32_t closeTime,
            std::vector<std::pair<std::int64_t, int>> const& fees) const
        {
            Ledger l;
            tests::TestNodeFamily f(j);
            SHAMap map(SHAMapType::TRANSACTION, f);
            for (std::uint32_t n = 0; n < fees.size(); ++n)
            {
                auto const tx = txn(n, fees[n].first, fees[n].second);
                Serializer item;
                item.addVL(tx);
                item.addVL(Blob{0xE1});  // an empty metadata object
                auto const id =
                    sha512Half(HashPrefix::transactionID, makeSlice(tx));
                map.addGiveItem(
                    SHAMapNodeType::tnTRANSACTION_MD,
                    make_shamapitem(id, item.slice()));
                Serializer leaf;
                leaf.addRaw(item.slice());
                leaf.addBitString(id);
                auto const h = sha512Half(HashPrefix::txNode, leaf.slice());
                l.leaves[h] = item.peekData();
                l.unit[h] = std::max<std::int64_t>(
                    10, fees[n].first / (1 + fees[n].second));
                l.burn += fees[n].first;
            }
            // inner hashes are lazy: adding an item zeroes them up the
            // path, and only hashing the map fills them back in
            auto const txHash = map.getHash().as_uint256();
            map.visitNodes([&](SHAMapTreeNode& n) {
                if (n.isInner())
                {
                    Serializer s;
                    n.serializeForWire(s);
                    auto const node = SHAMapTreeNode::makeFromWire(s.slice());
                    l.inner[node->getHash().as_uint256()] = node;
                }
                return true;
            });

            auto& i = l.info;
            i.seq = parent.seq + 1;
            i.drops = parent.drops - XRPAmount{std::int64_t(l.burn)};
            i.parentHash = calculateLedgerHash(parent);
            i.txHash = txHash;
            i.accountHash = uint256{2};
            i.parentCloseTime =
                NetClock::time_point{NetClock::duration{closeTime - 10}};
            i.closeTime = NetClock::time_point{NetClock::duration{closeTime}};
            i.closeTimeResolution = NetClock::duration{10};
            i.hash = calculateLedgerHash(i);

            auto& x = l.xpof;
            auto& lg = x[jss::ledger];
            lg[jss::index] = i.seq;
            lg[jss::coins] = std::to_string(i.drops.drops());
            lg[jss::phash] = to_string(i.parentHash);
            lg[jss::txroot] = to_string(i.txHash);
            lg[jss::acroot] = to_string(i.accountHash);
            lg[jss::pclose] = closeTime - 10;
            lg[jss::close] = closeTime;
            lg[jss::cres] = 10;
            lg[jss::flags] = 0;
            Serializer ph;
            addRaw(parent, ph);
            x["parent"] = strHex(ph.slice());

            STValidation const val(
                NetClock::time_point{NetClock::duration{closeTime + 1}},
                valSigning.first,
                valSigning.second,
                calcNodeID(valMaster.first),
                [&](STValidation& v) {
                    v.setFieldH256(sfLedgerHash, i.hash);
                    v.setFieldU32(sfLedgerSequence, i.seq);
                });
            Serializer vs;
            val.add(vs);
            x[jss::validation][jss::data][toBase58(
                TokenType::NodePublic, valSigning.first)] = strHex(vs.slice());

            Json::Value list;
            list[jss::sequence] = 1;
            list[jss::effective] = effective;
            list[jss::expiration] = closeTime + 365 * 24 * 60 * 60;
            auto& entry = list[jss::validators].append(Json::objectValue);
            entry[jss::validation_public_key] = strHex(valMaster.first);
            entry[jss::manifest] = manifest(valMaster, valSigning);
            auto const blob = Json::FastWriter().write(list);
            auto& unl = x[jss::validation][jss::unl];
            unl[jss::public_key] = vlKeyHex();
            unl[jss::manifest] = manifest(vlMaster, vlSigning);
            unl[jss::blob] = base64_encode(blob);
            unl[jss::signature] = strHex(
                sign(vlSigning.first, vlSigning.second, makeSlice(blob)));
            unl[jss::version] = 1;

            std::vector<std::int64_t> units;
            for (auto const& h :
                 Ingest::sampleLeaves(l.inner, i.txHash, i.hash))
                units.push_back(l.unit.at(h));
            std::sort(units.begin(), units.end());
            l.median = units[units.size() / 2];
            return l;
        }

        // the XPOF with `tree` holding `inner`, and the samples it calls for
        static std::string
        proof(Ledger const& l, Ingest::Nodes const& inner)
        {
            Json::Value x = l.xpof;
            auto& tree = x["tree"] = Json::arrayValue;
            for (auto const& [h, node] : inner)
            {
                Serializer s;
                node->serializeForWire(s);
                tree.append(strHex(s.slice()));
            }
            auto& samples = x["samples"] = Json::arrayValue;
            for (auto const& h :
                 Ingest::sampleLeaves(inner, l.info.txHash, l.info.hash))
            {
                auto& s = samples.append(Json::objectValue);
                if (auto const leaf = l.leaves.find(h); leaf != l.leaves.end())
                {
                    SerialIter sit(makeSlice(leaf->second));
                    s[jss::blob] = strHex(sit.getVL());
                    s[jss::meta] = strHex(sit.getVL());
                }
            }
            return Json::FastWriter().write(x);
        }
    };

    // The transaction that carries `xpof` as a proof of ledger `seq` of the
    // XRPL: field for field what Ingest::make builds, counting the leaves
    // the proof arrives at (none, if it is no proof), then `tweak`ed
    static std::shared_ptr<STTx const>
    ingest(
        jtx::Env& env,
        std::uint32_t seq,
        std::string const& xpof,
        std::function<void(STObject&)> const& tweak = {})
    {
        auto const x = Ingest::parse(
            makeSlice(xpof), beast::Journal{beast::Journal::getNullSink()});
        return std::make_shared<STTx const>(ttINGEST, [&](STObject& o) {
            o[sfAccount] = AccountID{};
            o[sfSequence] = 0;
            o[sfFee] = XRPAmount{0};
            o[sfSigningPubKey] = Slice{};
            o[sfNetworkID] = env.app().config().NETWORK_ID;
            o[sfRemoteNetworkID] = 0;
            o[sfLedgerSequence] = seq;
            o[sfTransactionCount] = x ? x->leaves : 0;
            o[sfBlob] = makeSlice(xpof);
            if (tweak)
                tweak(o);
        });
    }

    // Through the front door, as anyone would send it: the submit RPC, and
    // so checkValidity and the queue. The engine result, or the RPC error
    // if it never got that far.
    static std::string
    submit(jtx::Env& env, STTx const& tx)
    {
        Serializer s;
        tx.add(s);
        auto const jr = env.rpc("submit", strHex(s.slice()))[jss::result];
        return jr.isMember(jss::engine_result)
            ? jr[jss::engine_result].asString()
            : jr[jss::error].asString();
    }

    // Straight to the open ledger, past checkValidity (which would turn a
    // bad proof away as a bad signature): what the transactor itself says,
    // as it must of a transaction that reaches it in a proposed set.
    static TER
    applyDirect(jtx::Env& env, STTx const& tx)
    {
        TER ret = tesSUCCESS;
        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            ret = ripple::apply(env.app(), view, tx, tapNONE, j).ter;
            return false;  // discard
        });
        return ret;
    }

    // What a ttINGEST was before it had no account: from one, signed and
    // sequenced by jtx
    static Json::Value
    signedIngest(
        jtx::Account const& from,
        std::uint32_t seq,
        std::string const& xpof)
    {
        Json::Value jv;
        jv[jss::TransactionType] = "Ingest";
        jv[jss::Account] = from.human();
        jv[sfRemoteNetworkID.jsonName] = 0;
        jv[sfLedgerSequence.jsonName] = seq;
        jv[sfTransactionCount.jsonName] = 40;
        jv[jss::Blob] = strHex(xpof);
        return jv;
    }

    void
    testIngest()
    {
        testcase("ingest");
        using namespace jtx;

        OtherNetwork other;
        Env env{
            *this,
            network::makeNetworkVLConfig(21337, {other.vlKeyHex()}),
            supported_amendments() | featureExport};
        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();
        std::uint32_t const closeTime =
            env.current()->parentCloseTime().time_since_epoch().count();

        // forty transactions: most at 12 drops, two whales, two multisigned
        std::vector<std::pair<std::int64_t, int>> fees(36, {12, 0});
        fees.insert(
            fees.end(), {{1'000'000, 0}, {5'000'000, 0}, {50, 4}, {60, 2}});
        LedgerInfo genesis;
        genesis.seq = 99;
        genesis.drops = XRPAmount{99'999'999'999'999'000LL};
        auto const l1 = other.ledger(genesis, closeTime, fees);
        BEAST_EXPECT(l1.inner.size() > 1 && l1.leaves.size() == 40);
        auto const feeObj = [&] { return env.le(keylet::exportFee(0)); };

        // an incomplete tree: one inner node left out that no sample path
        // crosses. It counts fewer leaves and still proves.
        auto const thin = [&](OtherNetwork::Ledger const& l) {
            Ingest::Nodes fewer = l.inner;
            auto const want =
                Ingest::sampleLeaves(l.inner, l.info.txHash, l.info.hash);
            for (auto const& [h, node] : l.inner)
            {
                if (h == l.info.txHash)
                    continue;
                fewer.erase(h);
                if (Ingest::sampleLeaves(fewer, l.info.txHash, l.info.hash) ==
                    want)
                    break;
                fewer.emplace(h, node);
            }
            BEAST_EXPECT(fewer.size() == l.inner.size() - 1);
            return OtherNetwork::proof(l, fewer);
        };
        auto const inLedger = [&](std::shared_ptr<STTx const> const& tx) {
            return env.closed()->txRead(tx->getTransactionID()).first !=
                nullptr;
        };

        // Nobody's transaction: the one envelope, from account zero, with
        // no signature, sent by whoever holds the proof. It moves the
        // record and nothing else: no account is charged or sequenced, no
        // XAH is burned and no hook runs.
        auto const first = ingest(env, 100, thin(l1));
        {
            // (the envelope says what the proof counts: fewer than the
            // ledger held, and nothing else would be the proof's)
            auto const counted = (*first)[sfTransactionCount];
            BEAST_EXPECT(counted > 0 && counted < 40);
            auto const made = Ingest::make(
                21337, 0, 100, counted, makeSlice(first->getFieldVL(sfBlob)));
            BEAST_EXPECT(made.getTransactionID() == first->getTransactionID());
            BEAST_EXPECT(Ingest::isCanonical(*first, 21337));
            // (it is this network's: another's would carry its NetworkID)
            BEAST_EXPECT(!Ingest::isCanonical(*first, 21338));
            BEAST_EXPECT(first->getAccountID(sfAccount) == AccountID{});
            BEAST_EXPECT(first->getSigningPubKey().empty());
            BEAST_EXPECT(!first->isFieldPresent(sfTxnSignature));
        }
        auto const coins = env.current()->info().drops;
        auto const aliceSeq = env.seq(alice);
        auto const aliceBalance = env.balance(alice);
        BEAST_EXPECT(!env.le(keylet::account(AccountID{})));
        BEAST_EXPECT(submit(env, *first) == "tesSUCCESS");
        env.close();
        auto sle = feeObj();
        if (BEAST_EXPECT(sle))
        {
            // burn and median are the same however much of the tree the
            // proof gave
            BEAST_EXPECT((*sle)[sfLedgerSequence] == 100);
            BEAST_EXPECT((*sle)[sfBurnedDrops] == l1.burn);
            BEAST_EXPECT((*sle)[sfSampleFee] == l1.median);
            BEAST_EXPECT((*sle)[sfExportFeeUnit] == l1.median);
            // and, for admitting the next: what this proof counted, and
            // this network's clock when it was taken
            BEAST_EXPECT(
                (*sle)[sfTransactionCount] == (*first)[sfTransactionCount]);
            BEAST_EXPECT((*sle)[sfParentCloseTime] == closeTime);
        }
        BEAST_EXPECT(env.closed()->info().drops == coins);
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        BEAST_EXPECT(env.balance(alice) == aliceBalance);
        BEAST_EXPECT(!env.le(keylet::account(AccountID{})));
        {
            auto const [tx, meta] =
                env.closed()->txRead(first->getTransactionID());
            if (BEAST_EXPECT(tx && meta))
            {
                auto const& nodes = meta->getFieldArray(sfAffectedNodes);
                BEAST_EXPECT(
                    nodes.size() == 1 && nodes[0].getFName() == sfCreatedNode &&
                    nodes[0].getFieldU16(sfLedgerEntryType) == ltEXPORT_FEE);
                BEAST_EXPECT(!meta->isFieldPresent(sfHookExecutions));
            }
        }

        // One sample a minute, by this network's clock: until it has
        // passed nothing is taken, not the whole tree of the ledger
        // recorded (that ledger has closed, and nothing the record keeps
        // would change) and not a newer ledger. Refused from the envelope
        // alone, and not as malformed: the same proof is good in a minute.
        auto const full = OtherNetwork::proof(l1, l1.inner);
        auto const l2 = other.ledger(l1.info, closeTime + 4, fees);
        auto const p2 = OtherNetwork::proof(l2, l2.inner);
        BEAST_EXPECT(submit(env, *ingest(env, 100, full)) == "tefALREADY");
        BEAST_EXPECT(submit(env, *ingest(env, 101, p2)) == "tefALREADY");
        BEAST_EXPECT(
            !(env.app().getHashRouter().getFlags(
                  ingest(env, 101, p2)->getTransactionID()) &
              SF_BAD));

        // Only the one envelope. A good proof of a newer ledger from an
        // account, signed and paid for, is refused whole: the account
        // keeps its Sequence and its fee.
        env(signedIngest(alice, 101, p2), ter(temMALFORMED));
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        BEAST_EXPECT(env.balance(alice) == aliceBalance);

        // Nor with anything changed or added: it is free and unsigned, so
        // any other field would be data put into the ledger for nothing,
        // and another id for the same proof.
        {
            std::vector<std::function<void(STObject&)>> const tweaks{
                [&](STObject& o) { o[sfAccount] = alice.id(); },
                [](STObject& o) { o[sfSequence] = 1; },
                [](STObject& o) { o[sfFee] = XRPAmount{10}; },
                [](STObject& o) { o[sfFlags] = 0u; },
                [](STObject& o) { o[sfSourceTag] = 7u; },
                [](STObject& o) { o[sfLastLedgerSequence] = 1'000'000u; },
                [](STObject& o) { o[sfTxnSignature] = Slice{}; },
                // (it does prove a ledger, but not the one it names)
                [](STObject& o) { o[sfLedgerSequence] = 102u; },
                // (nor does it count the leaves it says: a proof cannot be
                // passed off as fuller than it is)
                [](STObject& o) { o[sfTransactionCount] = 41u; }};
            for (auto const& tweak : tweaks)
                BEAST_EXPECT(
                    applyDirect(env, *ingest(env, 101, p2, tweak)) ==
                    temMALFORMED);
            // and at the door such a one is just a transaction nobody
            // signed
            BEAST_EXPECT(
                submit(env, *ingest(env, 101, p2, tweaks[4])) ==
                "invalidTransaction");
        }

        // an inner node on a sample path left out: no transaction hashes to
        // what then passes for the leaf
        {
            Ingest::Nodes broken = l1.inner;
            broken.erase(l1.info.txHash);
            BEAST_EXPECT(
                applyDirect(
                    env, *ingest(env, 100, OtherNetwork::proof(l1, broken))) ==
                temMALFORMED);
        }

        // JSON of the wrong shape is refused before anything is read through
        // it: a number, string or array where an object belongs is not
        // something Json::Value::operator[] may be asked a member of
        for (auto const* bad :
             {R"([1])",
              R"({"ledger":1})",
              R"({"ledger":"x","validation":{"unl":{}}})",
              R"({"ledger":{},"validation":"x"})",
              R"({"ledger":{},"validation":{"unl":[1]}})",
              R"({"ledger":{},"validation":{"unl":{}},"tree":{},"samples":[]})",
              R"({"ledger":{},"validation":{"unl":{}},"tree":[],"samples":1})"})
            BEAST_EXPECT(
                applyDirect(env, *ingest(env, 100, bad)) == temMALFORMED);

        // And a proof is taken in its one canonical form only, so that a
        // free transaction cannot carry anything else into the ledger: not
        // a byte after it, not a key it does not need, not a tree node or a
        // validation twice, not another spelling of the same hex, not a
        // list version nothing reads.
        BEAST_EXPECT(
            applyDirect(env, *ingest(env, 101, p2 + " ")) == temMALFORMED);
        {
            Json::Value x;
            BEAST_EXPECT(Json::Reader().parse(p2, x));
            // (as parsed and written back it is the proof itself)
            BEAST_EXPECT(Json::FastWriter().write(x) == p2);

            auto padded = x;
            padded["memo"] = std::string(1024, 'x');
            BEAST_EXPECT(
                applyDirect(
                    env, *ingest(env, 101, Json::FastWriter().write(padded))) ==
                temMALFORMED);

            auto twice = x;
            twice["tree"].append(x["tree"][0u]);
            BEAST_EXPECT(
                applyDirect(
                    env, *ingest(env, 101, Json::FastWriter().write(twice))) ==
                temMALFORMED);

            auto lower = x;
            auto parent = x["parent"].asString();
            std::transform(
                parent.begin(), parent.end(), parent.begin(), [](char c) {
                    return static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                });
            lower["parent"] = parent;
            BEAST_EXPECT(
                applyDirect(
                    env, *ingest(env, 101, Json::FastWriter().write(lower))) ==
                temMALFORMED);

            auto versioned = x;
            versioned[jss::validation][jss::unl][jss::version] = 2;
            BEAST_EXPECT(
                applyDirect(
                    env,
                    *ingest(env, 101, Json::FastWriter().write(versioned))) ==
                temMALFORMED);

            // At the door a proof that does not verify is what a bad
            // signature is to any other transaction: it never reaches the
            // engine.
            BEAST_EXPECT(
                submit(
                    env, *ingest(env, 101, Json::FastWriter().write(padded))) ==
                "invalidTransaction");
        }

        // The minute over, the record is open again, to a newer ledger
        // only.
        std::vector<std::pair<std::int64_t, int>> dear(40, {5'000, 0});
        auto const l3 = other.ledger(l2.info, closeTime + 8, dear);
        auto const p3 = OtherNetwork::proof(l3, l3.inner);
        auto const minute = std::chrono::seconds{2 * Ingest::spacing};
        env.close(minute);
        BEAST_EXPECT(submit(env, *ingest(env, 100, full)) == "tefPAST_SEQ");

        // The fullest proof is the sample. While the ledger a proof was
        // recorded in is open, one that counts more leaves takes its place
        // (and folds nothing more into the unit); one that counts fewer or
        // as many does not, whichever ledger it proves.
        auto const thin2 = ingest(env, 101, thin(l2));
        auto const full2 = ingest(env, 101, p2);
        BEAST_EXPECT((*full2)[sfTransactionCount] == 40);
        BEAST_EXPECT(submit(env, *thin2) == "tesSUCCESS");
        BEAST_EXPECT(submit(env, *full2) == "tesSUCCESS");
        BEAST_EXPECT(applyDirect(env, *thin2) == tefALREADY);
        BEAST_EXPECT(submit(env, *ingest(env, 102, p3)) == "tefALREADY");
        // Both went into the set, and the set applies the fullest first:
        // that one lands and the other is refused for counting no more, so
        // a closed ledger holds one proof a minute and no more.
        env.close();
        BEAST_EXPECT(inLedger(full2) && !inLedger(thin2));
        sle = feeObj();
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT((*sle)[sfLedgerSequence] == 101);
            BEAST_EXPECT((*sle)[sfTransactionCount] == 40);
            BEAST_EXPECT((*sle)[sfExportFeeUnit] == l1.median);
            BEAST_EXPECT((*sle)[sfSampleFee] == l2.median);
        }

        // newer ledgers fold the recorded median into the unit, one behind
        env.close(minute);
        BEAST_EXPECT(submit(env, *ingest(env, 102, p3)) == "tesSUCCESS");
        env.close();
        sle = feeObj();
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT(l3.median == 5'000);
            BEAST_EXPECT(
                (*sle)[sfExportFeeUnit] == Ingest::fold(l1.median, l2.median));
            BEAST_EXPECT((*sle)[sfSampleFee] == 5'000);
        }

        // Strictly newer, always: not an older ledger, and not the one
        // recorded, which is the very transaction that recorded it
        env.close(minute);
        BEAST_EXPECT(submit(env, *ingest(env, 101, p2)) == "tefPAST_SEQ");
        BEAST_EXPECT(submit(env, *ingest(env, 102, p3)) == "tefPAST_SEQ");
        BEAST_EXPECT(applyDirect(env, *ingest(env, 102, p3)) == tefPAST_SEQ);
        sle = feeObj();
        if (BEAST_EXPECT(sle))
            BEAST_EXPECT((*sle)[sfLedgerSequence] == 102);

        // A list that is not in date is no fault in the proof: refused
        // with a tef, so that the one id the proof has is not remembered
        // as bad, and taken once the list is effective.
        {
            other.effective = closeTime + 100 * Ingest::spacing;
            auto const l4 = other.ledger(l3.info, closeTime + 12, fees);
            auto const early =
                ingest(env, 103, OtherNetwork::proof(l4, l4.inner));
            BEAST_EXPECT(applyDirect(env, *early) == tefFAILURE);
            BEAST_EXPECT(submit(env, *early) == "tefFAILURE");
            BEAST_EXPECT(!(
                env.app().getHashRouter().getFlags(early->getTransactionID()) &
                SF_BAD));
            env.close(std::chrono::seconds{101 * Ingest::spacing});
            BEAST_EXPECT(submit(env, *early) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(inLedger(early));
        }

        // All of it for nothing: four ledgers proven and no drop burned
        // (the refused, signed one was never applied)
        BEAST_EXPECT(env.closed()->info().drops == coins);

        // the leaf count can only be undercounted, so the median is the
        // fee: a whale in forty transactions never owns eight samples'
        // middle
        BEAST_EXPECT(l1.median < 1'000'000);
    }

    void
    testDisabled()
    {
        testcase("disabled");
        using namespace jtx;

        // Without the amendment the envelope is nothing: refused at the
        // door without being held against whoever sent it (it is good once
        // the amendment is enabled), and by the transactor.
        Env env{
            *this,
            network::makeNetworkVLConfig(21337, {}),
            supported_amendments() - featureExport};
        auto const tx = ingest(env, 100, "{}");
        BEAST_EXPECT(Ingest::isCanonical(*tx, 21337));
        BEAST_EXPECT(submit(env, *tx) == "invalidTransaction");
        BEAST_EXPECT(applyDirect(env, *tx) == temDISABLED);
    }

    void
    testFold()
    {
        testcase("fold");
        BEAST_EXPECT(Ingest::fold(10, 10) == 10);
        BEAST_EXPECT(Ingest::fold(10, 1'000'000) == (70 + 1'000'000) / 8);
        BEAST_EXPECT(Ingest::fold(1'000, 10) == (7'000 + 10) / 8);
        // never below the reference fee
        BEAST_EXPECT(Ingest::fold(0, 0) == Export::xrplBaseFee.drops());
    }

public:
    void
    run() override
    {
        testFold();
        testIngest();
        testDisabled();
    }
};

BEAST_DEFINE_TESTSUITE(Ingest, app, ripple);

}  // namespace test
}  // namespace ripple
