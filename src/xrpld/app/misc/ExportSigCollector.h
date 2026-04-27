#ifndef RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/contract.h>
#include <xrpl/protocol/PublicKey.h>
#include <algorithm>
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
/// Only verified signatures count toward quorum, appear in SHAMap
/// convergence, and are assembled into the final export blob.
/// Unverified sigs are a local cache that can be upgraded to verified
/// via `upgradeSignature()` when the tx becomes available (e.g. in
/// Export::doApply which always has the tx).
///
//@@start export-sig-collector-mutex
/// Thread-safe.
class ExportSigCollector
{
    mutable std::mutex mutex_;
    //@@end export-sig-collector-mutex

    struct SigEntry
    {
        /// All validators that have contributed (verified or unverified).
        std::set<PublicKey> validators;
        /// Actual multisign signature bytes keyed by validator pubkey.
        /// Empty buffers mean pubkey-only (standalone mode).
        std::map<PublicKey, Buffer> signatures;
        /// Validators whose sigs have been cryptographically verified.
        /// Only these count toward quorum and appear in SHAMap/snapshot.
        std::set<PublicKey> verified;
        std::uint32_t firstSeenSeq{0};
    };

    std::unordered_map<uint256, SigEntry> sigs_;
    std::set<uint256> sentThisRound_;

    static constexpr std::uint32_t maxStaleLedgers = 256;

