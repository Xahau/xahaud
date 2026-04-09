#ifndef RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/protocol/PublicKey.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

namespace ripple {

/// Export signature collector for the retriable export approach.
///
/// Stores multisign signatures from validators for pending ttEXPORT
/// transactions.  Signatures arrive via two paths:
///
///   1. Proposal ingestion (onTrustedPeerMessage) — post-checkSign,
///      sender-bound, and (when possible) multisign-verified.
///   2. SHAMap merge (onAcquiredSidecarSet) — trusted + verified.
///
/// Signatures are either **verified** (cryptographically checked against
/// buildMultiSigningData) or **unverified** (stored on proposal-level
/// trust alone, e.g. when the ttEXPORT tx isn't in the open ledger yet
/// due to relay ordering).
///
/// The distinction matters for caching: `hasVerifiedSignature()` returns
/// true only for verified sigs, so unverified sigs get re-checked when
/// encountered again through a path that CAN verify.
///
//@@start export-sig-collector-mutex
/// Thread-safe.
class ExportSigCollector
{
    mutable std::mutex mutex_;
    //@@end export-sig-collector-mutex

    struct SigEntry
    {
        std::set<PublicKey> validators;
        /// Actual multisign signatures keyed by validator pubkey.
        /// Empty buffers mean pubkey-only (standalone/test quorum counting).
        std::map<PublicKey, Buffer> signatures;
        /// Validators whose signatures have been cryptographically verified
        /// via buildMultiSigningData + verify().
        std::set<PublicKey> verified;
        std::uint32_t firstSeenSeq{0};
    };

    std::unordered_map<uint256, SigEntry> sigs_;
    std::set<uint256> sentThisRound_;

    static constexpr std::uint32_t maxStaleLedgers = 256;

public:
    /// Store a signature that has been cryptographically verified
    /// against buildMultiSigningData + verify().
    void
    addVerifiedSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        Buffer const& signature)
    {
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        entry.signatures[validator] = signature;
        entry.verified.insert(validator);
    }

    /// Store a signature from a trusted source (checkSign + sender
    /// binding passed) but without multisign content verification.
    /// Used when the ttEXPORT tx isn't in the open ledger yet due
    /// to relay ordering.  Will be upgraded to verified if the same
    /// sig is encountered again through a path that CAN verify.
    void
    addUnverifiedSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        Buffer const& signature)
    {
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        // Don't overwrite a verified sig with an unverified one.
        if (entry.verified.find(validator) == entry.verified.end())
            entry.signatures[validator] = signature;
    }

    /// Store a pubkey-only entry (no real signature).  Used in
    /// standalone mode where quorum counting is sufficient.
    void
    addStandaloneSignature(uint256 const& txnHash, PublicKey const& validator)
    {
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        if (entry.signatures.find(validator) == entry.signatures.end())
            entry.signatures[validator] = Buffer{};
    }

    /// Check if a cryptographically verified signature exists.
    /// Used to skip redundant verify() calls when the same sig
    /// arrives via multiple paths (proposal + SHAMap merge).
    bool
    hasVerifiedSignature(uint256 const& txnHash, PublicKey const& validator)
        const
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return false;
        return it->second.verified.find(validator) != it->second.verified.end();
    }

    std::size_t
    signatureCount(uint256 const& txnHash) const
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return 0;
        return it->second.validators.size();
    }

    void
    clear(uint256 const& txnHash)
    {
        std::lock_guard lock(mutex_);
        sigs_.erase(txnHash);
    }

    /// Get a snapshot of all sigs (pubkeys only) for building the SHAMap.
    std::unordered_map<uint256, std::set<PublicKey>>
    snapshot() const
    {
        std::lock_guard lock(mutex_);
        std::unordered_map<uint256, std::set<PublicKey>> result;
        for (auto const& [hash, entry] : sigs_)
            result[hash] = entry.validators;
        return result;
    }

    /// Get a snapshot including actual multisign signatures.
    /// Returns: txHash -> map of (validatorPK -> signature buffer).
    /// Empty buffers mean no real signature was collected for that validator.
    std::unordered_map<uint256, std::map<PublicKey, Buffer>>
    snapshotWithSigs() const
    {
        std::lock_guard lock(mutex_);
        std::unordered_map<uint256, std::map<PublicKey, Buffer>> result;
        for (auto const& [hash, entry] : sigs_)
            result[hash] = entry.signatures;
        return result;
    }

    /// Atomic quorum check + snapshot for a single txHash.
    /// Returns the signatures if quorum is met, nullopt otherwise.
    /// Eliminates the TOCTOU window between signatureCount() and
    /// snapshotWithSigs() in Export::doApply.
    std::optional<std::map<PublicKey, Buffer>>
    checkQuorumAndSnapshot(uint256 const& txnHash, std::size_t threshold) const
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end() || it->second.validators.size() < threshold)
            return std::nullopt;
        return it->second.signatures;
    }

    /// Remove entries older than maxStaleLedgers.
    void
    cleanupStale(std::uint32_t currentSeq)
    {
        std::lock_guard lock(mutex_);
        for (auto it = sigs_.begin(); it != sigs_.end();)
        {
            if (it->second.firstSeenSeq > 0 &&
                currentSeq > it->second.firstSeenSeq + maxStaleLedgers)
                it = sigs_.erase(it);
            else
                ++it;
        }
    }

    /// Returns true if we haven't sent our sig for this tx yet this round.
    /// Marks it as sent on first call.
    bool
    markSent(uint256 const& txnHash)
    {
        std::lock_guard lock(mutex_);
        return sentThisRound_.insert(txnHash).second;
    }

    /// Clear per-round state. Call at the start of each consensus round.
    void
    clearRound()
    {
        std::lock_guard lock(mutex_);
        sentThisRound_.clear();
    }
};

}  // namespace ripple

#endif
