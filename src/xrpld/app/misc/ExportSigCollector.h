#ifndef RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/protocol/PublicKey.h>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace ripple {

/// Export signature collector for the retriable export approach.
/// Tracks which validators have "signed" each pending export, and
/// which exports we've already attached our own sig to (so we don't
/// redundantly re-send on every proposal).
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
        /// Empty buffers mean pubkey-only (quorum counting without real sigs).
        std::map<PublicKey, Buffer> signatures;
        std::uint32_t firstSeenSeq{0};
    };

    std::unordered_map<uint256, SigEntry> sigs_;
    std::set<uint256> sentThisRound_;

    static constexpr std::uint32_t maxStaleLedgers = 256;

public:
    /// Add a pubkey-only entry (no real signature). Used in standalone/tests
    /// where quorum counting is sufficient.
    void
    addSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        std::uint32_t currentSeq = 0)
    {
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        if (entry.signatures.find(validator) == entry.signatures.end())
            entry.signatures[validator] = Buffer{};
        if (entry.firstSeenSeq == 0 && currentSeq > 0)
            entry.firstSeenSeq = currentSeq;
    }

    /// Add a pubkey + real multisign signature entry.
    void
    addSignature(
        uint256 const& txnHash,
        PublicKey const& validator,
        Buffer const& signature,
        std::uint32_t currentSeq = 0)
    {
        std::lock_guard lock(mutex_);
        auto& entry = sigs_[txnHash];
        entry.validators.insert(validator);
        entry.signatures[validator] = signature;
        if (entry.firstSeenSeq == 0 && currentSeq > 0)
            entry.firstSeenSeq = currentSeq;
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

    bool
    hasQuorum(uint256 const& txnHash, std::size_t threshold) const
    {
        return signatureCount(txnHash) >= threshold;
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

/// Global instance. In production this would be owned by Application.
inline ExportSigCollector&
exportSigCollector()
{
    static ExportSigCollector instance;
    return instance;
}

}  // namespace ripple

#endif
