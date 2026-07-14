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
        unknownOrigin,
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

    class PublicationToken
    {
        friend class ExportSigCollectorV2;

        uint256 origin_;
        std::uint64_t generation_;

        PublicationToken(uint256 const& origin, std::uint64_t generation)
            : origin_(origin), generation_(generation)
        {
        }
    };

    struct AdmitOutcome
    {
        AdmitResult result;
        // Present when this admission observed the second valid encoding.
        std::optional<Contribution> priorContribution;
        std::optional<Contribution> conflictingContribution;
    };

    using UnionSnapshot = std::map<uint256, std::vector<Contribution>>;

    // Conservative local bounds. These are tuning values, not wire-format
    // dimensions, and must be reviewed before feature activation.
    static constexpr std::size_t maxTrackedOrigins = 4096;
    static constexpr std::uint32_t maxStaleLedgers = 256;
    static constexpr std::uint32_t maxReservationLedgers = 1;
    static constexpr std::size_t maxPublicationTriggers = 64;
    static constexpr std::size_t maxSignatureBytes = 72;

private:
    struct Reservation
    {
        Contribution contribution;
        std::uint32_t reservedAtSeq;
    };

    struct PositionEntry
    {
        std::optional<Contribution> unique;
        bool conflicted{false};
        std::map<std::uint64_t, Reservation> reservations;
    };

    struct OriginEntry
    {
        std::map<Position, PositionEntry> positions;
        std::set<Position> published;
        std::set<uint256> publicationTriggers;
        uint256 publicationTrigger;
        std::uint64_t publicationGeneration{0};
        std::uint32_t lastTouchedSeq{0};
    };

    mutable std::mutex mutex_;
    std::unordered_map<uint256, OriginEntry> origins_;
    std::uint64_t nextReservation_{1};
    std::uint64_t nextPublicationGeneration_{1};

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

    static void
    pruneReservations(PositionEntry& position, std::uint32_t currentSeq)
    {
        for (auto it = position.reservations.begin();
             it != position.reservations.end();)
        {
            auto const reserved = it->second.reservedAtSeq;
            if (currentSeq > reserved &&
                currentSeq - reserved > maxReservationLedgers)
                it = position.reservations.erase(it);
            else
                ++it;
        }
    }

    static void
    eraseEmptyPosition(OriginEntry& origin, Position position)
    {
        auto const it = origin.positions.find(position);
        if (it != origin.positions.end() && !it->second.unique &&
            !it->second.conflicted && it->second.reservations.empty())
            origin.positions.erase(it);
    }

    std::uint64_t
    nextReservation()
    {
        auto const result = nextReservation_++;
        if (nextReservation_ == 0)
            nextReservation_ = 1;
        return result;
    }

    std::uint64_t
    nextPublicationGeneration()
    {
        auto const result = nextPublicationGeneration_++;
        if (nextPublicationGeneration_ == 0)
            nextPublicationGeneration_ = 1;
        return result;
    }

