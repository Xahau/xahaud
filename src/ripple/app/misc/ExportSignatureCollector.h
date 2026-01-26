//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Ripple Labs Inc.

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

#ifndef RIPPLE_APP_MISC_EXPORTSIGNATURECOLLECTOR_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGNATURECOLLECTOR_H_INCLUDED

#include <ripple/basics/base_uint.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/protocol/Protocol.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/UintTypes.h>

#include <map>
#include <mutex>
#include <vector>

namespace ripple {

class Application;
class ReadView;

/** Collects validator signatures for pending exports.

    Export signatures are collected via validation messages rather than
    on-ledger transactions. This eliminates the O(n²) metadata bloat that
    occurs when accumulating signatures in ledger entries.

    The collector stores signatures in memory until quorum is reached,
    at which point a ttEXPORT transaction can be created with all signatures.

    Thread safety: All public methods are thread-safe.
*/
class ExportSignatureCollector
{
public:
    ExportSignatureCollector(beast::Journal journal);

    /** Add a signature for an export.

        @param txnHash The hash of the exported transaction
        @param validator The public key of the signing validator
        @param signer The STObject containing the signature (sfSigner)
        @param currentSeq Current ledger sequence (for tracking first-seen)
    */
    void
    addSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        STObject signer,
        LedgerIndex currentSeq);

    /** Get all signatures collected for an export.

        @param txnHash The hash of the exported transaction
        @return STArray of signer objects, empty if none collected
    */
    STArray
    getSignatures(uint256 const& txnHash) const;

    /** Get the number of signatures collected for an export.

        @param txnHash The hash of the exported transaction
        @return Number of unique validator signatures
    */
    std::size_t
    signatureCount(uint256 const& txnHash) const;

    /** Check if an export has reached quorum.

        Quorum is 80% of the UNL (rounded up).

        @param txnHash The hash of the exported transaction
        @param view The current ledger view (for UNL size)
        @param app The application (for UNL access)
        @return true if quorum reached
    */
    bool
    hasQuorum(uint256 const& txnHash, ReadView const& view, Application& app)
        const;

    /** Get all pending exports that have reached quorum.

        @param view The current ledger view
        @param app The application
        @return Vector of txnHashes that have quorum
    */
    std::vector<uint256>
    getExportsWithQuorum(ReadView const& view, Application& app) const;

    /** Get all pending export hashes.

        @return Vector of all txnHashes being tracked
    */
    std::vector<uint256>
    getPendingExports() const;

    /** Clear signatures for a completed export.

        Called after ttEXPORT is applied to clean up memory.

        @param txnHash The hash of the completed export
    */
    void
    clearForTxn(uint256 const& txnHash);

    /** Clean up stale exports that haven't reached quorum.

        Prevents memory leak from orphaned exports.

        @param currentSeq Current ledger sequence
        @param maxAge Maximum age in ledgers before cleanup (default 256)
    */
    void
    cleanupStale(LedgerIndex currentSeq, LedgerIndex maxAge = 256);

private:
    // Map<txnHash, Map<validatorPubKey, SignerObject>>
    std::map<uint256, std::map<PublicKey, STObject>> signatures_;

    // Track when each export was first seen (for timeout)
    std::map<uint256, LedgerIndex> firstSeenLedger_;

    mutable std::mutex mutex_;
    beast::Journal j_;

    /** Get UNL size from the view.

        @param view The current ledger view
        @param app The application
        @return Number of validators in UNL, or 1 if not available
    */
    std::size_t
    getUNLSize(ReadView const& view, Application& app) const;
};

}  // namespace ripple

#endif
