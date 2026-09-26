#ifndef RIPPLE_PROTOCOL_EXPORTCOMMITTEE_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORTCOMMITTEE_H_INCLUDED

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/UintTypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ripple {

/** One canonical, network-neutral Export validator committee. */
struct ExportCommitteeProfile
{
    std::vector<PublicKey> members;
    std::size_t quorum;

    std::optional<std::uint16_t>
    position(PublicKey const& master) const
    {
        auto const it =
            std::lower_bound(members.begin(), members.end(), master);
        if (it == members.end() || *it != master)
            return std::nullopt;
        return static_cast<std::uint16_t>(std::distance(members.begin(), it));
    }
};

/** Parse a strictly sorted sequence of compressed validator master keys. */
inline std::optional<ExportCommitteeProfile>
resolveExportCommittee(Slice roster)
{
    constexpr std::size_t publicKeyBytes = 33;
    if (roster.empty() || roster.size() % publicKeyBytes != 0)
        return std::nullopt;

    auto const count = roster.size() / publicKeyBytes;
    if (count == 0 || count > ExportLimits::maxCommitteeMembers)
        return std::nullopt;

    std::vector<PublicKey> members;
    members.reserve(count);
    for (std::size_t offset = 0; offset < roster.size();
         offset += publicKeyBytes)
    {
        Slice const encoded{roster.data() + offset, publicKeyBytes};
        if (!publicKeyType(encoded))
            return std::nullopt;

        PublicKey member{encoded};
        if (!members.empty() && !(members.back() < member))
            return std::nullopt;
        members.push_back(std::move(member));
    }

    return ExportCommitteeProfile{
        std::move(members), ExportLimits::committeeQuorumThreshold(count)};
}

inline Blob
serializeExportCommittee(std::vector<PublicKey> members)
{
    if (members.empty() || members.size() > ExportLimits::maxCommitteeMembers)
        return {};

    std::sort(members.begin(), members.end());
    if (std::adjacent_find(members.begin(), members.end()) != members.end())
        return {};

    Blob roster;
    roster.reserve(members.size() * 33);
    for (auto const& member : members)
        roster.insert(roster.end(), member.begin(), member.end());
    return roster;
}

/** Canonicalize an unordered transaction roster into strict stored form. */
inline std::optional<Blob>
canonicalizeExportCommittee(Slice roster)
{
    constexpr std::size_t publicKeyBytes = 33;
    if (roster.empty() || roster.size() % publicKeyBytes != 0)
        return std::nullopt;

    auto const count = roster.size() / publicKeyBytes;
    if (count == 0 || count > ExportLimits::maxCommitteeMembers)
        return std::nullopt;

    std::vector<PublicKey> members;
    members.reserve(count);
    for (std::size_t offset = 0; offset < roster.size();
         offset += publicKeyBytes)
    {
        Slice const encoded{roster.data() + offset, publicKeyBytes};
        if (!publicKeyType(encoded))
            return std::nullopt;
        members.emplace_back(encoded);
    }

    auto canonical = serializeExportCommittee(std::move(members));
    if (canonical.empty())
        return std::nullopt;
    return canonical;
}

/** Content identity of one already-validated canonical roster. */
inline uint256
exportCommitteeHash(Slice roster)
{
    auto const profile = resolveExportCommittee(roster);
    if (!profile)
        return {};

    Serializer serialized;
    serialized.add32(HashPrefix::exportCommittee);
    serialized.add32(static_cast<std::uint32_t>(profile->members.size()));
    serialized.addRaw(roster);
    return serialized.getSHA512Half();
}

}  // namespace ripple

#endif
