#ifndef RIPPLE_OVERLAY_DETAIL_PROPOSALPRECHECK_H_INCLUDED
#define RIPPLE_OVERLAY_DETAIL_PROPOSALPRECHECK_H_INCLUDED

#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/messages.h>

#include <optional>

namespace ripple {
namespace detail {

enum class ProposalPrecheckResult {
    ok,
    badHashes,
    badPosition,
    extensionDiagnosticsDisabled,
    entropyDisabled
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

inline bool
proposalSignatureAccepted(bool /* clusterPeer */, bool signatureValid)
{
    // Cluster trust affects relay and charging, never validator attribution.
    return signatureValid;
}

inline bool
proposalHasMalformedHashes(protocol::TMProposeSet const& set)
{
    // `currenttxhash` is a legacy protobuf field that may now carry serialized
    // ExtendedPosition bytes. It must contain at least the base tx-set hash;
    // full position validation happens in ExtendedPosition::fromSerialIter().
    return set.currenttxhash().size() < uint256::size() ||
        set.previousledger().size() != uint256::size();
}

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
        case ProposalPrecheckResult::extensionDiagnosticsDisabled:
            return ProposalPrecheckRejection{
                "Proposal: extension diagnostics while consensus extensions "
                "disabled",
                "extension diagnostics disabled"};
        case ProposalPrecheckResult::entropyDisabled:
            return ProposalPrecheckRejection{
                "Proposal: entropy fields while featureConsensusEntropy "
                "disabled",
                "entropy fields disabled"};
    }
    return std::nullopt;
}

template <class IsEntropyEnabled>
inline ProposalPrecheck
checkProposalExtensions(
    protocol::TMProposeSet const& set,
    IsEntropyEnabled isEntropyEnabled)
{
    //@@start proposal-extension-precheck
    if (proposalHasMalformedHashes(set))
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
    bool const hasExtensionDiagnostics =
        parsedPosition->observedParticipantsHash.has_value();
    if (hasEntropyMaterial && !isEntropyEnabled())
        return {ProposalPrecheckResult::entropyDisabled, parsedPosition};
    if (hasExtensionDiagnostics && !isEntropyEnabled())
        return {
            ProposalPrecheckResult::extensionDiagnosticsDisabled,
            parsedPosition};

    return {ProposalPrecheckResult::ok, parsedPosition};
    //@@end proposal-extension-precheck
}

inline ProposalPrecheck
checkProposalExtensions(protocol::TMProposeSet const& set, bool entropyEnabled)
{
    return checkProposalExtensions(
        set, [entropyEnabled] { return entropyEnabled; });
}

}  // namespace detail
}  // namespace ripple

#endif
