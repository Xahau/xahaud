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
            list[jss::effective] = 0;
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

    static Json::Value
    ingest(
        jtx::Account const& from,
        std::uint32_t seq,
        std::string const& xpof,
        std::optional<std::string> fee = "0")
    {
        Json::Value jv;
        jv[jss::TransactionType] = "Ingest";
        jv[jss::Account] = from.human();
        jv[sfRemoteNetworkID.jsonName] = 0;
        jv[sfLedgerSequence.jsonName] = seq;
        jv[jss::Blob] = strHex(xpof);
        if (fee)
            jv[jss::Fee] = *fee;
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
        // crosses. It counts fewer leaves and still proves, for free.
        Ingest::Nodes fewer = l1.inner;
        auto const want =
            Ingest::sampleLeaves(l1.inner, l1.info.txHash, l1.info.hash);
        for (auto const& [h, node] : l1.inner)
        {
            if (h == l1.info.txHash)
                continue;
            fewer.erase(h);
            if (Ingest::sampleLeaves(fewer, l1.info.txHash, l1.info.hash) ==
                want)
                break;
            fewer.emplace(h, node);
        }
        BEAST_EXPECT(fewer.size() == l1.inner.size() - 1);
        env(ingest(alice, 100, OtherNetwork::proof(l1, fewer)));
        env.close();
        auto sle = feeObj();
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT((*sle)[sfLedgerSequence] == 100);
            BEAST_EXPECT((*sle)[sfTransactionCount] < 40);
            BEAST_EXPECT((*sle)[sfBurnedDrops] == l1.burn);
            BEAST_EXPECT((*sle)[sfSampleFee] == l1.median);
            BEAST_EXPECT((*sle)[sfExportFeeUnit] == l1.median);
        }

        // the same ledger again is not free; complete, and paid for, it wins
        auto const full = OtherNetwork::proof(l1, l1.inner);
        env(ingest(alice, 100, full), ter(telINSUF_FEE_P));
        env(ingest(alice, 100, full, std::nullopt));
        env.close();
        sle = feeObj();
        if (BEAST_EXPECT(sle))
            BEAST_EXPECT((*sle)[sfTransactionCount] == 40);
        env(ingest(alice, 100, OtherNetwork::proof(l1, fewer), std::nullopt),
            ter(tefALREADY));

        // an inner node on a sample path left out: no transaction hashes to
        // what then passes for the leaf
        {
            Ingest::Nodes broken = l1.inner;
            broken.erase(l1.info.txHash);
            env(ingest(alice, 100, OtherNetwork::proof(l1, broken)),
                ter(temMALFORMED));
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
            env(ingest(alice, 100, bad), ter(temMALFORMED));

        // And a proof is taken in its one canonical form only, so that a
        // free transaction cannot carry anything else into the ledger: not
        // a byte after it, not a key it does not need, not a tree node or a
        // validation twice, not another spelling of the same hex.
        env(ingest(alice, 100, full + " "), ter(temMALFORMED));
        {
            Json::Value x;
            BEAST_EXPECT(Json::Reader().parse(full, x));
            // (as parsed and written back it is the proof itself)
            BEAST_EXPECT(Json::FastWriter().write(x) == full);

            auto padded = x;
            padded["memo"] = std::string(1024, 'x');
            env(ingest(alice, 100, Json::FastWriter().write(padded)),
                ter(temMALFORMED));

            auto twice = x;
            twice["tree"].append(x["tree"][0u]);
            env(ingest(alice, 100, Json::FastWriter().write(twice)),
                ter(temMALFORMED));

            auto lower = x;
            auto parent = x["parent"].asString();
            std::transform(
                parent.begin(), parent.end(), parent.begin(), [](char c) {
                    return static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                });
            lower["parent"] = parent;
            env(ingest(alice, 100, Json::FastWriter().write(lower)),
                ter(temMALFORMED));
        }

        // newer ledgers fold the recorded median into the unit, one behind
        auto const l2 = other.ledger(l1.info, closeTime + 4, fees);
        std::vector<std::pair<std::int64_t, int>> dear(40, {5'000, 0});
        auto const l3 = other.ledger(l2.info, closeTime + 8, dear);
        env(ingest(alice, 101, OtherNetwork::proof(l2, l2.inner)));
        env.close();
        sle = feeObj();
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT((*sle)[sfExportFeeUnit] == l1.median);
            BEAST_EXPECT((*sle)[sfSampleFee] == l2.median);
        }
        env(ingest(alice, 102, OtherNetwork::proof(l3, l3.inner)));
        env.close();
        sle = feeObj();
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT(l3.median == 5'000);
            BEAST_EXPECT(
                (*sle)[sfExportFeeUnit] == Ingest::fold(l1.median, l2.median));
            BEAST_EXPECT((*sle)[sfSampleFee] == 5'000);
        }

        // never backwards: an older ledger is not free, so unpaid it fails
        // the fee check before preclaim sees it; paid, it is still refused
        env(ingest(alice, 101, OtherNetwork::proof(l2, l2.inner)),
            ter(telINSUF_FEE_P));
        env(ingest(alice, 101, OtherNetwork::proof(l2, l2.inner), std::nullopt),
            ter(tefPAST_SEQ));

        // a ledger must be newer than the one proven to be free, and the
        // leaf count can only be undercounted, so the median is the fee: a
        // whale in forty transactions never owns eight samples' middle
        BEAST_EXPECT(l1.median < 1'000'000);
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
    }
};

BEAST_DEFINE_TESTSUITE(Ingest, app, ripple);

}  // namespace test
}  // namespace ripple
