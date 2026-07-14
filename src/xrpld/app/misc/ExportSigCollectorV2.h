#ifndef RIPPLE_APP_MISC_EXPORTSIGCOLLECTORV2_H_INCLUDED
#define RIPPLE_APP_MISC_EXPORTSIGCOLLECTORV2_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/PublicKey.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ripple {

/** Post-validation Export contribution collector.

    This class is intentionally separate from the live legacy collector. Its
    contribution identity is the immutable Export origin plus the position in
    that origin's pinned validator universe. Publication attempts may reopen,
    but never reset admitted contributions or conflict state.
*/
class ExportSigCollectorV2
{
public:
    using Position = std::uint16_t;

    struct Contribution
    {
        Position position;
        PublicKey signingKey;
        Buffer signature;

        friend bool
        operator==(Contribution const& lhs, Contribution const& rhs)
        {
            return lhs.position == rhs.position &&
                lhs.signingKey == rhs.signingKey &&
                lhs.signature == rhs.signature;
        }
    };

    enum class BeginResult {
        verify,
        duplicate,
        conflicted,
        capacity,
        malformed
    };

    enum class AdmitResult { accepted, duplicate, conflicted, invalid, stale };

    enum class PositionStatus { empty, unique, conflicted };

    class AdmissionTicket
    {
        friend class ExportSigCollectorV2;

        uint256 origin_;
        std::uint64_t reservation_;
        Contribution contribution_;

        AdmissionTicket(
            uint256 const& origin,
            std::uint64_t reservation,
            Contribution contribution)
            : origin_(origin)
            , reservation_(reservation)
            , contribution_(std::move(contribution))
        {
        }

    public:
        uint256 const&
        origin() const
        {
            return origin_;
        }

        Contribution const&
        contribution() const
        {
            return contribution_;
        }
    };

    struct Admission
    {
        BeginResult result;
        std::optional<AdmissionTicket> ticket;
    };

    struct AdmitOutcome
    {
        AdmitResult result;
        // Present when this admission observed the second valid encoding.
        std::optional<Contribution> priorContribution;
    };

    using UnionSnapshot = std::map<uint256, std::vector<Contribution>>;

    // Conservative local bounds. These are tuning values, not wire-format
    // dimensions, and must be reviewed before feature activation.
    static constexpr std::size_t maxTrackedOrigins = 4096;
    static constexpr std::uint32_t maxStaleLedgers = 256;
    static constexpr std::size_t maxSignatureBytes = 72;

private:
    struct PositionEntry
    {
        std::optional<Contribution> unique;
        bool conflicted{false};
        std::map<std::uint64_t, Contribution> reservations;
    };

    struct OriginEntry
    {
        std::map<Position, PositionEntry> positions;
        std::set<Position> published;
        std::uint64_t publicationGeneration{0};
        std::uint32_t lastTouchedSeq{0};
    };

    mutable std::mutex mutex_;
    std::unordered_map<uint256, OriginEntry> origins_;
    std::uint64_t nextReservation_{1};

    static bool
    sameEncoding(Contribution const& lhs, Contribution const& rhs)
    {
        return lhs.signingKey == rhs.signingKey &&
            lhs.signature == rhs.signature;
    }

    static void
    touch(OriginEntry& entry, std::uint32_t currentSeq)
    {
        if (currentSeq > entry.lastTouchedSeq)
            entry.lastTouchedSeq = currentSeq;
    }

    std::uint64_t
    nextReservation()
    {
        auto const result = nextReservation_++;
        if (nextReservation_ == 0)
            nextReservation_ = 1;
        return result;
    }

public:
    /** Reserve one contribution encoding for verification.

        The reservation is the pre-verification DoS boundary. At most two
        distinct encodings can be in-flight or admitted at a position. The
        caller performs cryptographic verification only for `verify`, then
        returns the ticket through `admitContribution`.
    */
    Admission
    beginAdmission(
        uint256 const& origin,
        Contribution contribution,
        std::uint32_t currentSeq = 0)
    {
        if (origin.isZero() ||
            contribution.position >=
                ExportLimits::maxValidatorUniverseMembers ||
            contribution.signature.empty() ||
            contribution.signature.size() > maxSignatureBytes)
            return {BeginResult::malformed, std::nullopt};

        std::lock_guard lock(mutex_);
        auto originIt = origins_.find(origin);
        if (originIt == origins_.end())
        {
            if (origins_.size() >= maxTrackedOrigins)
                return {BeginResult::capacity, std::nullopt};
            originIt = origins_.emplace(origin, OriginEntry{}).first;
        }

        auto& originEntry = originIt->second;
        touch(originEntry, currentSeq);
        auto& position = originEntry.positions[contribution.position];
        if (position.conflicted)
            return {BeginResult::conflicted, std::nullopt};
        if (position.unique && sameEncoding(*position.unique, contribution))
            return {BeginResult::duplicate, std::nullopt};

        for (auto const& [_, pending] : position.reservations)
        {
            if (sameEncoding(pending, contribution))
                return {BeginResult::duplicate, std::nullopt};
        }

        auto const distinct = position.reservations.size() +
            static_cast<std::size_t>(position.unique.has_value());
        if (distinct >= 2)
            return {BeginResult::capacity, std::nullopt};

        auto const reservation = nextReservation();
        position.reservations.emplace(reservation, contribution);
        return {
            BeginResult::verify,
            AdmissionTicket{origin, reservation, std::move(contribution)}};
    }

