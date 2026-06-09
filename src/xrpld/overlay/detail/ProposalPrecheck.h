#ifndef RIPPLE_OVERLAY_DETAIL_PROPOSALPRECHECK_H_INCLUDED
#define RIPPLE_OVERLAY_DETAIL_PROPOSALPRECHECK_H_INCLUDED

#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/messages.h>

#include <optional>

namespace ripple {
namespace detail {

enum class ProposalPrecheckResult {
    ok,
    badHashes,
    badPosition,
    entropyDisabled,
    exportDisabled,
    tooManyExportSignatures,
    unsignedExportSignatures,
    exportSignaturesHashMismatch,
    missingExportSignatures
};

struct ProposalPrecheck
{
    ProposalPrecheckResult result = ProposalPrecheckResult::ok;
    std::optional<ExtendedPosition> position;
};

struct ProposalPrecheckRejection
{
    char const* logMessage;
    char const* feeReason;
};

inline std::optional<ProposalPrecheckRejection>
proposalPrecheckRejection(ProposalPrecheckResult result)
{
    switch (result)
    {
        case ProposalPrecheckResult::ok:
            return std::nullopt;
        case ProposalPrecheckResult::badHashes:
            return ProposalPrecheckRejection{
                "Proposal: malformed", "bad hashes"};
        case ProposalPrecheckResult::badPosition:
            return ProposalPrecheckRejection{
                "Proposal: malformed extended position",
                "bad proposal position"};
        case ProposalPrecheckResult::entropyDisabled:
            return ProposalPrecheckRejection{
                "Proposal: entropy fields while featureConsensusEntropy disabled",
                "entropy fields disabled"};
        case ProposalPrecheckResult::exportDisabled:
            return ProposalPrecheckRejection{
                "Proposal: export fields while featureExport disabled",
                "export fields disabled"};
        case ProposalPrecheckResult::tooManyExportSignatures:
            return ProposalPrecheckRejection{
                "Proposal: too many export signatures", "too many export sigs"};
        case ProposalPrecheckResult::unsignedExportSignatures:
            return ProposalPrecheckRejection{
                "Proposal: unsigned export signatures", "unsigned export sigs"};
        case ProposalPrecheckResult::exportSignaturesHashMismatch:
            return ProposalPrecheckRejection{
                "Proposal: export signatures hash mismatch",
                "export sig hash mismatch"};
        case ProposalPrecheckResult::missingExportSignatures:
            return ProposalPrecheckRejection{
                "Proposal: missing signed export signatures",
                "missing export sigs"};
    }
    return std::nullopt;
}

inline ProposalPrecheck
checkProposalExtensions(
    protocol::TMProposeSet const& set,
    bool entropyEnabled,
    bool exportEnabled)
{
    if (set.currenttxhash().size() < uint256::size() ||
        set.previousledger().size() != uint256::size())
    {
        return {ProposalPrecheckResult::badHashes, std::nullopt};
    }

    auto const currentPosSlice = makeSlice(set.currenttxhash());
    SerialIter currentPosSit{currentPosSlice};
    auto const parsedPosition = ExtendedPosition::fromSerialIter(
        currentPosSit, set.currenttxhash().size());
    if (!parsedPosition)
        return {ProposalPrecheckResult::badPosition, std::nullopt};

    bool const hasEntropyMaterial = parsedPosition->commitSetHash ||
        parsedPosition->entropySetHash || parsedPosition->myCommitment ||
        parsedPosition->myReveal;
    bool const hasExportMaterial = parsedPosition->exportSigSetHash ||
        parsedPosition->exportSignaturesHash || set.exportsignatures_size() > 0;
    if (hasEntropyMaterial && !entropyEnabled)
        return {ProposalPrecheckResult::entropyDisabled, parsedPosition};
    if (hasExportMaterial && !exportEnabled)
        return {ProposalPrecheckResult::exportDisabled, parsedPosition};

    if (set.exportsignatures_size() > ExportLimits::maxPendingExports)
        return {
            ProposalPrecheckResult::tooManyExportSignatures, parsedPosition};

    if (set.exportsignatures_size() > 0)
    {
        if (!parsedPosition->exportSignaturesHash)
            return {
                ProposalPrecheckResult::unsignedExportSignatures,
                parsedPosition};

        if (proposalExportSignaturesHash(set.exportsignatures()) !=
            *parsedPosition->exportSignaturesHash)
        {
            return {
                ProposalPrecheckResult::exportSignaturesHashMismatch,
                parsedPosition};
        }
    }
    else if (parsedPosition->exportSignaturesHash)
    {
        return {
            ProposalPrecheckResult::missingExportSignatures, parsedPosition};
    }

    return {ProposalPrecheckResult::ok, parsedPosition};
}

}  // namespace detail
}  // namespace ripple

#endif