    void
    touchSeq(SigEntry& entry, std::uint32_t seq)
    {
        if (entry.firstSeenSeq == 0 && seq > 0)
            entry.firstSeenSeq = seq;
    }

public:
    /// Store a signature that has been cryptographically verified
    /// against buildMultiSigningData + verify().
    void
    addVerifiedSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        Buffer const& signature,
        std::uint32_t currentSeq = 0)
    {
        XRPL_ASSERT(
            signature.size() > 0,
            "ripple::ExportSigCollector::addVerifiedSignature : "
            "non-empty signature");
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        entry.signatures[validator] = signature;
        entry.verified.insert(validator);
        touchSeq(entry, currentSeq);
    }

    /// Store a signature from a trusted source (checkSign + sender
    /// binding passed) but without multisign content verification.
    /// Used when the ttEXPORT tx isn't in the open ledger yet due
    /// to relay ordering.  Will be upgraded to verified via
    /// upgradeSignature() when the tx becomes available.
    ///
    /// Does NOT count toward quorum or appear in SHAMap/snapshot.
    void
    addUnverifiedSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        Buffer const& signature,
        std::uint32_t currentSeq = 0)
    {
        XRPL_ASSERT(
            signature.size() > 0,
            "ripple::ExportSigCollector::addUnverifiedSignature : "
            "non-empty signature");
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        // Don't overwrite a verified sig with an unverified one.
        if (entry.verified.find(validator) == entry.verified.end())
            entry.signatures[validator] = signature;
        touchSeq(entry, currentSeq);
    }

    /// Upgrade a previously unverified sig to verified.
    /// Called from Export::doApply after verifying against the inner tx.
    /// The caller passes the exact buffer it verified; we only promote
    /// if the stored buffer still matches (guards against concurrent
    /// overwrites between unverifiedSignatures() and this call).
    void
    upgradeSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        Buffer const& verifiedBuf)
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return;
        auto sit = it->second.signatures.find(validator);
        if (sit == it->second.signatures.end() || sit->second.size() == 0)
            return;
        // Only promote if the stored buffer is the same one we verified.
        if (!(sit->second == verifiedBuf))
            return;
        it->second.verified.insert(validator);
    }

    /// Store a pubkey-only entry (no real signature).  Used in
    /// standalone mode where quorum counting is sufficient.
    /// Treated as verified (standalone has no consensus to verify against).
    void
    addStandaloneSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        std::uint32_t currentSeq = 0)
    {
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        if (entry.signatures.find(validator) == entry.signatures.end())
            entry.signatures[validator] = Buffer{};
        entry.verified.insert(validator);
        touchSeq(entry, currentSeq);
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
        return it->second.verified.count(validator) > 0;
    }

    /// Return unverified validators for a given txHash.
    /// Used by Export::doApply to find sigs that need verification.
    std::map<PublicKey, Buffer>
    unverifiedSignatures(uint256 const& txnHash) const
    {
        std::lock_guard lock(mutex_);
        std::map<PublicKey, Buffer> result;
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return result;
        for (auto const& [pk, buf] : it->second.signatures)
        {
            if (buf.size() > 0 && it->second.verified.count(pk) == 0)
                result[pk] = buf;
        }
        return result;
    }

    bool
    hasUnverifiedSignatures() const
    {
        std::lock_guard lock(mutex_);
        for (auto const& [_, entry] : sigs_)
        {
            for (auto const& [pk, buf] : entry.signatures)
            {
                if (buf.size() > 0 && entry.verified.count(pk) == 0)
                    return true;
            }
        }
        return false;
    }

    /// Count of VERIFIED signatures only.
    template <class IncludeValidator>
    std::size_t
    signatureCount(uint256 const& txnHash, IncludeValidator includeValidator)
        const
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return 0;

        return std::count_if(
            it->second.verified.begin(),
            it->second.verified.end(),
            includeValidator);
    }

    /// Count of VERIFIED signatures only.
    std::size_t
    signatureCount(uint256 const& txnHash) const
    {
        return signatureCount(txnHash, [](PublicKey const&) { return true; });
    }

    void
    clear(uint256 const& txnHash)
    {
        std::lock_guard lock(mutex_);
        sigs_.erase(txnHash);
    }

    /// Get a snapshot of VERIFIED sigs (pubkeys only) for building
    /// the SHAMap.  Only verified sigs appear in convergence.
    std::unordered_map<uint256, std::set<PublicKey>>
    snapshot() const
    {
        std::lock_guard lock(mutex_);
        std::unordered_map<uint256, std::set<PublicKey>> result;
        for (auto const& [hash, entry] : sigs_)
        {
            if (!entry.verified.empty())
                result[hash] = entry.verified;
        }
        return result;
    }

    /// Get a snapshot of VERIFIED signatures including sig buffers.
    template <class IncludeValidator>
    std::unordered_map<uint256, std::map<PublicKey, Buffer>>
    snapshotWithSigs(IncludeValidator includeValidator) const
    {
        std::lock_guard lock(mutex_);
        std::unordered_map<uint256, std::map<PublicKey, Buffer>> result;
        for (auto const& [hash, entry] : sigs_)
        {
            std::map<PublicKey, Buffer> verifiedSigs;
            for (auto const& pk : entry.verified)
            {
                if (!includeValidator(pk))
                    continue;

                auto sit = entry.signatures.find(pk);
                if (sit != entry.signatures.end())
                    verifiedSigs[pk] = sit->second;
            }
            if (!verifiedSigs.empty())
                result[hash] = std::move(verifiedSigs);
        }
        return result;
    }

    /// Get a snapshot of VERIFIED signatures including sig buffers.
    std::unordered_map<uint256, std::map<PublicKey, Buffer>>
    snapshotWithSigs() const
    {
        return snapshotWithSigs([](PublicKey const&) { return true; });
    }

    /// Atomic quorum check + snapshot for a single txHash.
    /// Returns VERIFIED signatures if quorum is met, nullopt otherwise.
    template <class IncludeValidator>
    std::optional<std::map<PublicKey, Buffer>>
    checkQuorumAndSnapshot(
        uint256 const& txnHash,
        std::size_t threshold,
        IncludeValidator includeValidator) const
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return std::nullopt;

        std::map<PublicKey, Buffer> result;
        for (auto const& pk : it->second.verified)
        {
            if (!includeValidator(pk))
                continue;

            auto sit = it->second.signatures.find(pk);
            XRPL_ASSERT(
                sit != it->second.signatures.end(),
                "ripple::ExportSigCollector::checkQuorumAndSnapshot : "
                "verified key must exist in signatures map");
            XRPL_ASSERT(
                sit->second.size() > 0,
                "ripple::ExportSigCollector::checkQuorumAndSnapshot : "
                "verified signature must be non-empty");
            if (sit != it->second.signatures.end())
                result[pk] = sit->second;
        }

        if (result.size() < threshold)
            return std::nullopt;

        return result;
    }

    /// Atomic quorum check + snapshot for a single txHash.
    /// Returns VERIFIED signatures if quorum is met, nullopt otherwise.
    std::optional<std::map<PublicKey, Buffer>>
    checkQuorumAndSnapshot(uint256 const& txnHash, std::size_t threshold) const
    {
        return checkQuorumAndSnapshot(
            txnHash, threshold, [](PublicKey const&) { return true; });
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
