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

#ifndef RIPPLE_APP_MISC_ACCOUNTINDEXER_H_INCLUDED
#define RIPPLE_APP_MISC_ACCOUNTINDEXER_H_INCLUDED

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/beast/utility/PropertyStream.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Protocol.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace ripple {

class Application;

/** Maintains a persistent AccountIndex -> AccountID index.

    Every AccountRoot created since featureXahauGenesis carries an
    sfAccountIndex: a dense counter (sfAccountCount on the FeeSettings
    object) assigned at creation. Accounts are never deleted on Xahau, so
    the mapping index -> account is immutable and append-only. That gives
    the index two useful properties:

      * a hit is always authoritative, even while the index is building;
      * a miss is only authoritative once the index has caught up with
        the validated ledger the caller is asking about.

    The index lives in its own SQLite database (accountindex.db). It is
    built once by walking the state of a validated ledger on a background
    thread, then kept current from the metadata of each newly validated
    ledger. If a gap in locally available ledgers is detected, or the
    highest indexed value disagrees with sfAccountCount, the index
    rebuilds itself from the current validated ledger.

    Enabled by the [account_index] config stanza.
*/
class AccountIndexer : public beast::PropertyStream::Source
{
protected:
    AccountIndexer() : beast::PropertyStream::Source("accountindex")
    {
    }

public:
    struct Status
    {
        /// Configured on and database opened.
        bool enabled = false;
        /// A full state scan has completed at least once.
        bool built = false;
        /// Every account present in validated ledgers <= this is indexed.
        LedgerIndex indexedSeq = 0;
        /// Newest validated ledger the indexer has been told about.
        LedgerIndex targetSeq = 0;
        /// Number of indexed accounts (== highest index + 1 when dense).
        std::uint64_t count = 0;
    };

    virtual ~AccountIndexer() = default;

    virtual void
    start() = 0;

    virtual void
    stop() = 0;

    /** Called by LedgerMaster for every newly validated ledger.
        Cheap and non-blocking; the work happens on the indexer's thread. */
    virtual void
    onValidatedLedger(LedgerIndex seq) = 0;

    virtual bool
    enabled() const = 0;

    virtual Status
    status() const = 0;

    /** Resolve an AccountIndex to its AccountID.
        Returns nullopt if the index is unknown to the database (which may
        mean "not yet indexed" rather than "does not exist" — see status()). */
    virtual std::optional<AccountID>
    lookup(std::uint64_t index) const = 0;
};

std::unique_ptr<AccountIndexer>
make_AccountIndexer(Application& app, beast::Journal journal);

}  // namespace ripple

#endif