public:
    /** Reserve one contribution encoding for verification.

        The caller must first attribute the signing key to this exact selected
        universe position using the validated latch and current manifest. The
        reservation is then the cryptographic pre-verification DoS boundary.
        At most two
        distinct encodings can be in-flight or admitted at a position. The
        caller performs cryptographic verification only for `verify`, then
        returns the ticket through `admitContribution`.
    */
    Admission
    beginAttributedAdmission(
        uint256 const& origin,
        Contribution contribution,
        std::uint32_t currentSeq = 0)
    {
        if (origin.isZero() || currentSeq == 0 ||
            contribution.position >=
                ExportLimits::maxValidatorUniverseMembers ||
            contribution.signature.empty() ||
            contribution.signature.size() > maxSignatureBytes)
            return {BeginResult::malformed, std::nullopt};

        std::lock_guard lock(mutex_);
        auto originIt = origins_.find(origin);
        if (originIt == origins_.end())
            return {BeginResult::unknownOrigin, std::nullopt};

        auto& originEntry = originIt->second;
        auto& position = originEntry.positions[contribution.position];
        pruneReservations(position, currentSeq);
        if (position.conflicted)
            return {BeginResult::conflicted, std::nullopt};
        if (position.unique && sameEncoding(*position.unique, contribution))
            return {BeginResult::duplicate, std::nullopt};

        for (auto const& [_, pending] : position.reservations)
        {
            if (sameEncoding(pending.contribution, contribution))
                return {BeginResult::duplicate, std::nullopt};
        }

        auto const distinct = position.reservations.size() +
            static_cast<std::size_t>(position.unique.has_value());
        if (distinct >= 2)
            return {BeginResult::capacity, std::nullopt};

        auto const reservation = nextReservation();
        position.reservations.emplace(
            reservation, Reservation{contribution, currentSeq});
        return {
            BeginResult::verify,
            AdmissionTicket{origin, reservation, std::move(contribution)}};
    }

    /** Cancel a reservation when verification cannot be completed. */
    bool
    cancelAdmission(AdmissionTicket ticket)
    {
        std::lock_guard lock(mutex_);
        auto originIt = origins_.find(ticket.origin_);
        if (originIt == origins_.end())
            return false;
        auto positionIt =
            originIt->second.positions.find(ticket.contribution_.position);
        if (positionIt == originIt->second.positions.end())
            return false;
        auto reservationIt =
            positionIt->second.reservations.find(ticket.reservation_);
        if (reservationIt == positionIt->second.reservations.end() ||
            !(reservationIt->second.contribution == ticket.contribution_))
            return false;
        positionIt->second.reservations.erase(reservationIt);
        eraseEmptyPosition(originIt->second, ticket.contribution_.position);
        return true;
    }

    /** Complete a reserved admission after signature verification. */
    AdmitOutcome
    admitContribution(
        AdmissionTicket ticket,
        bool signatureVerified,
        std::uint32_t currentSeq = 0)
    {
        std::lock_guard lock(mutex_);
        auto originIt = origins_.find(ticket.origin_);
        if (originIt == origins_.end())
            return {AdmitResult::stale, std::nullopt, std::nullopt};

        auto& originEntry = originIt->second;
        auto positionIt =
            originEntry.positions.find(ticket.contribution_.position);
        if (positionIt == originEntry.positions.end())
            return {AdmitResult::stale, std::nullopt, std::nullopt};

        auto& position = positionIt->second;
        auto reservationIt = position.reservations.find(ticket.reservation_);
        if (reservationIt == position.reservations.end() ||
            !(reservationIt->second.contribution == ticket.contribution_))
            return {AdmitResult::stale, std::nullopt, std::nullopt};
        position.reservations.erase(reservationIt);

        if (!signatureVerified)
        {
            eraseEmptyPosition(originEntry, ticket.contribution_.position);
            return {AdmitResult::invalid, std::nullopt, std::nullopt};
        }
        if (position.conflicted)
            return {AdmitResult::stale, std::nullopt, std::nullopt};
        if (!position.unique)
        {
            position.unique = std::move(ticket.contribution_);
            touch(originEntry, currentSeq);
            return {AdmitResult::accepted, std::nullopt, std::nullopt};
        }
        if (sameEncoding(*position.unique, ticket.contribution_))
            return {AdmitResult::duplicate, std::nullopt, std::nullopt};

        auto prior = std::move(position.unique);
        auto conflicting = std::move(ticket.contribution_);
        position.unique.reset();
        position.conflicted = true;
        position.reservations.clear();
        touch(originEntry, currentSeq);
        return {
            AdmitResult::conflicted, std::move(prior), std::move(conflicting)};
    }

    /** Reopen per-attempt publication without changing contribution state. */
    std::optional<PublicationToken>
    reopenPublication(
        uint256 const& origin,
        uint256 const& trigger,
        std::uint32_t currentSeq)
    {
        if (origin.isZero() || trigger.isZero() || currentSeq == 0)
            return std::nullopt;

        std::lock_guard lock(mutex_);
        auto it = origins_.find(origin);
        if (it == origins_.end())
        {
            if (origins_.size() >= maxTrackedOrigins)
                return std::nullopt;
            it = origins_.emplace(origin, OriginEntry{}).first;
        }

        auto& entry = it->second;
        if (entry.publicationGeneration != 0 &&
            entry.publicationTrigger == trigger)
            return PublicationToken{origin, entry.publicationGeneration};
        if (entry.publicationTriggers.count(trigger) != 0 ||
            entry.publicationTriggers.size() >= maxPublicationTriggers)
            return std::nullopt;

        entry.published.clear();
        entry.publicationTrigger = trigger;
        entry.publicationTriggers.insert(trigger);
        entry.publicationGeneration = nextPublicationGeneration();
        touch(entry, currentSeq);
        return PublicationToken{origin, entry.publicationGeneration};
    }

    /** Claim one position's publication slot in the current attempt. */
    bool
    claimPublication(
        PublicationToken const& token,
        Position position,
        std::size_t maxDistinct)
    {
        std::lock_guard lock(mutex_);
        auto it = origins_.find(token.origin_);
        if (it == origins_.end() ||
            it->second.publicationGeneration != token.generation_)
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
            if (last > 0 && currentSeq > last &&
                currentSeq - last > maxStaleLedgers)
                it = origins_.erase(it);
            else
                ++it;
        }
    }
};

}  // namespace ripple

#endif
