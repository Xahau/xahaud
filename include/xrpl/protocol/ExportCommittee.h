#ifndef RIPPLE_PROTOCOL_EXPORTCOMMITTEE_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORTCOMMITTEE_H_INCLUDED

#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ValidatorBitset.h>

#include <cstddef>
#include <optional>
#include <utility>

namespace ripple {

/** Validated v1 Export committee profile for one pinned validator universe. */
struct ExportCommitteeProfile
{
    ValidatedValidatorBitset members;
    std::size_t quorum;
};

/** Resolve the fixed v1 profile from its canonical LSB-first bitmap.

    Callers bind `universeSize` to the immediate parent ledger's canonical
    pre-NegativeUNL UNLReport view. The intent selects members only; v1 fixes
    quorum policy at ceil(0.8 * selected members).
*/
inline std::optional<ExportCommitteeProfile>
resolveExportCommittee(Slice bitmap, std::size_t universeSize)
{
    if (universeSize == 0 ||
        universeSize > ExportLimits::maxValidatorUniverseMembers)
        return std::nullopt;

    auto members = validateValidatorBitset(bitmap, universeSize);
    if (!members || members->selected() == 0 ||
        members->selected() > ExportLimits::maxCommitteeMembers)
        return std::nullopt;

    auto const quorum =
        ExportLimits::committeeQuorumThreshold(members->selected());
    return ExportCommitteeProfile{std::move(*members), quorum};
}

}  // namespace ripple

#endif
