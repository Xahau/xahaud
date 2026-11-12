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

#include <xrpld/app/consensus/RCLEntropySet.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>
#include <exception>

namespace ripple {
namespace entropy_set {

namespace {

// Whether tx is the canonical serialization of a ttENTROPY of acceptable
// size. Sets id to its transaction ID if so.
bool
parse(Slice const& tx, uint256& id)
{
    if (tx.size() > maxTxnBytes || !looksLikeEntropyTx(tx))
        return false;

    try
    {
        SerialIter sit(tx);
        STTx const stx(sit);
        if (stx.getTxnType() != ttENTROPY)
            return false;

        // The ID an STTx reports is that of its canonical serialization. A
        // different ID for these bytes means they are some other encoding of
        // the transaction, which is not what a transaction set would hold.
        id = txID(tx);
        return stx.getTransactionID() == id;
    }
    catch (std::exception const&)
    {
        return false;
    }
}

}  // namespace

uint256
txID(Slice const& tx)
{
    return sha512Half(HashPrefix::transactionID, tx);
}

bool
looksLikeEntropyTx(Slice const& tx)
{
    // Fields are serialized in canonical order and no transaction field sorts
    // before sfTransactionType (a UINT16 with field code 2). So a serialized
    // transaction always starts with that field's ID, 0x12, followed by the
    // transaction type in big-endian order.
    return tx.size() >= 3 && tx[0] == 0x12 && tx[1] == (ttENTROPY >> 8) &&
        tx[2] == (ttENTROPY & 0xff);
}

boost::intrusive_ptr<SHAMapItem const>
makeItem(Slice const& tx)
{
    uint256 id;
    if (!parse(tx, id))
        return {};
    return make_shamapitem(id, tx);
}

std::uint32_t
ledgerSeq(SHAMapItem const& item)
{
    SerialIter sit(item.slice());
    return STTx(sit).getFieldU32(sfLedgerSequence);
}

bool
isEntropyItem(SHAMapItem const& item)
{
    uint256 id;
    return parse(item.slice(), id) && id == item.key();
}

Txns
entropyTxns(SHAMap const& set)
{
    Txns txns;
    set.visitLeaves(
        [&txns](boost::intrusive_ptr<SHAMapItem const> const& item) {
            if (isEntropyItem(*item))
                txns.push_back(item);
        });
    std::sort(txns.begin(), txns.end(), [](auto const& a, auto const& b) {
        return a->key() < b->key();
    });
    return txns;
}

std::shared_ptr<SHAMap>
withoutEntropy(SHAMap const& set)
{
    auto base = set.snapShot(true);
    for (auto const& tx : entropyTxns(set))
        base->delItem(tx->key());
    return base->snapShot(false);
}

std::shared_ptr<SHAMap>
rebuild(SHAMap const& base, Txns const& txns, uint256 const& setHash)
{
    auto map = base.snapShot(true);
    for (auto const& tx : txns)
    {
        if (!map->addItem(SHAMapNodeType::tnTRANSACTION_NM, tx))
            return {};
    }

    auto set = map->snapShot(false);
    if (set->getHash().as_uint256() != setHash)
        return {};
    return set;
}

uint256
digest(
    uint256 const& prevLedger,
    uint256 const& setHash,
    std::vector<uint256> const& txIDs)
{
    sha512_half_hasher h;
    using beast::hash_append;
    hash_append(h, HashPrefix::entropySet);
    hash_append(h, prevLedger);
    hash_append(h, setHash);
    for (auto const& id : txIDs)
        hash_append(h, id);
    return static_cast<sha512_half_hasher::result_type>(h);
}

uint256
digest(uint256 const& prevLedger, uint256 const& setHash, Txns const& txns)
{
    std::vector<uint256> ids;
    ids.reserve(txns.size());
    for (auto const& tx : txns)
        ids.push_back(tx->key());
    return digest(prevLedger, setHash, ids);
}

uint256
uniqueId(uint256 const& digest, Slice const& publicKey, Slice const& signature)
{
    Serializer s(512);
    s.addBitString(digest);
    s.addVL(publicKey);
    s.addVL(signature);
    return s.getSHA512Half();
}

}  // namespace entropy_set
}  // namespace ripple
