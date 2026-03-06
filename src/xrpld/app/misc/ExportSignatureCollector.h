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

#ifndef RIPPLE_APP_MISC_EXPORTSIGNATURECOLLECTOR_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGNATURECOLLECTOR_H_INCLUDED

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/UintTypes.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace ripple {

class Application;
class ReadView;
class ValidatorKeys;

/** Determine whether a validator key is eligible for export quorum.

    Membership model:
    - Prefer UNLReport ActiveValidators when available.
    - Fall back to local trusted validator configuration when UNLReport
      is unavailable (including early ledgers).
*/
bool
isExportValidatorTrusted(
    ReadView const& view,
    Application& app,
    PublicKey const& validator,
    beast::Journal const& j);

/** Determine export UNL size used for 80% quorum threshold.

    Uses UNLReport ActiveValidators size when present, otherwise falls back
    to local trusted validator set size (minimum 1).
*/
std::size_t
getExportUNLSize(ReadView const& view, Application& app);

/** Collects validator signatures for pending exports.

    Export signatures are collected via validation messages rather than
    on-ledger transactions. This eliminates the O(n²) metadata bloat that
    occurs when accumulating signatures in ledger entries.

    The collector stores signatures in memory until quorum (80% UNL) is reached,
    at which point a ttEXPORT transaction can be created with all signatures.

    Continuous broadcasting:
    ========================
    Validators sign ALL pending ltEXPORTED_TXN entries every ledger. The
    collector caches signatures so validators don't need to re-sign; they
    just retrieve and re-broadcast their cached signature.

    This ensures:
    - Late validators can contribute (sign when they come online)
    - Network partitions self-heal (signatures propagate on reconnect)
    - Node restarts recover (re-sign from ledger state, ltEXPORTED_TXN exists)

    The ltEXPORTED_TXN in the ledger is the gatekeeper - once deleted (after
    ttEXPORT processed or export expired), signatures naturally stop being
    broadcast. The collector clears its cache when ttEXPORT is applied.

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

    /** Get the number of cryptographically verified signatures.

        @param txnHash The hash of the exported transaction
        @return Number of verified validator signatures
    */
    std::size_t
    verifiedSignatureCount(uint256 const& txnHash) const;

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

    /** Check if we have a signature from a specific validator.

        Used to check if we've already signed an export (for caching).

        @param txnHash The hash of the exported transaction
        @param validator The public key of the validator
        @return true if signature exists from this validator
    */
    bool
    hasSignatureFrom(uint256 const& txnHash, PublicKey const& validator) const;

    /** Get a signature from a specific validator.

        @param txnHash The hash of the exported transaction
        @param validator The public key of the validator
        @return The signer object, or std::nullopt if not found
    */
    std::optional<STObject>
    getSignatureFrom(uint256 const& txnHash, PublicKey const& validator) const;

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

    // --- Signature verification cache ---

    /** Stash the serialized transaction data for signature verification.

        Called by signPendingExports() when signing, so we have the txn
        data available to verify signatures from other validators.

        @param txnHash The hash of the exported transaction
        @param txnData Serialized STTx for building verification data
        @param currentSeq Current ledger sequence (for stale cleanup aging)
    */
    void
    stashTxnData(
        uint256 const& txnHash,
        Serializer txnData,
        LedgerIndex currentSeq);

    /** Verify and add a signature.

        Verifies the signature against the cached txn data before adding.
        If txn data isn't cached yet (race), adds without verification and
        marks as unverified.

        @param txnHash The hash of the exported transaction
        @param validator The public key of the signing validator
        @param signer The STObject containing the signature (sfSigner)
        @param currentSeq Current ledger sequence
        @return true if signature was added (regardless of verification status)
    */
    bool
    verifyAndAddSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        STObject signer,
        LedgerIndex currentSeq);

    /** Check if a signature has been cryptographically verified.

        @param txnHash The hash of the exported transaction
        @param validator The public key of the validator
        @return true if signature exists AND has been verified
    */
    bool
    isSignatureVerified(uint256 const& txnHash, PublicKey const& validator)
        const;

    /** Verify a previously-added signature that wasn't verified on add.

        Called by Transactor as fallback when isSignatureVerified returns false.

        @param txnHash The hash of the exported transaction
        @param validator The public key of the validator
        @return true if verification succeeded (or already verified)
    */
    bool
    verifySignature(uint256 const& txnHash, PublicKey const& validator);

private:
    // Map<txnHash, Map<validatorPubKey, SignerObject>>
    std::map<uint256, std::map<PublicKey, STObject>> signatures_;

    // Track when each export was first seen (for timeout)
    std::map<uint256, LedgerIndex> firstSeenLedger_;

    // Signature verification cache
    // Serialized STTx for building multisig verification data
    std::map<uint256, Serializer> exportedTxnData_;

    // Which signatures have been cryptographically verified
    std::map<uint256, std::set<PublicKey>> verified_;

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

/**
 * Sign pending exports for ephemeral signature collection.
 *
 * Called during validate() to sign ALL pending ltEXPORTED_TXN entries. The
 * signatures are returned as (txnHash, sfSigner) pairs to be included in the
 * TMValidation message and broadcast to peers.
 *
 * @param view The current ledger view being validated
 * @param app The application (for validator keys and UNL)
 * @param j Journal for logging
 * @return Vector of (txnHash, signerObject) pairs to broadcast
 */
std::vector<std::pair<uint256, STObject>>
signPendingExports(
    ReadView const& view,
    Application& app,
    beast::Journal const& j);

}  // namespace ripple

#endif
