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

#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/ManifestLedger.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STObject.h>

#include <algorithm>
#include <optional>

namespace ripple {

TER
writeManifestObjects(
    ApplyView& view,
    std::shared_ptr<SLE> const& account,
    STObject const& manifest,
    Manifest const& parsed,
    beast::Journal j)
{
    AccountID const id = account->getAccountID(sfAccount);
    Keylet const klMan1 = keylet::manifest(parsed.masterKey);

    // The page obj1 is listed on, carried across a replacement.
    std::optional<std::uint64_t> page;

    if (account->isFieldPresent(sfManifestID))
    {
        uint256 const firstID = account->getFieldH256(sfManifestID);

        // The account is derived from the master key, so it can only ever
        // have pointed at that key's obj1. Anything else is corruption, and
        // carrying its page across would corrupt the directory as well.
        auto const sleMan1 = view.peek(Keylet{ltMANIFEST, firstID});
        if (firstID != klMan1.key || !sleMan1 ||
            sleMan1->getAccountID(sfAccount) != id)
        {
            JLOG(j.error()) << "SetManifest: Old manifest object missing or "
                               "misowned (ID1) !! "
                            << strHex(firstID);
            return tefBAD_LEDGER;
        }

        // Absent when the previous manifest was a revocation.
        if (sleMan1->isFieldPresent(sfManifestID))
        {
            uint256 const secondID = sleMan1->getFieldH256(sfManifestID);
            auto const sleMan2 = view.peek(Keylet{ltMANIFEST, secondID});
            if (secondID == firstID || !sleMan2 ||
                sleMan2->getAccountID(sfAccount) != id)
            {
                JLOG(j.error())
                    << "SetManifest: Old manifest object missing, misowned or "
                       "self-referential (ID2) !! "
                    << strHex(secondID);
                return tefBAD_LEDGER;
            }
            view.erase(sleMan2);
        }

        if (sleMan1->isFieldPresent(sfOwnerNode))
            page = sleMan1->getFieldU64(sfOwnerNode);

        view.erase(sleMan1);
    }

    std::optional<Keylet> klMan2;
    if (!parsed.revoked() && parsed.signingKey)
        klMan2 = keylet::manifest(*parsed.signingKey);

    // Neither key may still be occupied: preclaim rejects an ephemeral key held
    // by another account, and the block above cleared this account's own
    // copies.
    if (view.exists(klMan1) || (klMan2 && view.exists(*klMan2)))
    {
        JLOG(j.error()) << "SetManifest: Manifest keylet already occupied !! "
                        << strHex(klMan1.key);
        return tefBAD_LEDGER;
    }

    // A master key's first manifest. Also an obj1 written before it carried a
    // page, which has no entry to keep.
    if (!page)
    {
        page = view.dirInsert(
            keylet::manifestDir(), klMan1.key, [](std::shared_ptr<SLE> const&) {
            });
        if (!page)
            return tecDIR_FULL;
    }

    // Mirror the manifest losslessly, signatures included, so any node can
    // reconstruct and independently verify it (ManifestCache::applyLedger).
    // Field *presence* is copied faithfully: sfVersion is soeDEFAULT in the
    // manifest format, so materialising an absent one would alter the signed
    // payload and break verification.
    auto const write = [&](Keylet const& kl,
                           std::optional<uint256> const& other,
                           std::optional<std::uint64_t> const& dirPage) {
        auto sleMan = std::make_shared<SLE>(kl);
        sleMan->setAccountID(sfAccount, id);
        sleMan->setFieldU32(sfSequence, manifest.getFieldU32(sfSequence));
        sleMan->setFieldVL(sfPublicKey, manifest.getFieldVL(sfPublicKey));
        sleMan->setFieldVL(
            sfMasterSignature, manifest.getFieldVL(sfMasterSignature));
        if (manifest.isFieldPresent(sfVersion))
            sleMan->setFieldU16(sfVersion, manifest.getFieldU16(sfVersion));
        if (manifest.isFieldPresent(sfSigningPubKey))
            sleMan->setFieldVL(
                sfSigningPubKey, manifest.getFieldVL(sfSigningPubKey));
        if (manifest.isFieldPresent(sfSignature))
            sleMan->setFieldVL(sfSignature, manifest.getFieldVL(sfSignature));
        if (manifest.isFieldPresent(sfDomain))
            sleMan->setFieldVL(sfDomain, manifest.getFieldVL(sfDomain));
        if (other)
            sleMan->setFieldH256(sfManifestID, *other);
        if (dirPage)
            sleMan->setFieldU64(sfOwnerNode, *dirPage);
        view.insert(sleMan);
    };

    write(
        klMan1,
        klMan2 ? std::optional<uint256>{klMan2->key} : std::nullopt,
        page);
    if (klMan2)
        write(*klMan2, klMan1.key, std::nullopt);

    account->setFieldH256(sfManifestID, klMan1.key);
    view.update(account);

    return tesSUCCESS;
}

void
forEachLedgerManifest(
    ReadView const& view,
    std::function<void(std::shared_ptr<SLE const> const&)> const& f,
    std::uint64_t maxPages)
{
    auto const& root = keylet::manifestDir();
    auto page = view.read(root);

    // Also bounds the walk should a corrupt ledger link the pages in a cycle.
    maxPages = std::min(maxPages, dirNodeMaxPages);
    for (std::uint64_t pages = 0; page && pages < maxPages; ++pages)
    {
        for (auto const& key : page->getFieldV256(sfIndexes))
            if (auto const sle = view.read(Keylet{ltMANIFEST, key}))
                f(sle);

        auto const next = page->getFieldU64(sfIndexNext);
        if (next == 0 || pages + 1 >= maxPages)
            break;

        page = view.read(keylet::page(root, next));
    }
}

}  // namespace ripple
