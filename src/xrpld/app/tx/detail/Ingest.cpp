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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/Import.h>
#include <xrpld/app/tx/detail/Ingest.h>
#include <xrpld/core/Config.h>
#include <xrpld/shamap/SHAMapInnerNode.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

/*  An XPOF is an XPOP's `ledger` and `validation` sections (see Import),
    without `transaction`, plus:

        parent   hex: the parent ledger's header, 118 bytes
        tree     [hex]: every inner node of the transaction tree, as on the
                 wire (SHAMapTreeNode::serializeForWire)
        samples  [{blob, meta}]: the transactions at Ingest::sampleLeaves,
                 in that order, each as its serialized form and metadata

    Canonical form. A free proof must not be a way to store other data, so
    there is one sequence of bytes for what an XPOF proves: Ingest::parse
    rebuilds the document from what it verified and compares it with the
    blob. That is Json::to_string of {ledger, parent, samples, tree,
    validation: {data, unl}} with numbers as numbers (coins a decimal
    string), hex in upper case, base64 padded and unbroken, validations keyed
    by node public key (only ones that count), and tree nodes in ascending
    order of hash in the wire format their branch count calls for. No other
    keys, whitespace, repeats or spellings.
*/

namespace ripple {

static std::string
str(Json::Value const& v)
{
    return v.isString() ? v.asString() : std::string{};
}

static std::optional<Blob>
hex(Json::Value const& v)
{
    return v.isString() ? strUnHex(v.asString()) : std::nullopt;
}

std::vector<uint256>
Ingest::sampleLeaves(
    Nodes const& nodes,
    uint256 const& root,
    uint256 const& ledger)
{
    std::vector<uint256> out;
    for (std::uint32_t i = 0; i < samples; ++i)
    {
        auto const target = sha512Half(ledger, i);
        uint256 h = root;
        for (int depth = 0;; ++depth)
        {
            auto const it = nodes.find(h);
            if (it == nodes.end() || !it->second->isInner())
                break;
            if (depth >= 64)
                return {};
            auto const& in = static_cast<SHAMapInnerNode const&>(*it->second);
            int const nibble =
                (target.data()[depth / 2] >> (depth % 2 ? 0 : 4)) & 0xF;
            int b = 0;
            while (b < 16 && in.isEmptyBranch((nibble + b) % 16))
                ++b;
            if (b == 16)
                return {};
            h = in.getChildHash((nibble + b) % 16).as_uint256();
        }
        out.push_back(h);
    }
    return out;
}

std::uint32_t
Ingest::fold(std::uint32_t unit, std::uint32_t sample)
{
    return std::max<std::uint64_t>(
        Export::xrplBaseFee.drops(), (7ull * unit + sample) / 8);
}

std::optional<Ingest::Xpof>
Ingest::parse(Slice blob, beast::Journal j)
{
    auto const fail = [&](std::string const& why) {
        JLOG(j.warn()) << "Ingest: " << why;
        return std::optional<Xpof>{};
    };
    try
    {
        Json::Value x;
        if (!Json::Reader().parse(
                std::string(
                    reinterpret_cast<char const*>(blob.data()), blob.size()),
                x) ||
            !x.isObject())
            return fail("not JSON");

        // Json::Value::operator[] on anything but an object (or null) is
        // undefined behaviour, not an exception the catch below would see,
        // and this runs in preflight on whatever blob a transaction brings:
        // so the shape is checked before anything is read through it.
        if (!x[jss::ledger].isObject() || !x[jss::validation].isObject() ||
            !x[jss::validation][jss::unl].isObject() || !x["tree"].isArray() ||
            !x["samples"].isArray())
            return fail("not an XPOF");

        // the ledger, hashed from its header fields as an XPOP gives them
        LedgerInfo info;
        auto const& lg = x[jss::ledger];
        auto const coins = to_uint64(str(lg[jss::coins]));
        if (!coins || !info.parentHash.parseHex(str(lg[jss::phash])) ||
            !info.txHash.parseHex(str(lg[jss::txroot])) ||
            !info.accountHash.parseHex(str(lg[jss::acroot])))
            return fail("bad ledger header");
        info.seq = lg[jss::index].asUInt();
        info.drops = XRPAmount{static_cast<std::int64_t>(*coins)};
        info.parentCloseTime =
            NetClock::time_point{NetClock::duration{lg[jss::pclose].asUInt()}};
        info.closeTime =
            NetClock::time_point{NetClock::duration{lg[jss::close].asUInt()}};
        info.closeTimeResolution = NetClock::duration{lg[jss::cres].asUInt()};
        info.closeFlags = lg[jss::flags].asUInt();
        // the header hashes these two as single bytes, so a larger number
        // would be a second way of writing the same ledger
        if (lg[jss::cres].asUInt() > 0xFF || lg[jss::flags].asUInt() > 0xFF)
            return fail("bad ledger header");
        info.hash = calculateLedgerHash(info);

        // its parent, bound by phash: what was burned between them is what
        // this ledger's transactions paid
        auto const parent = hex(x["parent"]);
        if (!parent || parent->size() != 118)
            return fail("bad parent header");
        auto const p = deserializeHeader(makeSlice(*parent));
        if (calculateLedgerHash(p) != info.parentHash ||
            p.seq + 1 != info.seq || p.drops < info.drops)
            return fail("parent header does not match");
        std::uint64_t const burn = (p.drops - info.drops).drops();

        // the UNL: a validator list signed by its publisher, whose signing
        // keys the validations below must come from
        auto const& unl = x[jss::validation][jss::unl];
        auto const m =
            deserializeManifest(base64_decode(str(unl[jss::manifest])));
        auto const data = base64_decode(str(unl[jss::blob]));
        auto const sig = hex(unl[jss::signature]);
        Json::Value list;
        if (!m || !m->signingKey || !m->verify() || !sig ||
            !verify(*m->signingKey, makeSlice(data), makeSlice(*sig)) ||
            !Json::Reader().parse(data, list) || !list.isObject() ||
            !list[jss::validators].isArray())
            return fail("bad UNL");
        std::set<PublicKey> keys;
        for (auto const& v : list[jss::validators])
        {
            // the list is only known to be its publisher's own here (who
            // that is, preclaim checks), so its entries are untrusted too
            if (!v.isObject())
                return fail("bad UNL");
            if (auto const vm =
                    deserializeManifest(base64_decode(str(v[jss::manifest])));
                vm && vm->signingKey && vm->verify())
                keys.insert(*vm->signingKey);
        }
        auto const quorum =
            std::max<std::size_t>(1, list[jss::validators].size() * 4 / 5);

        // the validations: every one given must count (canonical form),
        // so none that fails to parse or verify, is for another ledger or
        // is from a key off the list is passed over, as an XPOP's are
        std::set<PublicKey> agreed;
        Json::Value canonData{Json::objectValue};
        for (auto const& v : x[jss::validation][jss::data])
        {
            auto const raw = hex(v);
            if (!raw)
                return fail("bad validation");
            SerialIter sit(makeSlice(*raw));
            STValidation const val(
                sit, [](PublicKey const& pk) { return calcNodeID(pk); }, false);
            if (val.getLedgerHash() != info.hash ||
                !keys.count(val.getSignerPublic()) || !val.isValid() ||
                !agreed.insert(val.getSignerPublic()).second)
                return fail("validation does not count");
            canonData[toBase58(TokenType::NodePublic, val.getSignerPublic())] =
                strHex(*raw);
        }
        if (agreed.size() < quorum)
            return fail("below quorum");

        // the tree: inner nodes only, all of them hanging off txroot, each
        // once; whatever hangs off one and is not among them is a leaf
        Nodes nodes;
        for (auto const& n : x["tree"])
        {
            auto const raw = hex(n);
            auto const node =
                raw ? SHAMapTreeNode::makeFromWire(makeSlice(*raw)) : nullptr;
            if (!node || !node->isInner())
                return fail("bad tree node");
            if (!nodes.emplace(node->getHash().as_uint256(), node).second)
                return fail("tree node given twice");
        }
        std::set<uint256> reached;
        std::uint32_t leaves = 0;
        auto const walk =
            [&](uint256 const& h, int depth, auto const& walk) -> bool {
            auto const it = nodes.find(h);
            if (it == nodes.end())
            {
                ++leaves;
                return true;
            }
            if (depth > 64 || !reached.insert(h).second)
                return false;
            auto const& in = static_cast<SHAMapInnerNode const&>(*it->second);
            for (int b = 0; b < 16; ++b)
                if (!in.isEmptyBranch(b) &&
                    !walk(in.getChildHash(b).as_uint256(), depth + 1, walk))
                    return false;
            return true;
        };
        if (!nodes.count(info.txHash) || !walk(info.txHash, 0, walk) ||
            reached.size() != nodes.size())
            return fail("tree does not hang off txroot");

        // the samples: the leaves the ledger's hash picks, in full, so the
        // fee comes from a median no one party can choose or own
        auto const want = sampleLeaves(nodes, info.txHash, info.hash);
        auto const& given = x["samples"];
        if (want.empty() || !given.isArray() || given.size() != want.size())
            return fail("samples missing");
        std::vector<std::uint64_t> units;
        Json::Value canonSamples{Json::arrayValue};
        for (Json::UInt i = 0; i < want.size(); ++i)
        {
            if (!given[i].isObject())
                return fail("bad sample");
            auto const tx = hex(given[i][jss::blob]);
            auto const meta = hex(given[i][jss::meta]);
            if (!tx || !meta)
                return fail("bad sample");
            auto& cs = canonSamples.append(Json::objectValue);
            cs[jss::blob] = strHex(*tx);
            cs[jss::meta] = strHex(*meta);
            Serializer s;
            s.addVL(*tx);
            s.addVL(*meta);
            s.addBitString(
                sha512Half(HashPrefix::transactionID, makeSlice(*tx)));
            if (sha512Half(HashPrefix::txNode, s.slice()) != want[i])
                return fail("sample is not the leaf");
            // generic, not STTx: a type this network lacks is still a fee
            SerialIter sit(makeSlice(*tx));
            STObject const t(sit, sfGeneric);
            std::int64_t const signers = t.isFieldPresent(sfSigners)
                ? t.getFieldArray(sfSigners).size()
                : 0;
            units.push_back(std::max<std::int64_t>(
                Export::xrplBaseFee.drops(),
                t.getFieldAmount(sfFee).xrp().drops() / (1 + signers)));
        }
        std::sort(units.begin(), units.end());

        // The canonical form (see the top of this file): the document as
        // rebuilt from what was verified above must be the blob itself.
        {
            Json::Value c{Json::objectValue};
            auto& cl = c[jss::ledger] = Json::objectValue;
            cl[jss::index] = info.seq;
            cl[jss::coins] = std::to_string(info.drops.drops());
            cl[jss::phash] = to_string(info.parentHash);
            cl[jss::txroot] = to_string(info.txHash);
            cl[jss::acroot] = to_string(info.accountHash);
            cl[jss::pclose] = info.parentCloseTime.time_since_epoch().count();
            cl[jss::close] = info.closeTime.time_since_epoch().count();
            cl[jss::cres] = info.closeTimeResolution.count();
            cl[jss::flags] = info.closeFlags;
            c["parent"] = strHex(*parent);
            auto& cv = c[jss::validation] = Json::objectValue;
            cv[jss::data] = std::move(canonData);
            auto& cu = cv[jss::unl] = Json::objectValue;
            cu[jss::public_key] = strHex(m->masterKey);
            cu[jss::manifest] =
                base64_encode(base64_decode(str(unl[jss::manifest])));
            cu[jss::blob] = base64_encode(data);
            cu[jss::signature] = strHex(*sig);
            cu[jss::version] = unl[jss::version].asUInt();
            auto& ct = c["tree"] = Json::arrayValue;
            for (auto const& [hash, node] : nodes)
            {
                Serializer s;
                node->serializeForWire(s);
                ct.append(strHex(s.slice()));
            }
            c["samples"] = std::move(canonSamples);
            auto const canonical = to_string(c);
            if (canonical.size() != blob.size() ||
                std::memcmp(canonical.data(), blob.data(), blob.size()) != 0)
                return fail("not in canonical form");
        }

        return Xpof{
            info,
            burn,
            leaves,
            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                units[units.size() / 2],
                std::numeric_limits<std::uint32_t>::max())),
            m->masterKey,
            list[jss::sequence].asUInt(),
            list[jss::effective].asUInt(),
            list[jss::expiration].asUInt()};
    }
    catch (std::exception const& e)
    {
        return fail(e.what());
    }
}

