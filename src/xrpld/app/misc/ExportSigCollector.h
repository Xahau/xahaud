#ifndef RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGCOLLECTOR_H_INCLUDED

#include <xrpl/protocol/PublicKey.h>
#include <mutex>
#include <set>
#include <unordered_map>

namespace ripple {

/// Minimal export signature collector for the retriable export approach.
/// Tracks which validators have "signed" each pending export.
/// Thread-safe.
class ExportSigCollector
{
    mutable std::mutex mutex_;
    std::unordered_map<uint256, std::set<PublicKey>> sigs_;

public:
    void
    addSignature(uint256 const& txnHash, PublicKey const& validator)
    {
        std::lock_guard lock(mutex_);
        sigs_[txnHash].insert(validator);
    }

    std::size_t
    signatureCount(uint256 const& txnHash) const
    {
        std::lock_guard lock(mutex_);
        auto it = sigs_.find(txnHash);
        if (it == sigs_.end())
            return 0;
        return it->second.size();
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
};

/// Global instance for the prototype.
/// In production this would be owned by Application.
inline ExportSigCollector&
exportSigCollector()
{
    static ExportSigCollector instance;
    return instance;
}

}  // namespace ripple

#endif
