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

#ifndef RIPPLE_APP_LEDGER_MANIFESTSYNC_H_INCLUDED
#define RIPPLE_APP_LEDGER_MANIFESTSYNC_H_INCLUDED

#include <xrpld/shamap/SHAMap.h>
#include <xrpld/shamap/SHAMapNodeID.h>
#include <xrpl/basics/base_uint.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace ripple {

/** The nodes a partly synced state map still needs to read its manifests.

    Walks the manifest directory as far as the nodes held allow: the root page;
    every other page, all at once, since the root names the last of them and
    pages are only ever added after it; each master object listed; and the
    ephemeral object each unrevoked one names, which is what a validation is
    resolved through. Where a key cannot be read, the first node on its path
    that is not held is reported, once however many keys it blocks.

    Asking peers for the nodes reported and calling again converges in a
    handful of rounds -- roughly the depth of the map plus one round each for
    the pages and for the objects they list -- on a set of nodes far smaller
    than the map. After that every read forEachLedgerManifest() and
    ManifestCache::applyLedgerDirectory() make succeeds on the partial map.

    A map with no manifest directory converges on the path to where its root
    would be, and nothing else.

    @param stateMap State map, with at least its root node
    @param max Most nodes to report

    @return nodes needed, as getMissingNodes() reports them; empty once every
        manifest can be read
*/
std::vector<std::pair<SHAMapNodeID, uint256>>
missingManifestNodes(SHAMap const& stateMap, std::size_t max);

}  // namespace ripple

#endif