XRPAmount
Ingest::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // a ledger newer than the one recorded is free; the same one again,
    // with more of its tree, is not
    auto const sle = view.read(keylet::exportFee(tx[sfRemoteNetworkID]));
    if (!sle || tx[sfLedgerSequence] > (*sle)[sfLedgerSequence])
        return XRPAmount{0};
    return Transactor::calculateBaseFee(view, tx);
}

NotTEC
Ingest::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureExport))
        return temDISABLED;
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;
    if (ctx.tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;
    auto const blob = ctx.tx.getFieldVL(sfBlob);
    if (blob.size() > maxBlob)
        return temMALFORMED;
    auto const x = parse(makeSlice(blob), ctx.j);
    if (!x || x->ledger.seq != ctx.tx[sfLedgerSequence])
        return temMALFORMED;
    return preflight2(ctx);
}

TER
Ingest::preclaim(PreclaimContext const& ctx)
{
    auto const seq = ctx.tx[sfLedgerSequence];
    auto const nid = ctx.tx[sfRemoteNetworkID];
    auto const sle = ctx.view.read(keylet::exportFee(nid));
    std::uint32_t const have = sle ? (*sle)[sfLedgerSequence] : 0;
    if (seq < have)
        return tefPAST_SEQ;

    auto const x = parse(makeSlice(ctx.tx.getFieldVL(sfBlob)), ctx.j);
    if (!x)
        return tefINTERNAL;

    // the same ledger again counts only with more of its tree
    if (sle && seq == have && x->leaves <= (*sle)[sfTransactionCount])
        return tefALREADY;

    // the UNL's publisher must be one Import trusts for this very network,
    // its list in date and no older than one already used
    auto const now = ctx.view.parentCloseTime().time_since_epoch().count();
    if (x->vlEffective > now || x->vlExpiration <= now)
        return temMALFORMED;
    if (auto const vl = ctx.view.read(keylet::import_vlseq(x->vlKey));
        vl && (*vl)[sfImportSequence] > x->vlSeq)
        return tefPAST_IMPORT_VL_SEQ;
    if (Import::vlNetwork(ctx.app.config(), ctx.view, strHex(x->vlKey)) != nid)
        return telIMPORT_VL_KEY_NOT_RECOGNISED;
    return tesSUCCESS;
}

