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

#ifndef RIPPLE_APP_MISC_MANIFESTLEDGER_H_INCLUDED
#define RIPPLE_APP_MISC_MANIFESTLEDGER_H_INCLUDED

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/TER.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace ripple {

class ApplyView;
class ReadView;
struct Manifest;

/** Write a manifest to the ledger, replacing any earlier one for its master.

    A manifest is stored twice so that it can be found from either key:

        keylet::manifest(masterKey)  -> obj1, sfManifestID -> obj2
        keylet::manifest(signingKey) -> obj2, sfManifestID -> obj1

    A revocation has no signing key, so it is stored only as obj1, with no
    sfManifestID. The master key's account points at obj1 with its own
    sfManifestID.

    obj1 is listed in keylet::manifestDir() and records its page there in
    sfOwnerNode. The entry is made with the master key's first manifest and is
    never removed: every later manifest for the master key, a revocation
    included, is written at the same key, so the entry stays and the page is
    carried from the old obj1 to the new. Removing and re-adding it instead
    would move it between pages on every key rotation for no gain. obj2 is not
    listed; it moves with the ephemeral key, and a reader finds it from obj1.

    Both of the old objects are erased and both new ones written afresh, so the
    two copies can never drift apart.

    @param view View to write to
    @param account The master key's account root, which is updated
    @param manifest The manifest as signed, in its serialized STObject form
    @param parsed The same manifest, deserialized and checked by the caller
    @param j Journal for reporting a corrupt ledger

    @return tesSUCCESS; tecDIR_FULL if the directory cannot take another
        master key; tefBAD_LEDGER if the existing objects are inconsistent
*/
TER
writeManifestObjects(
    ApplyView& view,
    std::shared_ptr<SLE> const& account,
    STObject const& manifest,
    Manifest const& parsed,
    beast::Journal j);

/** Visit every master key's manifest object listed in keylet::manifestDir().

    Each master key is visited once, with its current object: a revocation if
    it has been revoked. Entries that do not name an ltMANIFEST object are
    skipped.

    The view need not be complete. It must be able to read the directory and
    the objects listed in it, which is all this reads; a view over a partially
    synced state map that holds them will do.

    @param view View to read from
    @param f Called with each master manifest object
    @param maxPages Most directory pages to read, the root page included.
        Pages are only ever appended, so this reads the oldest entries. A
        partial view need hold no more pages than this.
*/
void
forEachLedgerManifest(
    ReadView const& view,
    std::function<void(std::shared_ptr<SLE const> const&)> const& f,
    std::uint64_t maxPages = dirNodeMaxPages);

}  // namespace ripple

#endif
