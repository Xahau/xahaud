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
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/Protocol.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ripple {

/** Manifest directory pages read while a ledger is still being acquired.

    Enough entries to fill the manifest cache's ledger tier, which is all a
    bulk read of the directory can admit. The master keys a node actually
    depends on are fetched by key instead, wherever they are listed, so a
    directory grown long by anyone able to fund an account cannot make the
    early phase of a sync arbitrarily long.
*/
std::uint64_t constexpr manifestSyncPages = 16;

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
    @param keys Further keys to read, with the ephemeral object any manifest
        among them names: in practice keylet::manifest() of each master key on
        the node's validator lists, which needs no directory to find
    @param maxPages Most directory pages to read, the root page included

    @return nodes needed, as getMissingNodes() reports them; empty once every
        manifest can be read
*/
std::vector<std::pair<SHAMapNodeID, uint256>>
missingManifestNodes(
    SHAMap const& stateMap,
    std::size_t max,
    std::vector<uint256> const& keys = {},
    std::uint64_t maxPages = dirNodeMaxPages);

/** Ledgers to read manifests from, chosen by how many peers report them.

    A node that has never validated a ledger, and holds no manifest for enough
    of the validators it trusts, cannot tell which validations to believe. The
    closed ledgers its peers report are then its only evidence of where the
    network is, and any recent ledger of the network holds the manifests
    published on it.

    Choosing wrongly costs little. Every manifest read from a ledger is
    verified as from any other source and the cache only moves forward, so a
    ledger a peer made up can at worst leave something out. Reading several
    candidates, rather than trusting the single most reported, is what keeps
    one misreporting peer among a few from deciding what is read.

    @param reported The closed ledger each peer last reported, one entry per
        peer. Zero entries are ignored.
    @param skip Ledgers already read, or being read
    @param max Most ledgers to return

    @return distinct ledgers, the most widely reported first. Ties go to the
        larger hash, as when NetworkOPs picks a ledger by peer count, so the
        first is usually a ledger being acquired anyway.
*/
std::vector<uint256>
pickManifestCandidates(
    std::vector<uint256> const& reported,
    hash_set<uint256> const& skip,
    std::size_t max);

/** How many validators need a manifest before a node can trust a ledger.

    @param validators The validators that count: the trusted set once
        ValidatorList has worked it out, and every listed one before that
    @param trusted Whether `validators` is the trusted set, and so whether
        `quorum` has been worked out from it
    @param quorum ValidatorList::quorum(). Before the trusted set exists this
        is a placeholder, so the usual 80% of `validators` is assumed instead.

    @return the number needed; never more than `validators`
*/
std::size_t
manifestQuorum(std::size_t validators, bool trusted, std::size_t quorum);

}  // namespace ripple

#endif