TER
Ingest::doApply()
{
    auto const x = parse(makeSlice(ctx_.tx.getFieldVL(sfBlob)), j_);
    if (!x)
        return tefINTERNAL;

    auto& view = ctx_.view();
    auto const nid = ctx_.tx[sfRemoteNetworkID];
    auto sle = view.peek(keylet::exportFee(nid));
    bool const created = !sle;
    if (created)
    {
        sle = std::make_shared<SLE>(keylet::exportFee(nid));
        (*sle)[sfRemoteNetworkID] = nid;
    }

    // A newer ledger folds the one recorded into the unit first; the same
    // ledger, with more of its tree, only replaces what was recorded of it.
    // So the unit trails the record by one ledger, and nothing is unwound.
    if (created)
        (*sle)[sfExportFeeUnit] = x->median;
    else if (x->ledger.seq > (*sle)[sfLedgerSequence])
        (*sle)[sfExportFeeUnit] =
            fold((*sle)[sfExportFeeUnit], (*sle)[sfSampleFee]);
    (*sle)[sfLedgerSequence] = x->ledger.seq;
    (*sle)[sfTransactionCount] = x->leaves;
    (*sle)[sfBurnedDrops] = x->burn;
    (*sle)[sfSampleFee] = x->median;
    if (created)
        view.insert(sle);
    else
        view.update(sle);
    return tesSUCCESS;
}

}  // namespace ripple
