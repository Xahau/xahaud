//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
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

#include <xrpld/app/tx/detail/PQKeyLedger.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PQPublicKey.h>
#include <xrpl/protocol/TER.h>

#include <algorithm>
#include <iterator>

namespace ripple {
namespace pqkey {

std::shared_ptr<SLE const>
read(ReadView const& view, AccountID const& id)
{
    if (!view.rules().enabled(featurePQSignatures))
        return nullptr;
    return view.read(keylet::pqPublicKey(id));
}

std::optional<Blob>
readInlineKey(ReadView const& view, AccountID const& id)
{
    auto const sle = read(view, id);
    if (!sle)
        return std::nullopt;
    auto const type = pq::keyTypeFromByte(sle->getFieldU8(sfPublicKeyType));
    if (!type)
        return std::nullopt;
    return pq::makeInlineKey(*type, makeSlice(sle->getFieldVL(sfPublicKeyData)));
}

STTx::PQKeyLookup
makeLookup(ReadView const& view)
{
    return [&view](AccountID const& id) { return readInlineKey(view, id); };
}

std::uint64_t
unrevealedKeyBytes(ReadView const& view, STTx const& tx)
{
    if (!view.rules().enabled(featurePQSignatures))
        return 0;
    std::uint64_t bytes = 0;
    for (auto const& [id, key] : tx.inlinePQKeys())
        if (!view.exists(keylet::pqPublicKey(id)))
            bytes += key.size() - 2;  // the prefix and type byte are not stored
    return bytes;
}

TER
reveal(ApplyView& view, STTx const& tx, beast::Journal j)
{
    if (!view.rules().enabled(featurePQSignatures))
        return tesSUCCESS;

    for (auto const& [id, key] : tx.inlinePQKeys())
    {
        auto const k = keylet::pqPublicKey(id);
        if (view.exists(k))
            continue;

        auto const inlineKey = makeSlice(key);
        auto const type = pq::inlineKeyType(inlineKey);
        if (!type)
            return tefINTERNAL;  // LCOV_EXCL_LINE: inlinePQKeys() filtered

        auto sle = std::make_shared<SLE>(k);
        sle->setFieldU8(sfPublicKeyType, static_cast<std::uint8_t>(*type));
        sle->setFieldVL(sfPublicKeyData, pq::rawKey(inlineKey));
        sle->setFieldU64(sfReferenceCount, 1);
        view.insert(sle);

        JLOG(j.trace()) << "PQ public key revealed for " << toBase58(id);
    }
    return tesSUCCESS;
}

void
addReference(ApplyView& view, AccountID const& id, beast::Journal j)
{
    if (!view.rules().enabled(featurePQSignatures))
        return;
    auto sle = view.peek(keylet::pqPublicKey(id));
    if (!sle)
        return;
    sle->setFieldU64(sfReferenceCount, sle->getFieldU64(sfReferenceCount) + 1);
    view.update(sle);
    JLOG(j.trace()) << "PQ public key " << toBase58(id) << " refcount +1";
}

void
removeReference(ApplyView& view, AccountID const& id, beast::Journal j)
{
    if (!view.rules().enabled(featurePQSignatures))
        return;
    auto sle = view.peek(keylet::pqPublicKey(id));
    if (!sle)
        return;
    auto const count = sle->getFieldU64(sfReferenceCount);
    if (count <= 1)
    {
        view.erase(sle);
        JLOG(j.trace()) << "PQ public key " << toBase58(id) << " deleted";
        return;
    }
    sle->setFieldU64(sfReferenceCount, count - 1);
    view.update(sle);
    JLOG(j.trace()) << "PQ public key " << toBase58(id) << " refcount -1";
}

void
adjustReferences(
    ApplyView& view,
    std::vector<AccountID> before,
    std::vector<AccountID> after,
    beast::Journal j)
{
    if (!view.rules().enabled(featurePQSignatures))
        return;
    auto normalize = [](std::vector<AccountID>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    normalize(before);
    normalize(after);

    std::vector<AccountID> removed;
    std::vector<AccountID> added;
    std::set_difference(
        before.begin(),
        before.end(),
        after.begin(),
        after.end(),
        std::back_inserter(removed));
    std::set_difference(
        after.begin(),
        after.end(),
        before.begin(),
        before.end(),
        std::back_inserter(added));

    for (auto const& id : removed)
        removeReference(view, id, j);
    for (auto const& id : added)
        addReference(view, id, j);
}

}  // namespace pqkey
}  // namespace ripple
