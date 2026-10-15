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

#ifndef RIPPLE_APP_LEDGER_LITEWATCH_H_INCLUDED
#define RIPPLE_APP_LEDGER_LITEWATCH_H_INCLUDED

#include <xrpld/shamap/SHAMap.h>
#include <xrpld/shamap/SHAMapNodeID.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ripple {

class Config;
class ReadView;

/** The set of ledger objects a lite node tracks.

    A node in [lite_mode] does not hold a ledger's state map. For each
    validated ledger it fetches the header, the whole transaction map (small,
    and needed to answer `tx` and account subscriptions), and only the state
    map nodes on the paths from the root to the keys in this set. Everything
    else about the ledger stays on the network.

    The set has three tiers:

    - System keys every ledger needs to be usable at all: the fee settings,
      the amendments object, the skip list and the negative UNL. Always
      present, never removed.
    - Configured keys from [lite_watch] and the lite_watch RPC: account
      roots, raw keylets and trust lines. Removed only by lite_unwatch.
    - Discovered keys: objects that transactions touching a watched account
      created or modified (a trust line opened to it, an escrow it is the
      destination of, hook state its hooks wrote). Added by discover() as
      each ledger's transactions arrive, removed again when the object is
      deleted.

    All methods are thread safe. The walk over the state map in
    missingNodes() reads a map that InboundLedger is still adding nodes to,
    which is what SHAMap::getMissingNodes() already does.
*/
class LiteWatch
{
public:
    enum class Tier { system, configured, discovered };

    struct Entry
    {
        uint256 key;
        Tier tier;
        /** Why it is watched, for lite_status: "account rXXX", "system
            fees", "created by <txid>", ... */
        std::string reason;
    };

    /** Most keys the set will hold, system keys included. Discovery stops
        adding at this point rather than let one busy issuer account drag a
        Raspberry Pi into fetching its whole owner tree. */
    static constexpr std::size_t maxKeys = 16384;

    LiteWatch(Config const& config, beast::Journal journal);

    /** True when the server runs in [lite_mode]. */
    bool
    enabled() const
    {
        return enabled_;
    }

    /** Watch an account: its AccountRoot and Hook object, plus discovery of
        the objects its transactions touch. Returns false if already watched
        or full. */
    bool
    watchAccount(AccountID const& id, Tier tier = Tier::configured);

    /** Watch one ledger object by key. */
    bool
    watchKey(
        uint256 const& key,
        Tier tier = Tier::configured,
        std::string reason = {});

    /** Stop watching a key. System keys cannot be removed. Returns true if
        something was removed. */
    bool
    unwatchKey(uint256 const& key);

    /** Stop watching an account and its root. Objects discovered through it
        are dropped as well; configured keys are kept. */
    bool
    unwatchAccount(AccountID const& id);

    bool
    isWatched(uint256 const& key) const;

    bool
    isWatchedAccount(AccountID const& id) const;

    /** Every key currently watched. */
    std::vector<uint256>
    keys() const;

    /** Every entry, for lite_status. */
    std::vector<Entry>
    entries() const;

    std::size_t
    size() const;

    /** The nodes a partly held state map still needs to read every watched
        key. One entry per node, however many keys it blocks; at most `max`.
        Empty once every watched key can be read (or shown absent).

        @param stateMap State map with at least its root node
        @param max Most nodes to report
    */
    std::vector<std::pair<SHAMapNodeID, uint256>>
    missingNodes(SHAMap const& stateMap, std::size_t max) const;

    /** Learn from one validated transaction and its metadata which objects
        are now of interest.

        - An affected node any of whose account fields name a watched account
          (Account, Owner, Destination, Issuer, the two limits of a trust
          line, ...) is watched from now on.
        - If the transaction was about a watched account (sent by it, ran a
          hook of it, or wrote an object naming it), every node it created
          is watched too: a new trust line, escrow, check, offer, URIToken
          or hook state belonging to the account is almost always among
          them, and a transaction creates few objects.
        - A deleted node that was only discovered is forgotten.

        @return number of keys added
    */
    std::size_t
    discover(STTx const& tx, STObject const& meta, std::uint32_t ledgerSeq);

    /** discover() over every transaction of a ledger whose transaction map
        is complete. Reading the state map is not required. */
    std::size_t
    discover(ReadView const& ledger);

    Json::Value
    getJson() const;

private:
    bool
    addLocked(uint256 const& key, Tier tier, std::string reason);

    bool const enabled_;
    bool const discoverEnabled_;
    beast::Journal j_;

    mutable std::mutex mutex_;
    hash_map<uint256, Entry> keys_;
    hash_set<AccountID> accounts_;
};

}  // namespace ripple

#endif