    /** Complete a reserved admission after signature verification. */
    AdmitOutcome
    admitContribution(
        AdmissionTicket ticket,
        bool verified,
        std::uint32_t currentSeq = 0)
    {
        std::lock_guard lock(mutex_);
        auto originIt = origins_.find(ticket.origin_);
        if (originIt == origins_.end())
            return {AdmitResult::stale, std::nullopt};

        auto& originEntry = originIt->second;
        auto positionIt =
            originEntry.positions.find(ticket.contribution_.position);
        if (positionIt == originEntry.positions.end())
            return {AdmitResult::stale, std::nullopt};

        auto& position = positionIt->second;
        auto reservationIt = position.reservations.find(ticket.reservation_);
        if (reservationIt == position.reservations.end() ||
            !(reservationIt->second == ticket.contribution_))
            return {AdmitResult::stale, std::nullopt};
        position.reservations.erase(reservationIt);
        touch(originEntry, currentSeq);

        if (!verified)
            return {AdmitResult::invalid, std::nullopt};
        if (position.conflicted)
            return {AdmitResult::stale, std::nullopt};
        if (!position.unique)
        {
            position.unique = std::move(ticket.contribution_);
            return {AdmitResult::accepted, std::nullopt};
        }
        if (sameEncoding(*position.unique, ticket.contribution_))
            return {AdmitResult::duplicate, std::nullopt};

        auto prior = std::move(position.unique);
        position.unique.reset();
        position.conflicted = true;
        position.reservations.clear();
        return {AdmitResult::conflicted, std::move(prior)};
    }

    /** Reopen per-attempt publication without changing contribution state. */
    bool
    reopenPublication(uint256 const& origin, std::uint32_t currentSeq = 0)
    {
        std::lock_guard lock(mutex_);
        auto it = origins_.find(origin);
        if (it == origins_.end())
        {
            if (origins_.size() >= maxTrackedOrigins)
                return false;
            it = origins_.emplace(origin, OriginEntry{}).first;
        }

        auto& entry = it->second;
        entry.published.clear();
        ++entry.publicationGeneration;
        if (entry.publicationGeneration == 0)
            entry.publicationGeneration = 1;
        touch(entry, currentSeq);
        return true;
    }

    /** Claim one position's publication slot in the current attempt. */
    bool
    claimPublication(
        uint256 const& origin,
        Position position,
        std::size_t maxDistinct)
    {
        std::lock_guard lock(mutex_);
        auto it = origins_.find(origin);
        if (it == origins_.end() || it->second.publicationGeneration == 0)
            return false;
        auto const positionIt = it->second.positions.find(position);
        if (positionIt == it->second.positions.end() ||
            !positionIt->second.unique || positionIt->second.conflicted)
            return false;
        if (it->second.published.count(position) != 0 ||
            it->second.published.size() >= maxDistinct)
            return false;
        it->second.published.insert(position);
        return true;
    }

    PositionStatus
    positionStatus(uint256 const& origin, Position position) const
    {
        std::lock_guard lock(mutex_);
        auto const originIt = origins_.find(origin);
        if (originIt == origins_.end())
            return PositionStatus::empty;
        auto const positionIt = originIt->second.positions.find(position);
        if (positionIt == originIt->second.positions.end())
            return PositionStatus::empty;
        if (positionIt->second.conflicted)
            return PositionStatus::conflicted;
        return positionIt->second.unique ? PositionStatus::unique
                                         : PositionStatus::empty;
    }

    std::uint64_t
    publicationGeneration(uint256 const& origin) const
    {
        std::lock_guard lock(mutex_);
        auto const it = origins_.find(origin);
        return it == origins_.end() ? 0 : it->second.publicationGeneration;
    }

    /** Deterministic full union of every unique, verified contribution. */
    UnionSnapshot
    fullUnionSnapshot() const
    {
        std::lock_guard lock(mutex_);
        UnionSnapshot result;
        for (auto const& [origin, entry] : origins_)
        {
            std::vector<Contribution> contributions;
            for (auto const& [_, position] : entry.positions)
            {
                if (!position.conflicted && position.unique)
                    contributions.push_back(*position.unique);
            }
            if (!contributions.empty())
                result.emplace(origin, std::move(contributions));
        }
        return result;
    }

    void
    clear(uint256 const& origin)
    {
        std::lock_guard lock(mutex_);
        origins_.erase(origin);
    }

    void
    clearAll()
    {
        std::lock_guard lock(mutex_);
        origins_.clear();
    }

    void
    cleanupStale(std::uint32_t currentSeq)
    {
        std::lock_guard lock(mutex_);
        for (auto it = origins_.begin(); it != origins_.end();)
        {
            auto const last = it->second.lastTouchedSeq;
            if (last > 0 && currentSeq > last + maxStaleLedgers)
                it = origins_.erase(it);
            else
                ++it;
        }
    }
};

}  // namespace ripple

#endif
