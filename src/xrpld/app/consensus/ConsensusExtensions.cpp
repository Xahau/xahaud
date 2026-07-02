//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/consensus/ExportSignatureHarvester.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/AmendmentTable.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/consensus/Consensus.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/random.h>
#include <xrpl/crypto/csprng.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SidecarType.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <random>

namespace ripple {

ConsensusExtensions::ConsensusExtensions(Application& app, beast::Journal j)
    : app_(app), j_(j)
{
}

//------------------------------------------------------------------------------
// RNG Helper Methods

namespace {

auto
activeSignerFilter(
    ConsensusExtensions const& extensions,
    ConsensusExtensions::ActiveValidatorViewPtr validatorView)
{
    auto const* ext = &extensions;
    return [ext, validatorView](PublicKey const& key) {
        return ext->isActiveValidator(key, *validatorView);
    };
}

std::string
buildObservedParticipantBitmap(
    std::vector<NodeID> const& activeSorted,
    hash_set<NodeID> const& observed)
{
    std::string bitmapBin;
    bitmapBin.reserve(activeSorted.size());

    for (std::size_t i = 0; i < activeSorted.size(); ++i)
    {
        bool const seen = observed.count(activeSorted[i]) != 0;
        bitmapBin.push_back(seen ? '1' : '0');
    }

    return bitmapBin;
}

uint256
entropyCommitment(
    uint256 const& reveal,
    PublicKey const& validatorKey,
    LedgerIndex seq)
{
    return sha512Half(reveal, validatorKey, seq);
}

bool
verifyProposalDigest(
    PublicKey const& publicKey,
    uint256 const& signingHash,
    Slice const& signature)
{
    // Proposal/validation signatures are secp256k1 digest signatures today.
    // Proposal-carried sidecars can still name a valid ed25519 manifest/master
    // key, so reject non-secp keys before verifyDigest, which aborts on them.
    auto const type = publicKeyType(publicKey);
    return type && *type == KeyType::secp256k1 &&
        verifyDigest(publicKey, signingHash, signature);
}

struct AdmittedSidecarLeaf
{
    STObject sidecar;
    std::uint8_t type;
};

std::optional<AdmittedSidecarLeaf>
admitSidecarLeaf(
    uint256 const& itemKey,
    Slice const& entry,
    uint256 const& setHash,
    beast::Journal j,
    char const* owner,
    char const* kind,
    char const* source,
    std::optional<std::size_t> maxEntryBytes = std::nullopt,
    bool* malformed = nullptr)
{
    if (maxEntryBytes && entry.size() > *maxEntryBytes)
    {
        if (malformed)
            *malformed = true;
        JLOG(j.warn()) << owner << ": rejecting sidecar entry"
                       << " reason=entry-too-large"
                       << " kind=" << kind << " source=" << source
                       << " setHash=" << setHash << " itemKey=" << itemKey
                       << " entryBytes=" << entry.size()
                       << " maxEntryBytes=" << *maxEntryBytes;
        return std::nullopt;
    }

    SerialIter sit(entry);
    STObject sidecar(sit, sfGeneric);

    if (!sidecar.isFieldPresent(sfSidecarType))
        return std::nullopt;

    auto const sidecarHash = sidecar.getHash(HashPrefix::sidecar);
    if (sidecarHash != itemKey)
    {
        if (malformed)
            *malformed = true;
        JLOG(j.warn()) << owner << ": rejecting sidecar entry"
                       << " reason=item-key-mismatch"
                       << " kind=" << kind << " source=" << source
                       << " setHash=" << setHash << " itemKey=" << itemKey
                       << " sidecarHash=" << sidecarHash;
        return std::nullopt;
    }

    auto const type = sidecar.getFieldU8(sfSidecarType);
    return AdmittedSidecarLeaf{std::move(sidecar), type};
}

boost::intrusive_ptr<SHAMapItem>
makeSidecarItem(STObject const& sidecar)
{
    Serializer s;
    sidecar.add(s);
    auto const itemKey = sidecar.getHash(HashPrefix::sidecar);
    XRPL_ASSERT(
        itemKey == sha512Half(HashPrefix::sidecar, s.slice()),
        "ripple::makeSidecarItem : sidecar hash matches serialized bytes");
    return make_shamapitem(itemKey, s.slice());
}

//@@start active-validator-view-build
ActiveValidatorViewSource
buildActiveValidatorViewSource(
    std::shared_ptr<Ledger const> const& sourceLedger)
{
    ActiveValidatorViewSource source;

    if (!sourceLedger)
        return source;

    source.sourceLedgerHash = sourceLedger->info().hash;

    if (auto const sle = sourceLedger->read(keylet::UNLReport()))
    {
        if (sle->isFieldPresent(sfActiveValidators))
        {
            hash_set<PublicKey> reportKeys;
            for (auto const& obj : sle->getFieldArray(sfActiveValidators))
            {
                auto const pk = obj.getFieldVL(sfPublicKey);
                if (!publicKeyType(makeSlice(pk)))
                    continue;

                reportKeys.insert(PublicKey{makeSlice(pk)});
            }

            if (!reportKeys.empty())
                source.unlReportMasterKeys = std::move(reportKeys);
        }
    }

    // UNLReport records recently active validators; NegativeUNL is the separate
    // ledger policy overlay that core consensus applies to quorum.
    source.negativeUNLEnabled =
        sourceLedger->rules().enabled(featureNegativeUNL);
    if (source.negativeUNLEnabled)
        source.negativeUNL = sourceLedger->negativeUNL();

    return source;
}

ActiveValidatorViewFallback
buildActiveValidatorViewFallback(Application& app)
{
    ActiveValidatorViewFallback fallback;

    // Fallback exists for early ledgers and dev/test networks before the report
    // object is available. It is deliberately the configured trusted master-key
    // set so manifest signing keys still resolve through trust.
    fallback.trustedMasterKeys = app.validators().getTrustedMasterKeys();

    // Some standalone/dev configurations trust local validation implicitly. The
    // builder dedupes this if self is already in the trusted set.
    auto const& valKeys = app.getValidatorKeys();
    if (valKeys.keys && valKeys.nodeID != beast::zero)
        fallback.localMasterKey = valKeys.keys->masterPublicKey;

    return fallback;
}
//@@end active-validator-view-build

bool
addExportSidecarCandidate(
    ExportTxnLookup& exportTxns,
    std::shared_ptr<STTx const> stx)
{
    // Export signature sidecars are bounded by maxPendingExports. Overflow
    // export-work txns may still be present in the base consensus tx set; they
    // are just not sidecar-signature candidates for this round. Cancel-only
    // ttEXPORTs are not export work and must not consume scarce signing slots.
    if (exportTxns.size() >= ExportLimits::maxPendingExports)
        return false;

    exportTxns.emplace(stx->getTransactionID(), std::move(stx));
    return true;
}

ExportTxnLookup
buildExportTxnLookup(SHAMap const& txns, beast::Journal j)
{
    ExportTxnLookup exportTxns;
    txns.visitLeaves([&](boost::intrusive_ptr<SHAMapItem const> const& item) {
        try
        {
            SerialIter sit(item->slice());
            auto stx = std::make_shared<STTx const>(sit);
            if (ExportLedgerOps::isPendingExportWorkTxn(*stx))
                addExportSidecarCandidate(exportTxns, std::move(stx));
        }
        catch (std::exception const& e)
        {
            JLOG(j.warn()) << "Export: failed to parse candidate tx"
                           << " itemKey=" << item->key()
                           << " context=build-export-lookup"
                           << " error=" << e.what();
        }
    });
    return exportTxns;
}

ExportTxnLookup
buildOpenLedgerExportTxnLookup(Application& app)
{
    ExportTxnLookup exportTxns;
    auto const openLedger = app.openLedger().current();
    if (!openLedger)
        return exportTxns;

    for (auto const& entry : openLedger->txs)
    {
        auto const& stx = entry.first;
        if (stx && ExportLedgerOps::isPendingExportWorkTxn(*stx))
            addExportSidecarCandidate(exportTxns, stx);
    }
    return exportTxns;
}

LedgerIndex
currentClosedLedgerSeq(Application& app)
{
    if (auto const closed = app.getLedgerMaster().getClosedLedger())
        return closed->info().seq;
    return 0;
}

}  // namespace

std::size_t
ConsensusExtensions::quorumThreshold() const
{
    // Validator_quorum entropy uses a fixed 80% threshold over the effective
    // active UNL snapshot. Tier 2 participant_aligned entropy has its own
    // lower intersection-safe floor; recent proposers are useful for liveness
    // heuristics, but they do not lower either threshold.
    // Use the shared validator view so Tier 3 RNG and Export use the same
    // denominator.
    auto const base = activeValidatorView()->size();
    return safeQuorumThreshold(base);
}

std::size_t
ConsensusExtensions::exportSigQuorumThreshold() const
{
    return exportSigQuorumThreshold(*activeValidatorView());
}

std::size_t
ConsensusExtensions::exportSigQuorumThreshold(
    ActiveValidatorView const& validatorView)
{
    auto const base = validatorView.size();

    // Export sidecar hashes are signed through ExtendedPosition even when RNG
    // is disabled, so a quorum-aligned exportSigSetHash is deterministic
    // enough for Export-only mode. Unanimity would let one active validator
    // veto an otherwise converged export round.
    return safeQuorumThreshold(base);
}

std::size_t
ConsensusExtensions::tier2Threshold() const
{
    // Tier 2 (participant_aligned) lowers the alignment bar from the 80%
    // validator-quorum gate to the quorum-intersection floor over the ORIGINAL
    // (pre-nUNL) view (~60%; exact value from calculateParticipantThreshold,
    // which keeps two aligned cohorts sharing an honest validator so an
    // equivocator cannot mint two distinct digests). Anchored to
    // originalViewSize, not size(): nUNL can shrink the effective view while
    // leaving faulty nodes in it, so a fraction of the effective view could
    // exceed the Byzantine fraction (which is bounded over the original UNL).
    // Regressing this to size() is a consensus fork under nUNL and is pinned by
    // ConsensusExtensions_test::testTier2ThresholdAnchorsToOriginalView.
    auto const base = activeValidatorView()->originalViewSize;
    return safeParticipantThreshold(base);
}

std::size_t
ConsensusExtensions::entropyGateThreshold() const
{
    // The bar at which the commit/reveal/entropy pipeline engages and the
    // entropy conflict gate resolves: the lowest ENABLED accepted tier's
    // threshold. In the normal band tier2Threshold (~0.6*original) < quorum
    // (0.8*effective), so this is the participant-alignment floor and
    // sub-quorum rounds reach injection. Under nUNL the exact integer
    // thresholds can cross either way; that only changes which threshold lets
    // the pipeline proceed. Final tier labels are computed later from the
    // agreed entropy set count in selectEntropy(). Non-fallback tier labels are
    // allowed only when the round view is anchored by UNLReport; the
    // trusted-fallback view is local configuration and selectEntropy() maps it
    // to consensus_fallback.
    auto const view = activeValidatorView();
    return entropyGateThresholdForView(view->size(), view->originalViewSize);
}

std::size_t
ConsensusExtensions::entropyGateThresholdForView(
    std::size_t effectiveViewSize,
    std::size_t originalViewSize)
{
    auto const quorum = safeQuorumThreshold(effectiveViewSize);
    auto const tier2 = safeParticipantThreshold(originalViewSize);
    return std::min(quorum, tier2);
}

EntropyTier
ConsensusExtensions::selectEntropyTierForView(
    bool fromUNLReport,
    std::size_t participantCount,
    std::size_t effectiveViewSize,
    std::size_t originalViewSize)
{
    if (!fromUNLReport)
        return entropyTierConsensusFallback;

    auto const quorum = safeQuorumThreshold(effectiveViewSize);
    if (participantCount >= quorum)
        return entropyTierValidatorQuorum;

    auto const tier2 = safeParticipantThreshold(originalViewSize);
    if (participantCount >= tier2)
        return entropyTierParticipantAligned;

    return entropyTierConsensusFallback;
}

void
ConsensusExtensions::setExpectedProposers(hash_set<NodeID> proposers)
{
    bool const includeSelf = mode_ == ConsensusMode::proposing &&
        app_.getValidatorKeys().keys &&
        app_.getValidatorKeys().nodeID != beast::zero;

    if (!proposers.empty())
    {
        // Intersect recent proposers with the active UNL. This set is used as
        // a liveness hint only; commit quorum itself remains fixed to the
        // active UNL snapshot for the round.
        auto const validatorView = activeValidatorView();
        hash_set<NodeID> filtered;
        for (auto const& id : proposers)
        {
            if (!includeSelf && id == app_.getValidatorKeys().nodeID)
                continue;
            // Recent proposers are only a liveness hint; filter them through
            // the same active view that defines commit quorum membership.
            if (validatorView->containsNode(id))
                filtered.insert(id);
        }
        if (includeSelf)
            filtered.insert(app_.getValidatorKeys().nodeID);
        likelyParticipants_ = std::move(filtered);
        JLOG(j_.trace()) << "RNG: likelyParticipants"
                         << " source=recent-proposers"
                         << " count=" << likelyParticipants_.size()
                         << " input=" << proposers.size()
                         << " includeSelf=" << (includeSelf ? "yes" : "no")
                         << " activeValidators=" << validatorView->size();
        return;
    }

    // First round (or no recent data): fall back to the active UNL snapshot as
    // our best guess for who may still contribute before timeout.
    auto const validatorView = activeValidatorView();
    if (validatorView->size() > 0)
    {
        likelyParticipants_ = validatorView->nodeIds;
        JLOG(j_.trace()) << "RNG: likelyParticipants"
                         << " source=active-validator-view"
                         << " count=" << likelyParticipants_.size()
                         << " activeValidators=" << validatorView->size()
                         << " viewSource="
                         << (validatorView->fromUNLReport ? "UNLReport"
                                                          : "trusted-fallback");
        return;
    }

    // No data at all (shouldn't happen — cacheUNLReport falls back to
    // trusted keys). Leave empty; diagnostics will show no liveness hint.
    JLOG(j_.warn()) << "RNG: likelyParticipants unavailable"
                    << " reason=empty-active-validator-view";
}

std::size_t
ConsensusExtensions::pendingCommitCount() const
{
    return pendingCommits_.size();
}

bool
ConsensusExtensions::hasProofedCommit(NodeID const& nodeId) const
{
    return pendingCommits_.count(nodeId) > 0 && commitProofs_.count(nodeId) > 0;
}

bool
ConsensusExtensions::hasActiveProofedCommit(
    NodeID const& nodeId,
    ActiveValidatorView const& validatorView) const
{
    return validatorView.containsNode(nodeId) &&
        nodeIdToKey_.count(nodeId) > 0 && hasProofedCommit(nodeId);
}

std::size_t
ConsensusExtensions::proofedCommitCount() const
{
    auto const validatorView = activeValidatorView();
    return std::count_if(
        pendingCommits_.begin(),
        pendingCommits_.end(),
        [this, validatorView](auto const& entry) {
            auto const& nid = entry.first;
            // Match buildCommitSet(): only commits that can be emitted as
            // verifiable sidecar leaves count toward any commit threshold.
            return hasActiveProofedCommit(nid, *validatorView);
        });
}

std::size_t
ConsensusExtensions::pendingRevealCount() const
{
    return pendingReveals_.size();
}

std::size_t
ConsensusExtensions::proofedRevealCount() const
{
    auto const validatorView = activeValidatorView();
    return std::count_if(
        pendingReveals_.begin(),
        pendingReveals_.end(),
        [this, validatorView](auto const& entry) {
            auto const& nid = entry.first;
            return hasActiveProofedCommit(nid, *validatorView);
        });
}

bool
ConsensusExtensions::ingestRngContribution(
    NodeID const& nodeId,
    PublicKey const& publicKey,
    RngContributionKind kind,
    uint256 const& digest,
    std::optional<LedgerIndex> seq,
    std::optional<ProposalProof> const& proof,
    char const* sourceTag,
    RngProofCachePolicy proofCachePolicy)
{
    bool const isCommit = kind == RngContributionKind::commit;
    char const* kindName = isCommit ? "commit" : "reveal";

    if (!isUNLReportMember(nodeId))
    {
        JLOG(j_.trace()) << "RNG: rejecting contribution"
                         << " reason=non-active-validator"
                         << " kind=" << kindName << " source=" << sourceTag
                         << " node=" << nodeId;
        return false;
    }

    //@@start rng-contribution-identity-gate
    auto const directMaster = calcNodeID(publicKey) == nodeId;
    auto const trustedMaster = directMaster
        ? std::optional<PublicKey>{publicKey}
        : app_.validators().getTrustedKey(publicKey);
    if (!trustedMaster || calcNodeID(*trustedMaster) != nodeId)
    {
        JLOG(j_.warn()) << "RNG: rejecting contribution"
                        << " reason=node-key-mismatch"
                        << " kind=" << kindName << " source=" << sourceTag
                        << " node=" << nodeId;
        return false;
    }
    //@@end rng-contribution-identity-gate

    if (isCommit)
    {
        auto const existing = pendingCommits_.find(nodeId);
        bool const hasSeq0Proof = proof && proof->proposeSeq == 0;
        if (commitSetFrozen_)
        {
            if (existing != pendingCommits_.end() && existing->second == digest)
            {
                JLOG(j_.trace())
                    << "RNG: ignoring duplicate commitment after freeze"
                    << " source=" << sourceTag << " node=" << nodeId
                    << " commit=" << digest;
            }
            else
            {
                JLOG(j_.warn())
                    << "RNG: rejecting changed commitment after freeze"
                    << " source=" << sourceTag << " node=" << nodeId
                    << " new=" << digest << " existing="
                    << (existing != pendingCommits_.end()
                            ? to_string(existing->second)
                            : std::string{"none"});
            }
            return false;
        }

        if (existing != pendingCommits_.end() && existing->second != digest)
        {
            if (hasProofedCommit(nodeId) &&
                proofCachePolicy == RngProofCachePolicy::keepExisting)
            {
                JLOG(j_.warn())
                    << "RNG: ignoring changed commitment"
                    << " reason=existing-proofed-commit"
                    << " source=" << sourceTag << " node=" << nodeId
                    << " old=" << existing->second << " new=" << digest;
                return false;
            }

            JLOG(j_.warn()) << "RNG: replacing changed commitment"
                            << " source=" << sourceTag << " node=" << nodeId
                            << " old=" << existing->second << " new=" << digest
                            << " proofed=" << (hasSeq0Proof ? "yes" : "no");

            existing->second = digest;
            pendingReveals_.erase(nodeId);
            if (!hasSeq0Proof)
                commitProofs_.erase(nodeId);
        }
        else
        {
            pendingCommits_.emplace(nodeId, digest);
        }

        nodeIdToKey_.insert_or_assign(nodeId, publicKey);

        if (proof)
        {
            if (proof->proposeSeq == 0)
            {
                if (proofCachePolicy == RngProofCachePolicy::replaceExisting)
                    commitProofs_.insert_or_assign(nodeId, *proof);
                else
                    commitProofs_.emplace(nodeId, *proof);
            }
            else
            {
                JLOG(j_.debug())
                    << "RNG: commit proof not cached"
                    << " reason=nonzero-propose-seq"
                    << " source=" << sourceTag << " node=" << nodeId
                    << " proposeSeq=" << proof->proposeSeq << " seq="
                    << (seq ? std::to_string(*seq) : std::string{"unknown"});
            }
        }

        JLOG(j_.trace()) << "RNG: admitted contribution"
                         << " kind=commit"
                         << " source=" << sourceTag << " node=" << nodeId
                         << " proofed="
                         << (commitProofs_.count(nodeId) ? "yes" : "no");
        return true;
    }

    if (!seq)
    {
        JLOG(j_.warn()) << "RNG: rejecting contribution"
                        << " reason=unknown-sequence"
                        << " kind=reveal"
                        << " source=" << sourceTag << " node=" << nodeId;
        return false;
    }

    auto const commitIt = pendingCommits_.find(nodeId);
    if (commitIt == pendingCommits_.end())
    {
        JLOG(j_.debug()) << "RNG: rejecting contribution"
                         << " reason=reveal-without-commitment"
                         << " kind=reveal"
                         << " source=" << sourceTag << " node=" << nodeId
                         << " seq=" << *seq;
        return false;
    }
    if (!hasProofedCommit(nodeId))
    {
        JLOG(j_.debug()) << "RNG: rejecting contribution"
                         << " reason=reveal-without-proofed-commit"
                         << " kind=reveal"
                         << " source=" << sourceTag << " node=" << nodeId
                         << " seq=" << *seq;
        return false;
    }

    auto const expectedCommit = entropyCommitment(digest, publicKey, *seq);
    if (expectedCommit != commitIt->second)
    {
        JLOG(j_.warn()) << "RNG: rejecting contribution"
                        << " reason=reveal-commitment-mismatch"
                        << " kind=reveal"
                        << " source=" << sourceTag << " node=" << nodeId
                        << " seq=" << *seq << " expected=" << commitIt->second
                        << " calculated=" << expectedCommit;
        return false;
    }

    auto [it, inserted] = pendingReveals_.emplace(nodeId, digest);
    if (!inserted && it->second != digest)
    {
        JLOG(j_.warn()) << "RNG: validator changed reveal"
                        << " source=" << sourceTag << " node=" << nodeId
                        << " old=" << it->second << " new=" << digest;
        it->second = digest;
    }
    nodeIdToKey_.insert_or_assign(nodeId, publicKey);
    JLOG(j_.trace()) << "RNG: admitted contribution"
                     << " kind=reveal"
                     << " source=" << sourceTag << " node=" << nodeId
                     << " seq=" << *seq;
    return true;
}

std::size_t
ConsensusExtensions::expectedProposerCount() const
{
    return likelyParticipants_.size();
}

bool
ConsensusExtensions::hasQuorumOfCommits() const
{
    auto const threshold = quorumThreshold();
    auto const activeValidators = activeValidatorView()->size();
    auto const proofedCommitCount = this->proofedCommitCount();
    bool result = static_cast<std::size_t>(proofedCommitCount) >= threshold;
    JLOG(j_.trace()) << "RNG: commit quorum check"
                     << " proofedCommits=" << proofedCommitCount
                     << " threshold=" << threshold
                     << " result=" << (result ? "yes" : "no")
                     << " pendingCommits=" << pendingCommits_.size()
                     << " activeValidators=" << activeValidators
                     << " likelyParticipants=" << likelyParticipants_.size();
    return result;
}

bool
ConsensusExtensions::hasMinimumReveals() const
{
    // Wait for reveals from ALL committers, not just 80%.  The commit
    // set is deterministic (SHAMap agreed), so we know exactly which
    // validators should reveal.  Waiting for all of them ensures every
    // node builds the same entropy set.  rngPIPELINE_TIMEOUT in
    // Consensus.h is the safety valve for nodes that crash/partition
    // between commit and reveal.
    // Reveal quorum targets the commit sidecar set, not every later proposal
    // commitment we heard. That keeps proofless late commits from extending
    // the reveal wait after they were excluded from buildCommitSet().
    auto const expected = proofedCommitCount();
    auto const revealCount = proofedRevealCount();
    auto const activeValidators = activeValidatorView()->size();
    bool result = revealCount >= expected;
    JLOG(j_.trace()) << "RNG: reveal quorum check"
                     << " reveals=" << revealCount << " expected=" << expected
                     << " result=" << (result ? "yes" : "no")
                     << " pendingReveals=" << pendingReveals_.size()
                     << " pendingCommits=" << pendingCommits_.size()
                     << " activeValidators=" << activeValidators;
    return result;
}

bool
ConsensusExtensions::hasAnyReveals() const
{
    return !pendingReveals_.empty();
}

void
ConsensusExtensions::acceptEntropySet(uint256 const& hash)
{
    acceptedEntropySetHash_ = hash;
}

void
ConsensusExtensions::clearAcceptedEntropySet()
{
    acceptedEntropySetHash_.reset();
}

ConsensusExtensions::EntropySelection
ConsensusExtensions::selectEntropy(
    uint256 const& agreedTxSetHash,
    LedgerIndex seq) const
{
    //@@start entropy-selector-fallback
    // Tier 1 fallback: consensus-bound deterministic digest over already-agreed
    // round inputs. agreedTxSetHash is the pre-injection consensus tx set hash:
    // the digest must never depend on a set that could contain the pseudo-tx
    // carrying it (circular).
    auto const fallback = [&]() -> EntropySelection {
        return {
            sha512Half(
                HashPrefix::entropyFallback,
                roundPrevLedgerHash_,
                agreedTxSetHash,
                seq),
            entropyTierConsensusFallback,
            0,
            0};
    };
    //@@end entropy-selector-fallback

    // Standalone/dev: synthetic deterministic entropy so hook dice/random work.
    //@@start entropy-selector-standalone
    if (app_.config().standalone())
        return {
            sha512Half(std::string("standalone-entropy"), seq),
            entropyTierValidatorQuorum,
            20,
            20};
    //@@end entropy-selector-standalone

    // Non-fallback entropy labels depend on validator-view thresholds. Without
    // an on-ledger UNLReport, that view is derived from local trusted config,
    // so two nodes can agree on the same entropy set but label it with
    // different tiers. A non-standalone node therefore mints only
    // consensus_fallback until the round view is ledger-anchored.
    auto const validatorView = activeValidatorView();
    //@@start entropy-selector-unlreport-gate
    if (!validatorView->fromUNLReport)
    {
        if (validatorView->sourceLedgerHash)
        {
            XRPL_ASSERT(
                *validatorView->sourceLedgerHash == roundPrevLedgerHash_,
                "ripple::ConsensusExtensions::selectEntropy : "
                "active view source matches round parent");
        }
        JLOG(j_.warn()) << "RNG: using consensus fallback entropy"
                        << " reason=no-unl-report"
                        << " seq=" << seq
                        << " activeValidators=" << validatorView->size()
                        << " originalView=" << validatorView->originalViewSize
                        << " sourceLedgerHash="
                        << (validatorView->sourceLedgerHash
                                ? to_string(*validatorView->sourceLedgerHash)
                                : std::string{"none"})
                        << " roundPrevLedgerHash=" << roundPrevLedgerHash_;
        return fallback();
    }
    //@@end entropy-selector-unlreport-gate

    // A cached entropySetMap_ is only candidate material. The tick gate marks
    // the sidecar hash accepted after peer-observation/alignment checks have
    // completed; injection never consults local timeout state directly.
    if (!acceptedEntropySetHash_ || !entropySetMap_ ||
        entropySetMap_->getHash().as_uint256() != *acceptedEntropySetHash_)
        return fallback();
    auto const agreedHash = *acceptedEntropySetHash_;

    // Derive from the AGREED entropySetMap_ — NOT local pendingReveals_. The
    // map's hash was published in proposals and accepted by the gate, so every
    // node holding the same entropySetHash produces byte-identical entropy and
    // the same tier/count/denominator labels. Read leaves through the shared
    // sidecar admission helper so accepted-map consumption enforces the same
    // content-address/type contract as snapshot construction.
    std::vector<std::pair<PublicKey, uint256>> sorted;
    entropySetMap_->visitLeaves(
        [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
            try
            {
                auto admitted = admitSidecarLeaf(
                    item->key(),
                    item->slice(),
                    agreedHash,
                    j_,
                    "RNG",
                    "entropySet",
                    "select");
                if (!admitted || admitted->type != sidecarRngReveal)
                    return;
                auto const& sidecar = admitted->sidecar;
                auto const pk = sidecar.getFieldVL(sfSigningPubKey);
                if (!publicKeyType(makeSlice(pk)))
                    return;
                sorted.emplace_back(
                    PublicKey(makeSlice(pk)), sidecar.getFieldH256(sfDigest));
            }
            catch (...)
            {
            }
        });

    // Residual: the gate passed but no leaf parsed — fall back rather than
    // skip, so a fresh ConsensusEntropy entry always exists.
    if (sorted.empty())
        return fallback();

    std::sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b) {
        if (a.first.slice() < b.first.slice())
            return true;
        if (b.first.slice() < a.first.slice())
            return false;
        return a.second < b.second;
    });

    Serializer s;
    for (auto const& [key, reveal] : sorted)
    {
        s.addVL(key.slice());
        s.addBitString(reveal);
    }
    auto const digest = sha512Half(s.slice());
    auto const count = static_cast<std::uint16_t>(sorted.size());
    auto const denominator = static_cast<std::uint16_t>(validatorView->size());

    //@@start entropy-selector-tier-ladder
    // Tier ladder over the AGREED participant count — deterministic on every
    // node holding this entropySetHash. quorumThreshold() = ceil(0.8 *
    // effective view); tier2Threshold() = the equivocation-safe intersection
    // floor over the original view (~0.6*n; see calculateParticipantThreshold).
    // Below tier2Threshold too few aligned participants contributed to trust
    // the result — fall back.
    auto const tier = selectEntropyTierForView(
        validatorView->fromUNLReport,
        count,
        validatorView->size(),
        validatorView->originalViewSize);
    if (tier != entropyTierConsensusFallback)
        return {digest, static_cast<std::uint8_t>(tier), count, denominator};
    return fallback();
    //@@end entropy-selector-tier-ladder
}

bool
ConsensusExtensions::rngEnabled() const
{
    return rngEnabledThisRound_.load(std::memory_order_relaxed);
}

uint256
ConsensusExtensions::txnOrderingSalt(
    uint256 const& agreedTxSetHash,
    LedgerIndex seq) const
{
    if (!rngEnabled())
        return agreedTxSetHash;

    auto const selection = selectEntropy(agreedTxSetHash, seq);
    return sha512Half(
        HashPrefix::entropyTxnOrder,
        agreedTxSetHash,
        selection.digest,
        selection.tier,
        selection.count,
        selection.denominator);
}

bool
ConsensusExtensions::exportEnabled() const
{
    return exportEnabledThisRound_.load(std::memory_order_relaxed);
}

bool
ConsensusExtensions::testSuppressExportSigSetHash() const
{
    auto const cfg = app_.getRuntimeConfig().getConsensusTestConfig();
    return cfg && cfg->noExportSigHash.has_value() && *cfg->noExportSigHash;
}

bool
ConsensusExtensions::testBootstrapFastStartEnabled() const
{
    auto const cfg = app_.getRuntimeConfig().getConsensusTestConfig();
    if (cfg && cfg->bootstrapFastStart.has_value())
        return *cfg->bootstrapFastStart;
    return false;
}

uint256
ConsensusExtensions::buildCommitSet(LedgerIndex seq)
{
    // Track the active RNG round explicitly. Nodes in observing/switching
    // mode can have a closed ledger index behind the consensus round while
    // still building that round's local RNG snapshots.
    rngRoundSeq_ = seq;

    auto map =
        std::make_shared<SHAMap>(SHAMapType::SIDECAR, app_.getNodeFamily());
    map->setUnbacked();

    auto const validatorView = activeValidatorView();
    std::size_t entryCount = 0;
    // NOTE: avoid structured bindings in for-loops containing lambdas —
    // clang-14 (CI) rejects capturing them (P2036R3 not implemented).
    for (auto const& entry : pendingCommits_)
    {
        auto const& nid = entry.first;
        auto const& commit = entry.second;

        // Commit sidecars are consensus inputs, so only publish leaves from
        // the proofed commit set over the frozen quorum validator view.
        if (!hasActiveProofedCommit(nid, *validatorView))
            continue;

        auto kit = nodeIdToKey_.find(nid);
        if (kit == nodeIdToKey_.end())
            continue;
        auto proofIt = commitProofs_.find(nid);
        if (proofIt == commitProofs_.end())
            continue;

        // Encode the NodeID into sfAccount so the local snapshot can be
        // replayed without recomputing master-vs-signing key identity.
        AccountID acctId;
        std::memcpy(acctId.data(), nid.data(), acctId.size());

        STObject sidecar(sfGeneric);
        sidecar.setFieldU8(sfSidecarType, sidecarRngCommit);
        sidecar.setFieldU32(sfLedgerSequence, seq);
        sidecar.setAccountID(sfAccount, acctId);
        sidecar.setFieldH256(sfDigest, commit);
        sidecar.setFieldVL(sfSigningPubKey, kit->second.slice());
        sidecar.setFieldVL(sfBlob, serializeProof(proofIt->second));

        map->addItem(SHAMapNodeType::tnSIDECAR, makeSidecarItem(sidecar));
        ++entryCount;
    }

    map = map->snapShot(false);
    commitSetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    // TODO: move consensus-extension snapshots out of InboundTransactions.
    // They are same-process materialization caches only; sidecar roots are no
    // longer advertised, fetched, served, or merged from peers.
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built commitSet SHAMap"
                     << " hash=" << hash << " seq=" << seq
                     << " entries=" << entryCount
                     << " pendingCommits=" << pendingCommits_.size()
                     << " activeValidators=" << validatorView->size();
    return hash;
}

uint256
ConsensusExtensions::buildEntropySet(LedgerIndex seq)
{
    rngRoundSeq_ = seq;

    auto map =
        std::make_shared<SHAMap>(SHAMapType::SIDECAR, app_.getNodeFamily());
    map->setUnbacked();

    auto const validatorView = activeValidatorView();
    std::size_t entryCount = 0;
    // NOTE: avoid structured bindings — clang-14 can't capture them (P2036R3).
    for (auto const& entry : pendingReveals_)
    {
        auto const& nid = entry.first;
        auto const& reveal = entry.second;

        // The entropy set is the reveal side of the proofed commit set. Late
        // proofless commits/reveals may sit in the local harvest cache, but
        // they must not affect the agreed digest/count/tier.
        if (!hasActiveProofedCommit(nid, *validatorView))
            continue;

        auto kit = nodeIdToKey_.find(nid);
        if (kit == nodeIdToKey_.end())
            continue;

        AccountID acctId;
        std::memcpy(acctId.data(), nid.data(), acctId.size());

        STObject sidecar(sfGeneric);
        sidecar.setFieldU8(sfSidecarType, sidecarRngReveal);
        sidecar.setFieldU32(sfLedgerSequence, seq);
        sidecar.setAccountID(sfAccount, acctId);
        sidecar.setFieldH256(sfDigest, reveal);
        sidecar.setFieldVL(sfSigningPubKey, kit->second.slice());
        // Intentionally omit sfBlob for reveal-set entries.
        //
        // Reveal proofs are timing-dependent (seq/closeTime/signature can
        // differ while the reveal digest is identical), which makes the
        // entropy-set hash non-deterministic across nodes under packet
        // loss/reordering. We only need deterministic reveal material
        // (validator identity + digest) for accepted-root replay and entropy
        // calculation.

        map->addItem(SHAMapNodeType::tnSIDECAR, makeSidecarItem(sidecar));
        ++entryCount;
    }

    map = map->snapShot(false);
    entropySetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    // TODO: move consensus-extension snapshots out of InboundTransactions.
    // They are same-process materialization caches only; sidecar roots are no
    // longer advertised, fetched, served, or merged from peers.
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built entropySet SHAMap"
                     << " hash=" << hash << " seq=" << seq
                     << " entries=" << entryCount
                     << " pendingReveals=" << pendingReveals_.size()
                     << " activeValidators=" << validatorView->size();
    return hash;
}

uint256
ConsensusExtensions::buildExportSigSet(LedgerIndex seq)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::SIDECAR, app_.getNodeFamily());
    map->setUnbacked();

    auto const validatorView = activeValidatorView();
    // Export sidecar convergence should not advertise signatures from trusted
    // but inactive validators; those signatures cannot count at apply time.
    auto const allSigs = exportSigCollector_.snapshotWithSigs(
        activeSignerFilter(*this, validatorView));
    // Only signatures for export txns in the consensus candidate can affect
    // this round's sidecar hash; open-ledger-only txns stay cached for later.
    std::size_t entryCount = 0;

    for (auto const& [txHash, valSigs] : allSigs)
    {
        // Candidate membership is the deterministic publication gate. A sig
        // may have been verified earlier from the open ledger, but it only
        // enters the sidecar hash if the same tx hash is in the converged set.
        if (consensusExportTxns_.find(txHash) == consensusExportTxns_.end())
            continue;

        for (auto const& [valPK, sigBuf] : valSigs)
        {
            STObject sidecar(sfGeneric);
            sidecar.setFieldU8(sfSidecarType, sidecarExportSig);
            sidecar.setFieldH256(sfTransactionHash, txHash);
            sidecar.setFieldVL(sfSigningPubKey, valPK.slice());
            if (sigBuf.size() > 0)
                sidecar.setFieldVL(
                    sfTxnSignature, Slice(sigBuf.data(), sigBuf.size()));

            map->addItem(SHAMapNodeType::tnSIDECAR, makeSidecarItem(sidecar));
            ++entryCount;
        }
    }

    auto const maxExportSidecarLeaves = validatorView->size() *
        std::min(consensusExportTxns_.size(),
                 static_cast<std::size_t>(ExportLimits::maxPendingExports));
    XRPL_ASSERT(
        entryCount <= maxExportSidecarLeaves,
        "ripple::ConsensusExtensions::buildExportSigSet : "
        "export sidecar leaf count must stay within bounded local cap");

    map = map->snapShot(false);
    exportSigSetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    // TODO: move consensus-extension snapshots out of InboundTransactions.
    // They are same-process materialization caches only; sidecar roots are no
    // longer advertised, fetched, served, or merged from peers.
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "Export: built exportSigSet SHAMap"
                     << " hash=" << hash << " seq=" << seq
                     << " entries=" << entryCount
                     << " candidateExportTxns=" << consensusExportTxns_.size()
                     << " activeValidators=" << validatorView->size();
    return hash;
}

bool
ConsensusExtensions::hasPendingExportSigs() const
{
    auto const validatorView = activeValidatorView();
    // The export convergence gate only needs to run for signatures that are
    // eligible under the active view used by final quorum evaluation.
    auto const allSigs = exportSigCollector_.snapshotWithSigs(
        activeSignerFilter(*this, validatorView));
    if (allSigs.empty() || !consensusTxSetMap_)
        return false;

    for (auto const& entry : allSigs)
    {
        if (consensusExportTxns_.find(entry.first) !=
            consensusExportTxns_.end())
            return true;
    }
    return false;
}

bool
ConsensusExtensions::hasConsensusExportTxns() const
{
    return !consensusExportTxns_.empty();
}

void
ConsensusExtensions::setExportSigConvergenceFailed()
{
    exportSigConvergenceFailed_ = true;
}

bool
ConsensusExtensions::exportSigConvergenceFailed() const
{
    return exportSigConvergenceFailed_;
}

void
ConsensusExtensions::acceptExportSigSet(uint256 const& hash)
{
    acceptedExportSigSetHash_ = hash;
}

void
ConsensusExtensions::clearAcceptedExportSigSet()
{
    acceptedExportSigSetHash_.reset();
}

std::optional<ConsensusExtensions::ExportSignatureSnapshot>
ConsensusExtensions::agreedExportSignatures(
    STTx const& exportTx,
    uint256 const& txHash,
    std::size_t threshold) const
{
    // A local exportSigSetMap_ is only candidate material until the sidecar
    // gate accepts its root. Without this guard, a timed-out node with a local
    // partial-but-quorum map could mint a different signed export blob from
    // the quorum-aligned nodes.
    if (!acceptedExportSigSetHash_)
    {
        JLOG(j_.warn()) << "Export: exportSigSet not accepted"
                        << " txHash=" << txHash << " threshold=" << threshold;
        return std::nullopt;
    }

    auto const acceptedHash = *acceptedExportSigSetHash_;
    std::shared_ptr<SHAMap> agreedMap;
    if (exportSigSetMap_ &&
        exportSigSetMap_->getHash().as_uint256() == acceptedHash)
    {
        agreedMap = exportSigSetMap_;
    }
    else
    {
        agreedMap = app_.getInboundTransactions().getSet(acceptedHash, false);
    }

    if (!agreedMap)
    {
        JLOG(j_.warn()) << "Export: accepted exportSigSet missing"
                        << " acceptedHash=" << acceptedHash
                        << " txHash=" << txHash << " threshold=" << threshold;
        return std::nullopt;
    }
    if (agreedMap->mapType() != SHAMapType::SIDECAR)
    {
        JLOG(j_.warn()) << "Export: accepted exportSigSet has wrong map type"
                        << " acceptedHash=" << acceptedHash
                        << " txHash=" << txHash;
        return std::nullopt;
    }

    auto const agreedHash = agreedMap->getHash().as_uint256();
    if (agreedHash != acceptedHash)
    {
        JLOG(j_.warn()) << "Export: accepted exportSigSet hash mismatch"
                        << " setHash=" << agreedHash
                        << " acceptedHash=" << acceptedHash
                        << " txHash=" << txHash;
        return std::nullopt;
    }

    // The accepted root is the membership decision. Candidate construction
    // filters live active signers, but materialization must not re-resolve
    // signer keys through mutable manifests or nodes can diverge after a
    // rotation. Keep cryptographic verification below; drop only the live
    // membership re-filter.
    ExportSignatureSnapshot signatures;
    bool invalid = false;
    agreedMap->visitLeaves(
        [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
            if (invalid)
                return;

            try
            {
                auto admitted = admitSidecarLeaf(
                    item->key(),
                    item->slice(),
                    agreedHash,
                    j_,
                    "Export",
                    "exportSigSet",
                    "agreed",
                    ExportLimits::maxExportSignatureSidecarBytes,
                    &invalid);
                if (!admitted || admitted->type != sidecarExportSig)
                    return;
                auto const& sidecar = admitted->sidecar;

                if (!sidecar.isFieldPresent(sfTransactionHash) ||
                    !sidecar.isFieldPresent(sfSigningPubKey) ||
                    !sidecar.isFieldPresent(sfTxnSignature))
                    return;

                if (sidecar.getFieldH256(sfTransactionHash) != txHash)
                    return;

                auto const pk = sidecar.getFieldVL(sfSigningPubKey);
                if (!publicKeyType(makeSlice(pk)))
                    return;

                PublicKey const valPK{makeSlice(pk)};

                auto const sigVL = sidecar.getFieldVL(sfTxnSignature);
                auto const sigSlice = makeSlice(sigVL);
                if (!verifyExportSignatureAgainstTx(
                        exportTx,
                        valPK,
                        sigSlice,
                        txHash,
                        j_,
                        "agreed exportSigSet"))
                {
                    invalid = true;
                    return;
                }

                Buffer sigBuf(sigSlice.data(), sigSlice.size());
                if (auto const [_, inserted] =
                        signatures.emplace(valPK, std::move(sigBuf));
                    !inserted)
                {
                    JLOG(j_.warn())
                        << "Export: accepted exportSigSet duplicate signer"
                        << " setHash=" << agreedHash << " txHash=" << txHash
                        << " signer=" << toBase58(TokenType::NodePublic, valPK);
                    invalid = true;
                }
            }
            catch (std::exception const& e)
            {
                JLOG(j_.warn())
                    << "Export: agreed exportSigSet parse failed"
                    << " setHash=" << agreedHash << " txHash=" << txHash
                    << " error=" << e.what();
                invalid = true;
            }
        });

    if (invalid)
        return std::nullopt;

    if (signatures.size() < threshold)
    {
        JLOG(j_.info()) << "Export: accepted exportSigSet below quorum"
                        << " setHash=" << agreedHash << " txHash=" << txHash
                        << " signers=" << signatures.size()
                        << " threshold=" << threshold;
        return std::nullopt;
    }

    return signatures;
}

void
ConsensusExtensions::generateEntropySecret()
{
    // Generate cryptographically secure random entropy
    crypto_prng()(myEntropySecret_.data(), myEntropySecret_.size());
    entropyFailed_ = false;
}

uint256
ConsensusExtensions::getEntropySecret() const
{
    return myEntropySecret_;
}

void
ConsensusExtensions::setEntropyFailed()
{
    entropyFailed_ = true;
}

void
ConsensusExtensions::freezeRngCommitSet()
{
    commitSetFrozen_ = true;
}

void
ConsensusExtensions::selfSeedReveal()
{
    auto const& valKeys = app_.getValidatorKeys();
    if (!valKeys.keys || valKeys.nodeID == beast::zero)
        return;

    if (myEntropySecret_ != uint256{})
    {
        pendingReveals_[valKeys.nodeID] = myEntropySecret_;
        nodeIdToKey_.insert_or_assign(valKeys.nodeID, valKeys.keys->publicKey);
    }
}

//@@start clear-rng-state
void
ConsensusExtensions::clearRngStatePreservingExport()
{
    //@@start round-stop-rng-reset
    pendingCommits_.clear();
    pendingReveals_.clear();
    nodeIdToKey_.clear();
    myEntropySecret_ = uint256{};
    entropyFailed_ = false;
    commitSetFrozen_ = false;
    commitSetMap_.reset();
    entropySetMap_.reset();
    acceptedEntropySetHash_.reset();
    rngRoundSeq_.reset();
    roundPrevLedgerHash_ = uint256{};
    consensusTxSetMap_.reset();
    consensusExportTxns_.clear();
    consensusTxSetHash_.reset();
    observedParticipantsHash_.reset();
    observedParticipantsCount_ = 0;
    observedParticipantsBitmapBin_.clear();
    likelyParticipants_.clear();
    commitProofs_.clear();
    //@@end round-stop-rng-reset
    // Keep the round-level enable latches intact here. Callers either already
    // hold a valid snapshot, or onRoundStart() refreshes it from the consensus
    // parent ledger after clearing per-round working state.
}

void
ConsensusExtensions::clearRngState()
{
    //@@start round-stop-export-reset
    exportSigCollector_.clearRound();
    if (auto const closed = app_.getLedgerMaster().getClosedLedger())
        exportSigCollector_.cleanupStale(closed->info().seq);
    if (!exportEnabled())
    {
        // Export disabled is an amendment boundary, not a retry boundary.
        // Drop cached signatures so an emergency stop cannot leave old quorum
        // material waiting for a later re-enable.
        exportSigCollector_.clearAll();
    }
    exportSigSetMap_.reset();
    acceptedExportSigSetHash_.reset();
    consensusExportTxns_.clear();
    exportSigGateStarted_ = false;
    exportSigGateStart_ = {};
    exportSigConvergenceFailed_ = false;
    //@@end round-stop-export-reset

    clearRngStatePreservingExport();
}
//@@end clear-rng-state

void
ConsensusExtensions::cacheUNLReport(
    std::shared_ptr<Ledger const> const& prevLedger)
{
    auto view = makeActiveValidatorView(prevLedger);
    auto const size = view->size();
    auto const fromUNLReport = view->fromUNLReport;

    {
        std::lock_guard lock(activeValidatorViewMutex_);
        activeValidatorView_ = std::move(view);
    }

    JLOG(j_.trace()) << "RNG: cached active validator view"
                     << " activeValidators=" << size << " source="
                     << (fromUNLReport ? "UNLReport" : "trusted-fallback");
}

bool
ConsensusExtensions::isUNLReportMember(NodeID const& nodeId) const
{
    // RNG commit/reveal sidecars identify validators by master-key NodeID, so
    // use the shared active view instead of a separate RNG-only membership set.
    return activeValidatorView()->containsNode(nodeId);
}

bool
ConsensusExtensions::localIsActiveValidator() const
{
    // Our own sidecar position only counts toward alignment when this validator
    // is itself in the active view — the same universe as the peer-membership
    // filter and the entropy/export thresholds.
    auto const& valKeys = app_.getValidatorKeys();
    if (!valKeys.keys || valKeys.nodeID == beast::zero)
        return false;
    return activeValidatorView()->containsNode(valKeys.nodeID);
}

ConsensusExtensions::ActiveValidatorViewPtr
ConsensusExtensions::activeValidatorView() const
{
    std::lock_guard lock(activeValidatorViewMutex_);
    return activeValidatorView_;
}

ConsensusExtensions::ActiveValidatorViewPtr
ConsensusExtensions::makeActiveValidatorView(
    std::shared_ptr<Ledger const> const& prevLedger) const
{
    // Prefer the consensus parent ledger so all validators evaluate the round
    // against the same frozen UNLReport, not a local latest-validated ledger.
    if (!prevLedger)
    {
        XRPL_ASSERT(
            roundPrevLedgerHash_.isZero(),
            "ripple::ConsensusExtensions::makeActiveValidatorView : "
            "null parent is outside an active consensus round");
    }
    auto const sourceLedger =
        prevLedger ? prevLedger : app_.getLedgerMaster().getValidatedLedger();
    XRPL_ASSERT(
        sourceLedger,
        "ripple::ConsensusExtensions::makeActiveValidatorView : "
        "source ledger is available");
    return std::make_shared<ActiveValidatorView const>(buildActiveValidatorView(
        buildActiveValidatorViewSource(sourceLedger),
        buildActiveValidatorViewFallback(app_)));
}

bool
ConsensusExtensions::isActiveValidator(PublicKey const& validationKey) const
{
    return isActiveValidator(validationKey, *activeValidatorView());
}

bool
ConsensusExtensions::isActiveValidator(
    PublicKey const& validationKey,
    ActiveValidatorView const& view) const
{
    auto const trustedMaster = app_.validators().getTrustedKey(validationKey);
    if (!trustedMaster)
        return false;

    return view.containsMaster(*trustedMaster);
}

void
ConsensusExtensions::recordParticipantDiagnostics(
    ConsensusMode mode,
    std::vector<NodeID> peerNodeIds)
{
    auto const view = activeValidatorView();
    hash_set<NodeID> observed;
    observed.reserve(peerNodeIds.size() + 1);

    for (auto const& nodeId : peerNodeIds)
    {
        if (view->containsNode(nodeId))
            observed.insert(nodeId);
    }

    auto const& valKeys = app_.getValidatorKeys();
    if (mode == ConsensusMode::proposing && valKeys.nodeID != beast::zero &&
        view->containsNode(valKeys.nodeID))
    {
        observed.insert(valKeys.nodeID);
    }

    std::vector<NodeID> activeSorted(
        view->nodeIds.begin(), view->nodeIds.end());
    std::sort(activeSorted.begin(), activeSorted.end());
    std::vector<NodeID> observedSorted(observed.begin(), observed.end());
    std::sort(observedSorted.begin(), observedSorted.end());
    auto bitmapBin = buildObservedParticipantBitmap(activeSorted, observed);

    Serializer s(512);
    if (view->sourceLedgerHash)
        s.addBitString(*view->sourceLedgerHash);
    else
    {
        uint256 noSource;
        noSource.zero();
        s.addBitString(noSource);
    }
    s.add32(static_cast<std::uint32_t>(view->size()));
    for (auto const& nodeId : activeSorted)
        s.addBitString(nodeId);
    s.add32(static_cast<std::uint32_t>(observedSorted.size()));
    for (auto const& nodeId : observedSorted)
        s.addBitString(nodeId);

    auto const hash = sha512Half(HashPrefix::observedParticipants, s.slice());
    bool const changed = !observedParticipantsHash_ ||
        *observedParticipantsHash_ != hash ||
        observedParticipantsCount_ != observedSorted.size() ||
        observedParticipantsBitmapBin_ != bitmapBin;

    observedParticipantsHash_ = hash;
    observedParticipantsCount_ = observedSorted.size();
    observedParticipantsBitmapBin_ = std::move(bitmapBin);

    if (changed)
    {
        JLOG(j_.debug()) << "STALLDIAG: observed-active-participants"
                         << " count=" << observedParticipantsCount_
                         << " activeView=" << view->size()
                         << " quorum=" << quorumThreshold() << " hash=" << hash
                         << " source="
                         << (view->fromUNLReport ? "UNLReport"
                                                 : "trusted-fallback")
                         << " mode=" << to_string(mode)
                         << " peerPositions=" << peerNodeIds.size()
                         << " bitmapBin=" << observedParticipantsBitmapBin_;
    }
}

void
ConsensusExtensions::attachParticipantDiagnostics(ExtendedPosition& pos) const
{
    if (!rngEnabled() && !exportEnabled())
        return;

    if (observedParticipantsHash_)
        pos.observedParticipantsHash = observedParticipantsHash_;
}

std::size_t
ConsensusExtensions::observedParticipantCount() const
{
    return observedParticipantsCount_;
}

std::optional<uint256>
ConsensusExtensions::observedParticipantsHash() const
{
    return observedParticipantsHash_;
}

std::string const&
ConsensusExtensions::observedParticipantsBitmapBin() const
{
    return observedParticipantsBitmapBin_;
}

void
ConsensusExtensions::cacheConsensusTxSet(RCLTxSet const& txns)
{
    auto const txSetHash = txns.id();
    if (consensusTxSetHash_ && *consensusTxSetHash_ == txSetHash)
        return;

    consensusTxSetMap_ = txns.map_;
    consensusExportTxns_ = buildExportTxnLookup(*txns.map_, j_);
    consensusTxSetHash_ = txSetHash;
}

std::size_t
ConsensusExtensions::verifyPendingExportSigs(
    RCLTxSet const& txns,
    LedgerIndex seq)
{
    if (!exportSigCollector_.hasUnverifiedSignatures())
        return 0;

    if (!consensusTxSetHash_ || *consensusTxSetHash_ != txns.id())
        cacheConsensusTxSet(txns);

    if (consensusExportTxns_.empty())
        return 0;

    auto const validatorView = activeValidatorView();
    auto const isActiveSigner = activeSignerFilter(*this, validatorView);
    std::size_t upgraded = 0;
    for (auto const& [txHash, stx] : consensusExportTxns_)
    {
        auto const unverified =
            exportSigCollector_.unverifiedSignatures(txHash);
        for (auto const& [valPK, sigBuf] : unverified)
        {
            if (!isActiveSigner(valPK))
                continue;

            if (!verifyExportSignatureAgainstTx(
                    *stx,
                    valPK,
                    Slice(sigBuf.data(), sigBuf.size()),
                    txHash,
                    j_,
                    "consensus tx set"))
                continue;

            exportSigCollector_.upgradeSignature(txHash, valPK, sigBuf, seq);
            ++upgraded;
        }
    }

    if (upgraded > 0)
    {
        JLOG(j_.debug()) << "Export: upgraded proposal signatures"
                         << " upgraded=" << upgraded << " txSet=" << txns.id()
                         << " seq=" << seq << " candidateExportTxns="
                         << consensusExportTxns_.size();
    }
    return upgraded;
}

void
ConsensusExtensions::onPreBuild(
    CanonicalTXSet& retriableTxs,
    LedgerIndex seq,
    uint256 const& txSetHash)
{
    if (rngEnabled())
    {
        JLOG(j_.info()) << "RNG: injectEntropy"
                        << " seq=" << seq
                        << " commits=" << pendingCommits_.size()
                        << " reveals=" << pendingReveals_.size()
                        << " entropyFailed=" << (entropyFailed_ ? "yes" : "no")
                        << " quorum=" << quorumThreshold() << " entropySetHash="
                        << (entropySetMap_
                                ? to_string(
                                      entropySetMap_->getHash().as_uint256())
                                : std::string{"none"});

        //@@start rng-inject-entropy-selection
        // One deterministic selector over the AGREED entropySetMap_ chooses the
        // digest and its tier/count/denominator labels. Every node derives the
        // same entropy for the same agreed round inputs. txSetHash is the
        // agreed pre-injection consensus tx set hash.
        auto const selection = selectEntropy(txSetHash, seq);
        uint256 const finalEntropy = selection.digest;
        std::uint8_t const entropyTier = selection.tier;
        std::uint16_t const entropyCount = selection.count;
        std::uint16_t const entropyDenominator = selection.denominator;
        //@@end rng-inject-entropy-selection

        JLOG(j_.info()) << "RNG: entropy selected"
                        << " seq=" << seq
                        << " tier=" << static_cast<int>(entropyTier)
                        << " count=" << entropyCount
                        << " denominator=" << entropyDenominator
                        << " digest=" << finalEntropy;

        //@@start rng-inject-pseudotx
        // Synthesize and inject the pseudo-transaction. The selector always
        // yields a digest (fallback when there is no validator entropy), so
        // injection is unconditional — every RNG-enabled ledger carries a
        // ConsensusEntropy tx.
        {
            // Design note: this is the canonical path that materializes
            // the synthetic entropy-bearing tx-set in production.
            //
            // Why here (onAccept/buildLCL) instead of mutating proposals
            // earlier?
            // - Consensus agreement is keyed by proposal txSetHash during
            //   establish. Late mutation of txSetHash in establish can fragment
            //   votes under loss/reordering (base hash vs synthetic hash).
            // - Injecting at accept preserves robust convergence semantics:
            //   peers agree on the base transaction set first, then
            //   deterministically derive/apply the entropy pseudo-tx for ledger
            //   construction.
            //
            //@@start rng-inject-pseudotx-core
            // Account Zero convention for pseudo-transactions (same as ttFEE,
            // etc)
            STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
                obj.setFieldU32(sfLedgerSequence, seq);
                obj.setAccountID(sfAccount, AccountID{});
                obj.setFieldU32(sfSequence, 0);
                obj.setFieldAmount(sfFee, STAmount{});
                obj.setFieldH256(sfDigest, finalEntropy);
                obj.setFieldU16(sfEntropyCount, entropyCount);
                obj.setFieldU16(sfEntropyDenominator, entropyDenominator);
                obj.setFieldU8(sfEntropyTier, entropyTier);
            });

            auto const txID = tx.getTransactionID();
            // Value-based dedup. There must never be two entropy pseudo-txs,
            // but when one is already present in the agreed set it must be
            // VALIDATED as the exact pseudo-tx we would have produced — not
            // merely "same type". Injection is deterministic, so every honest
            // node derives the identical pseudo-tx (identical txID) for the
            // same agreed inputs. A present-but-different entropy pseudo-tx is
            // therefore a determinism violation (version skew or a
            // divergent/malicious peer) and must be surfaced, not silently
            // trusted.
            auto const existing = std::find_if(
                retriableTxs.begin(),
                retriableTxs.end(),
                [](auto const& entry) {
                    return entry.second->getTxnType() == ttCONSENSUS_ENTROPY;
                });
            if (existing != retriableTxs.end())
            {
                auto const existingID = existing->second->getTransactionID();
                if (existingID == txID)
                {
                    JLOG(j_.debug()) << "RNG: entropy pseudo-tx already present"
                                     << " txHash=" << txID << " seq=" << seq
                                     << " action=skip-duplicate-verified";
                }
                else
                {
                    // The agreed tx set's hash already commits to the existing
                    // pseudo-tx, so we cannot replace it without forking off
                    // the agreed ledger. This is detect-and-log only: the
                    // existing pseudo-tx is KEPT and is still applied at ledger
                    // build (BuildLedger applyTransactions). A hard-fail/reject
                    // policy on mismatch is a deliberate future decision (it
                    // trades a determinism violation for a halt risk under
                    // benign skew).
                    //
                    // Read present fields defensively: the mismatching
                    // pseudo-tx may be exactly the old/malformed (pre-tier)
                    // entry we are guarding against, and getField...() on a
                    // missing required field would throw here, inside
                    // onPreBuild during build.
                    auto const& pres = *existing->second;
                    JLOG(j_.error())
                        << "RNG: entropy pseudo-tx MISMATCH"
                        << " seq=" << seq << " reason=determinism-violation"
                        << " action=keep-agreed-and-flag"
                        << " ourTxHash=" << txID
                        << " ourDigest=" << finalEntropy
                        << " ourTier=" << static_cast<int>(entropyTier)
                        << " ourCount=" << entropyCount
                        << " presentTxHash=" << existingID << " presentDigest="
                        << (pres.isFieldPresent(sfDigest)
                                ? to_string(pres.getFieldH256(sfDigest))
                                : std::string{"<missing>"})
                        << " presentTier="
                        << (pres.isFieldPresent(sfEntropyTier)
                                ? std::to_string(pres.getFieldU8(sfEntropyTier))
                                : std::string{"<missing>"})
                        << " presentCount="
                        << (pres.isFieldPresent(sfEntropyCount)
                                ? std::to_string(
                                      pres.getFieldU16(sfEntropyCount))
                                : std::string{"<missing>"})
                        << " presentDenominator="
                        << (pres.isFieldPresent(sfEntropyDenominator)
                                ? std::to_string(
                                      pres.getFieldU16(sfEntropyDenominator))
                                : std::string{"<missing>"});
                }
            }
            else
            {
                retriableTxs.insert(std::make_shared<STTx>(std::move(tx)));
            }
            //@@end rng-inject-pseudotx-core
        }
        //@@end rng-inject-pseudotx
    }

    if (exportEnabled())
    {
        auto const validatorView = activeValidatorView();
        for (auto it = retriableTxs.begin(); it != retriableTxs.end();)
        {
            auto const& tx = it->second;
            if (tx && tx->getTxnType() == ttEXPORT_SIGNATURES)
            {
                // Live witnesses are build-time materializations of the
                // accepted sidecar root. Remove stale or externally supplied
                // pseudos before reinserting the deterministic witness below.
                it = retriableTxs.erase(it);
                continue;
            }
            ++it;
        }

        if (app_.config().standalone())
        {
            auto const& valKeys = app_.getValidatorKeys();
            if (valKeys.keys)
            {
                for (auto const& entry : retriableTxs)
                {
                    auto const& stx = entry.second;
                    if (!stx || stx->getTxnType() != ttEXPORT ||
                        !stx->isFieldPresent(sfExportedTxn))
                    {
                        continue;
                    }

                    auto const exportTxHash = stx->getTransactionID();
                    auto const existing = std::find_if(
                        retriableTxs.begin(),
                        retriableTxs.end(),
                        [&](auto const& candidate) {
                            auto const& tx = candidate.second;
                            return tx &&
                                tx->getTxnType() == ttEXPORT_SIGNATURES &&
                                tx->isFieldPresent(sfTransactionHash) &&
                                tx->getFieldH256(sfTransactionHash) ==
                                exportTxHash;
                        });

                    if (existing != retriableTxs.end())
                        continue;

                    auto innerTx = ExportLedgerOps::innerExportedTx(*stx);
                    if (!innerTx)
                    {
                        JLOG(j_.warn()) << "Export: standalone witness skipped"
                                        << " exportTxHash=" << exportTxHash
                                        << " reason=inner-tx-parse-failed";
                        continue;
                    }

                    ExportResultBuilder::SignatureSnapshot signatures;
                    signatures.emplace(
                        valKeys.keys->publicKey,
                        ExportResultBuilder::signExportedTxn(
                            *innerTx,
                            valKeys.keys->publicKey,
                            valKeys.keys->secretKey));

                    auto witness = ExportResultBuilder::buildSignatureWitness(
                        exportTxHash, signatures, seq);

                    // Standalone uses the same replay witness shape as
                    // network mode, but the witness is locally synthesized
                    // from the node's validator key instead of quorum sidecar
                    // convergence.
                    retriableTxs.insert(
                        std::make_shared<STTx>(std::move(witness)));
                }
            }
        }
        else if (validatorView->fromUNLReport)
        {
            auto const threshold = exportSigQuorumThreshold(*validatorView);
            for (auto const& entry : retriableTxs)
            {
                auto const& stx = entry.second;
                if (!stx || stx->getTxnType() != ttEXPORT ||
                    !stx->isFieldPresent(sfExportedTxn))
                    continue;

                auto const exportTxHash = stx->getTransactionID();
                auto sigs =
                    agreedExportSignatures(*stx, exportTxHash, threshold);
                if (!sigs)
                    continue;

                auto witness = ExportResultBuilder::buildSignatureWitness(
                    exportTxHash, *sigs, seq);
                auto const witnessHash = witness.getTransactionID();
                auto const existing = std::find_if(
                    retriableTxs.begin(),
                    retriableTxs.end(),
                    [&](auto const& candidate) {
                        auto const& tx = candidate.second;
                        return tx && tx->getTxnType() == ttEXPORT_SIGNATURES &&
                            tx->isFieldPresent(sfTransactionHash) &&
                            tx->getFieldH256(sfTransactionHash) == exportTxHash;
                    });

                if (existing != retriableTxs.end())
                {
                    auto const existingHash =
                        existing->second->getTransactionID();
                    if (existingHash == witnessHash)
                        continue;

                    JLOG(j_.error())
                        << "Export: signature witness pseudo-tx mismatch"
                        << " exportTxHash=" << exportTxHash
                        << " witnessHash=" << witnessHash
                        << " existingHash=" << existingHash
                        << " action=replace-with-agreed";
                    // The witness is build-time materialization of the
                    // accepted sidecar, not a base consensus-set transaction.
                    // Replacing a mismatch keeps the tx stream tied to the
                    // accepted root instead of preserving stale local input.
                    retriableTxs.erase(existing);
                }

                // Export signatures change the shadow-ticket hash, so they
                // must be tx-stream input, not only accepted sidecar memory.
                // The matching ttEXPORT consumes this pseudo through the
                // BuildLedger pre-scan; the pseudo itself has no ledger effect.
                retriableTxs.insert(std::make_shared<STTx>(std::move(witness)));
            }
        }
        else if (!consensusExportTxns_.empty())
        {
            JLOG(j_.warn())
                << "Export: not injecting signature witnesses"
                << " reason=no-ledger-anchored-validator-view"
                << " seq=" << seq
                << " candidateExportTxns=" << consensusExportTxns_.size();
        }
    }

    //@@start accept-time-cleanup-success
    // Export's ledger-defining signature witness is now in the tx stream.
    // After this point build/replay must use the pre-scanned pseudo, not
    // ephemeral sidecar state retained from consensus establish.
    clearRngStatePreservingExport();
    //@@end accept-time-cleanup-success
}

void
ConsensusExtensions::harvestRngData(
    NodeID const& nodeId,
    PublicKey const& publicKey,
    ExtendedPosition const& position,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger,
    Slice const& signature)
{
    JLOG(j_.trace()) << "RNG: harvestRngData"
                     << " node=" << nodeId
                     << " commit=" << (position.myCommitment ? "yes" : "no")
                     << " reveal=" << (position.myReveal ? "yes" : "no")
                     << " proposeSeq=" << proposeSeq
                     << " prevLedger=" << prevLedger;

    //@@start runtime-rng-claim-drop
    // RuntimeConfig: randomly drop RNG claims for testing
    auto& rc = app_.getRuntimeConfig();
    if (rc.active())
    {
        if (auto cfg = rc.getConsensusTestConfig())
        {
            if (cfg->rngClaimDropPctX100 && *cfg->rngClaimDropPctX100 > 0)
            {
                static thread_local std::mt19937 rng{std::random_device{}()};
                if (std::uniform_int_distribution<int>{0, 9999}(rng) <
                    *cfg->rngClaimDropPctX100)
                {
                    JLOG(j_.warn())
                        << "RNG: TESTING dropping claim"
                        << " node=" << nodeId
                        << " dropPctX100=" << *cfg->rngClaimDropPctX100
                        << " proposeSeq=" << proposeSeq;
                    return;
                }
            }
        }
    }
    //@@end runtime-rng-claim-drop

    //@@start rng-harvest-commit
    // Harvest commitment if present
    if (position.myCommitment)
    {
        std::optional<ProposalProof> proof;
        if (proposeSeq == 0)
        {
            ProposalProof p;
            p.proposeSeq = proposeSeq;
            p.closeTime = static_cast<std::uint32_t>(
                closeTime.time_since_epoch().count());
            p.prevLedger = prevLedger;
            Serializer s;
            position.add(s);
            p.positionData = std::move(s);
            p.signature = Buffer(signature.data(), signature.size());
            proof = std::move(p);
        }

        if (ingestRngContribution(
                nodeId,
                publicKey,
                RngContributionKind::commit,
                *position.myCommitment,
                std::nullopt,
                proof,
                "proposal",
                RngProofCachePolicy::keepExisting))
        {
            JLOG(j_.trace())
                << "RNG: harvested commitment"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " commitment=" << *position.myCommitment;
        }
    }
    //@@end rng-harvest-commit

    //@@start rng-harvest-reveal-verification
    // Harvest reveal if present — verify it matches the stored commitment
    if (position.myReveal)
    {
        if (rc.active())
        {
            if (auto cfg = rc.getConsensusTestConfig())
            {
                if (cfg->rngRevealDropPctX100 && *cfg->rngRevealDropPctX100 > 0)
                {
                    static thread_local std::mt19937 rng{
                        std::random_device{}()};
                    if (std::uniform_int_distribution<int>{0, 9999}(rng) <
                        *cfg->rngRevealDropPctX100)
                    {
                        JLOG(j_.warn())
                            << "RNG: TESTING dropping reveal claim"
                            << " node=" << nodeId
                            << " dropPctX100=" << *cfg->rngRevealDropPctX100
                            << " proposeSeq=" << proposeSeq;
                        return;
                    }
                }
            }
        }

        // Verify Hash(reveal | pubKey | seq) == commitment
        auto const prevLgr = app_.getLedgerMaster().getLedgerByHash(prevLedger);
        if (!prevLgr)
        {
            JLOG(j_.warn())
                << "RNG: cannot verify reveal"
                << " reason=prev-ledger-unavailable"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " prevLedger=" << prevLedger;
            return;
        }

        auto const seq = prevLgr->info().seq + 1;

        if (ingestRngContribution(
                nodeId,
                publicKey,
                RngContributionKind::reveal,
                *position.myReveal,
                seq,
                std::nullopt,
                "proposal",
                RngProofCachePolicy::keepExisting))
        {
            JLOG(j_.trace())
                << "RNG: harvested reveal"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " seq=" << seq << " reveal=" << *position.myReveal;
        }
    }
    //@@end rng-harvest-reveal-verification
}

Blob
ConsensusExtensions::serializeProof(ProposalProof const& proof)
{
    Serializer s;
    s.add32(proof.proposeSeq);
    s.add32(proof.closeTime);
    s.addBitString(proof.prevLedger);
    s.addVL(proof.positionData.slice());
    s.addVL(Slice(proof.signature.data(), proof.signature.size()));
    return s.getData();
}

std::optional<ConsensusExtensions::ProposalProof>
ConsensusExtensions::deserializeProof(Blob const& proofBlob)
{
    try
    {
        SerialIter sit(makeSlice(proofBlob));

        ProposalProof proof;
        proof.proposeSeq = sit.get32();
        proof.closeTime = sit.get32();
        proof.prevLedger = sit.get256();

        auto const positionData = sit.getVL();
        auto const signature = sit.getVL();

        if (!sit.empty())
            return std::nullopt;

        proof.positionData =
            Serializer(positionData.data(), positionData.size());
        proof.signature = Buffer(signature.data(), signature.size());
        return proof;
    }
    catch (std::exception const&)
    {
        return std::nullopt;
    }
}

bool
ConsensusExtensions::verifyProof(
    Blob const& proofBlob,
    PublicKey const& publicKey,
    uint256 const& expectedDigest,
    bool isCommit)
{
    try
    {
        auto const proof = deserializeProof(proofBlob);
        if (!proof)
            return false;

        auto const positionData = proof->positionData.slice();

        // Deserialize ExtendedPosition from the proof
        SerialIter posIter(positionData);
        auto maybePos =
            ExtendedPosition::fromSerialIter(posIter, positionData.size());
        if (!maybePos)
            return false;
        auto position = std::move(*maybePos);

        // Verify the expected digest matches the position's leaf
        if (isCommit)
        {
            if (!position.myCommitment ||
                *position.myCommitment != expectedDigest)
                return false;
        }
        else
        {
            if (!position.myReveal || *position.myReveal != expectedDigest)
                return false;
        }

        // Recompute the signing hash (must match
        // ConsensusProposal::signingHash)
        auto signingHash = sha512Half(
            HashPrefix::proposal,
            proof->proposeSeq,
            proof->closeTime,
            proof->prevLedger,
            position);

        // Use the proposal verifier rather than calling verifyDigest directly:
        // proposal-proof bytes are part of the authenticated snapshot.
        return verifyProposalDigest(
            publicKey,
            signingHash,
            Slice(proof->signature.data(), proof->signature.size()));
    }
    catch (std::exception const&)
    {
        return false;
    }
}

void
ConsensusExtensions::onRoundStart(
    RCLCxLedger const& prevLedger,
    hash_set<NodeID> lastProposers)
{
    clearRngState();
    auto const& rules = prevLedger.ledger_->rules();
    setRngEnabledThisRound(rules.enabled(featureConsensusEntropy));
    setExportEnabledThisRound(rules.enabled(featureExport));

    roundPrevLedgerHash_ = prevLedger.ledger_->info().hash;
    rngRoundSeq_ = prevLedger.ledger_->info().seq + 1;
    cacheUNLReport(prevLedger.ledger_);
    auto const validatorView = activeValidatorView();
    if (validatorView->sourceLedgerHash)
    {
        XRPL_ASSERT(
            *validatorView->sourceLedgerHash == roundPrevLedgerHash_,
            "ripple::ConsensusExtensions::onRoundStart : "
            "active view source matches round parent");
    }
    setExpectedProposers(std::move(lastProposers));
    resetSubState();
}

void
ConsensusExtensions::onTrustedPeerProposal(
    NodeID const& nodeId,
    PublicKey const& publicKey,
    ExtendedPosition const& position,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger,
    Slice const& signature,
    std::vector<std::string> const& exportSignatures)
{
    // Cluster peers may relay proposals that failed the overlay signature
    // check. Extension sidecars become ledger inputs, so harvest them only
    // after re-checking the proposal proof against the claimed validator key.
    auto const signingHash = sha512Half(
        HashPrefix::proposal,
        proposeSeq,
        closeTime.time_since_epoch().count(),
        prevLedger,
        position);
    if (!verifyProposalDigest(publicKey, signingHash, signature))
    {
        JLOG(j_.debug()) << "ConsensusExtensions: ignoring unsigned proposal "
                            "sidecars"
                         << " node=" << nodeId << " proposeSeq=" << proposeSeq;
        return;
    }

    harvestRngData(
        nodeId,
        publicKey,
        position,
        proposeSeq,
        closeTime,
        prevLedger,
        signature);

    // Stored future/wrong-ledger proposals are replayed through this path, not
    // through PeerImp's original protobuf packet. Re-harvest signed export
    // blobs here so they are evaluated against the active view for this parent.
    if (!exportSignatures.empty() && position.exportSignaturesHash &&
        proposalExportSignaturesHash(exportSignatures) ==
            *position.exportSignaturesHash)
    {
        harvestExportSignatures(
            publicKey, prevLedger, exportSignatures, "stored proposal");
    }
}

void
ConsensusExtensions::appendJson(Json::Value& ret) const
{
    using Int = Json::Value::Int;
    Json::Value rng(Json::objectValue);

    rng["enabled"] = rngEnabled();

    auto estStateName = [&]() -> char const* {
        switch (estState_)
        {
            case EstablishState::ConvergingTx:
                return "ConvergingTx";
            case EstablishState::ConvergingCommit:
                return "ConvergingCommit";
            case EstablishState::ConvergingReveal:
                return "ConvergingReveal";
        }
        return "Unknown";
    };
    rng["est_state"] = estStateName();
    rng["commits"] = static_cast<Int>(pendingCommitCount());
    rng["quorum"] = static_cast<Int>(quorumThreshold());
    rng["commit_quorum"] = hasQuorumOfCommits();
    rng["min_reveals"] = hasMinimumReveals();
    rng["any_reveals"] = hasAnyReveals();
    rng["reveals"] = static_cast<Int>(pendingRevealCount());
    rng["likely_participants"] = static_cast<Int>(expectedProposerCount());
    rng["observed_active_participants"] =
        static_cast<Int>(observedParticipantCount());
    if (observedParticipantsHash_)
    {
        rng["observed_participants"] = to_string(*observedParticipantsHash_);
        rng["observed_participants_bitmap"] = observedParticipantsBitmapBin_;
    }

    ret["rng"] = std::move(rng);
}

void
ConsensusExtensions::logPosition(
    ExtendedPosition const& pos,
    beast::Journal j,
    beast::severities::Severity level) const
{
    if (!j.active(level))
        return;

    j.stream(level) << "STALLDIAG: position-sidecar"
                    << " commitSetHash="
                    << (pos.commitSetHash ? to_string(*pos.commitSetHash)
                                          : std::string{"none"})
                    << " entropySetHash="
                    << (pos.entropySetHash ? to_string(*pos.entropySetHash)
                                           : std::string{"none"})
                    << " exportSigSetHash="
                    << (pos.exportSigSetHash ? to_string(*pos.exportSigSetHash)
                                             : std::string{"none"})
                    << " exportSignaturesHash="
                    << (pos.exportSignaturesHash
                            ? to_string(*pos.exportSignaturesHash)
                            : std::string{"none"})
                    << " observedParticipantsHash="
                    << (pos.observedParticipantsHash
                            ? to_string(*pos.observedParticipantsHash)
                            : std::string{"none"})
                    << " myCommitment=" << (pos.myCommitment ? "yes" : "no")
                    << " myReveal=" << (pos.myReveal ? "yes" : "no");
}

std::size_t
ConsensusExtensions::harvestExportSignatures(
    PublicKey const& senderPK,
    uint256 const& proposalPrevLedger,
    std::vector<std::string> const& exportSignatures,
    char const* source)
{
    if (!exportEnabled())
        return 0;

    if (exportSignatures.empty())
        return 0;

    auto const validatorView = activeValidatorView();
    // Proposal ingress is outside the consensus mutex, so take a snapshot of
    // the shared active view and reject trusted-but-inactive signers here.
    auto const exportTxns = buildOpenLedgerExportTxnLookup(app_);
    auto const currentSeq = currentClosedLedgerSeq(app_);

    return ripple::harvestExportSignatures(
        ExportSignatureHarvestInput{
            senderPK,
            proposalPrevLedger,
            exportSignatures,
            validatorView->sourceLedgerHash,
            activeSignerFilter(*this, validatorView),
            exportTxns,
            currentSeq,
            source,
            ExportLimits::maxPendingExports},
        exportSigCollector_,
        j_);
}

//@@start peer-harvest-export-sigs
void
ConsensusExtensions::onTrustedPeerMessage(
    ::protocol::TMProposeSet const& wireMsg)
{
    if (wireMsg.exportsignatures_size() == 0)
        return;

    auto const senderSlice = makeSlice(wireMsg.nodepubkey());
    if (!publicKeyType(senderSlice))
        return;
    PublicKey const senderPK{senderSlice};

    if (wireMsg.previousledger().size() != uint256::size())
        return;

    auto const positionSlice = makeSlice(wireMsg.currenttxhash());
    SerialIter positionSit{positionSlice};
    auto const position = ExtendedPosition::fromSerialIter(
        positionSit, wireMsg.currenttxhash().size());
    if (!position || !position->exportSignaturesHash)
        return;

    uint256 proposalPrevLedger;
    std::memcpy(
        proposalPrevLedger.data(),
        wireMsg.previousledger().data(),
        uint256::size());

    std::vector<std::string> exportSignatures;
    exportSignatures.reserve(wireMsg.exportsignatures_size());
    for (int i = 0; i < wireMsg.exportsignatures_size(); ++i)
    {
        if (wireMsg.exportsignatures(i).size() >
            ExportLimits::maxExportSignatureBytes)
            return;
        exportSignatures.push_back(wireMsg.exportsignatures(i));
    }

    // The raw protobuf field is not signed directly; the ExtendedPosition
    // digest is. Keep this check local so every harvesting path enforces the
    // same binding, even when called outside PeerImp's proposal precheck.
    if (proposalExportSignaturesHash(exportSignatures) !=
        *position->exportSignaturesHash)
        return;

    harvestExportSignatures(
        senderPK, proposalPrevLedger, exportSignatures, "wire proposal");
}
//@@end peer-harvest-export-sigs

//@@start rng-bootstrap-commitment
void
ConsensusExtensions::decoratePosition(
    ExtendedPosition& pos,
    std::shared_ptr<Ledger const> const& prevLedger,
    bool proposing)
{
    if (!proposing || !prevLedger->rules().enabled(featureConsensusEntropy))
    {
        JLOG(j_.debug())
            << "RNG: decoratePosition skipped"
            << " proposing=" << (proposing ? "yes" : "no") << " amendment="
            << (prevLedger->rules().enabled(featureConsensusEntropy) ? "yes"
                                                                     : "no")
            << " prevLedgerSeq=" << prevLedger->info().seq
            << " prevLedger=" << prevLedger->info().hash;
        return;
    }

    auto const& valKeys = app_.getValidatorKeys();
    if (!valKeys.keys || valKeys.nodeID == beast::zero)
    {
        // Only validators author RNG sidecars. Observers still consume peer
        // sidecars and diagnostics, but never seed local commit/reveal state.
        JLOG(j_.debug()) << "RNG: decoratePosition skipped"
                         << " reason=no-validator-key"
                         << " prevLedgerSeq=" << prevLedger->info().seq
                         << " prevLedger=" << prevLedger->info().hash;
        return;
    }

    setMode(ConsensusMode::proposing);
    cacheUNLReport(prevLedger);
    generateEntropySecret();

    pos.myCommitment = entropyCommitment(
        getEntropySecret(),
        valKeys.keys->publicKey,
        prevLedger->info().seq + 1);

    // Seed our own commitment into pendingCommits_ so we count
    // toward quorum (harvestRngData only sees peer proposals).
    pendingCommits_[valKeys.nodeID] = *pos.myCommitment;
    nodeIdToKey_.insert_or_assign(valKeys.nodeID, valKeys.keys->publicKey);

    JLOG(j_.info()) << "RNG: decoratePosition bootstrap"
                    << " seq=" << (prevLedger->info().seq + 1)
                    << " prevLedger=" << prevLedger->info().hash
                    << " node=" << valKeys.nodeID
                    << " commitment=" << *pos.myCommitment;
}
//@@end rng-bootstrap-commitment

//@@start export-sig-attachment
void
ConsensusExtensions::attachExportSignatures(
    protocol::TMProposeSet& prop,
    RCLCxPeerPos::Proposal const& proposal)
{
    auto const& valKeys = app_.getValidatorKeys();

    if (!exportEnabled())
        return;

    // Attach export signatures for any ttEXPORT txns in the open ledger.
    // Gated on featureExport amendment.
    //@@start runtime-export-no-sig
    // RuntimeConfig no_export_sig disables sig attachment (testing sub-quorum).
    {
        auto& rc = app_.getRuntimeConfig();
        if (rc.active())
        {
            if (auto cfg = rc.getConsensusTestConfig())
            {
                if (cfg->noExportSig && *cfg->noExportSig)
                {
                    JLOG(j_.debug()) << "Export: skipping proposal signatures"
                                     << " reason=runtime-config-noExportSig";
                    return;
                }
            }
        }
    }
    //@@end runtime-export-no-sig

    auto const openLedger = app_.openLedger().current();
    if (!openLedger || !openLedger->rules().enabled(featureExport))
        return;

    if (!valKeys.keys || valKeys.nodeID == beast::zero)
    {
        // Export signatures are validator attestations. Non-validator nodes may
        // relay proposals, but must not advertise locally authored signatures.
        JLOG(j_.debug()) << "Export: skipping proposal signatures"
                         << " reason=no-validator-key";
        return;
    }

    auto const& valPK = valKeys.keys->publicKey;
    auto const& valSK = valKeys.keys->secretKey;
    // A locally configured validator may be trusted but not active for this
    // round; only active validators should advertise export signatures.
    if (!isActiveValidator(valPK))
        return;

    auto const signerAcctID = calcAccountID(valPK);
    std::uint8_t attached = 0;

    for (auto const& [stx, meta] : openLedger->txs)
    {
        if (!stx || !ExportLedgerOps::isPendingExportWorkTxn(*stx))
            continue;

        if (attached >= ExportLimits::maxPendingExports)
        {
            JLOG(j_.debug())
                << "Export: proposal signature attachment cap reached"
                << " max=" << +ExportLimits::maxPendingExports
                << " openLedgerSeq=" << openLedger->info().seq;
            break;
        }

        auto const txHash = stx->getTransactionID();

        // Only attach our sig on the first proposal this round, and only for
        // the bounded export sidecar candidate set.
        if (!exportSigCollector_.markSent(
                txHash, ExportLimits::maxPendingExports))
            continue;

        //@@start export-compute-proposal-sig
        Buffer sigBuf;
        if (stx->isFieldPresent(sfExportedTxn))
        {
            try
            {
                auto innerTx = ExportLedgerOps::innerExportedTx(*stx);
                if (!innerTx)
                {
                    JLOG(j_.warn())
                        << "Export: failed to sign inner tx"
                        << " txHash=" << txHash
                        << " openLedgerSeq=" << openLedger->info().seq
                        << " reason=inner-tx-parse-failed";
                }
                else
                {
                    auto sigData =
                        buildMultiSigningData(*innerTx, signerAcctID);
                    sigBuf = sign(valPK, valSK, sigData.slice());
                }
            }
            catch (std::exception const& e)
            {
                JLOG(j_.warn()) << "Export: failed to sign inner tx"
                                << " txHash=" << txHash
                                << " openLedgerSeq=" << openLedger->info().seq
                                << " error=" << e.what();
            }
        }
        //@@end export-compute-proposal-sig

        //@@start export-attach-wire-sigs
        Serializer s;
        s.addBitString(txHash);
        s.addRaw(valPK.slice());
        if (sigBuf.size() > 0)
            s.addRaw(Slice(sigBuf.data(), sigBuf.size()));
        prop.add_exportsignatures(s.peekData().data(), s.peekData().size());
        ++attached;
        //@@end export-attach-wire-sigs

        // Only store if we actually produced a signature.
        // sigBuf is empty if the inner tx failed to deserialize.
        if (sigBuf.size() > 0)
            exportSigCollector_.addVerifiedSignature(
                txHash, valPK, sigBuf, openLedger->info().seq);

        JLOG(j_.debug()) << "Export: attached proposal signature"
                         << " txHash=" << txHash
                         << " signer=" << calcNodeID(valPK)
                         << " openLedgerSeq=" << openLedger->info().seq
                         << " sigLen=" << sigBuf.size()
                         << " attached=" << +attached;
    }
}
//@@end export-sig-attachment

void
ConsensusExtensions::decorateMessage(
    protocol::TMProposeSet&,
    RCLCxPeerPos::Proposal const& proposal,
    ExtendedPosition const& signedPosition,
    Buffer const& proposalSig)
{
    auto const& valKeys = app_.getValidatorKeys();
    if (!valKeys.keys || valKeys.nodeID == beast::zero)
        return;

    // Self-seed our own reveal so we count toward reveal quorum
    // (harvestRngData only sees peer proposals, not our own).
    if (signedPosition.myReveal)
    {
        pendingReveals_[valKeys.nodeID] = *signedPosition.myReveal;
        nodeIdToKey_.insert_or_assign(valKeys.nodeID, valKeys.keys->publicKey);
        JLOG(j_.trace()) << "RNG: self-seeded reveal"
                         << " node=" << valKeys.nodeID
                         << " proposeSeq=" << proposal.proposeSeq()
                         << " prevLedger=" << proposal.prevLedger();
    }

    // Store our own deterministic commit proof for commitSet entries.
    // Reveal sidecars deliberately omit proofs.
    if (signedPosition.myCommitment)
    {
        auto makeProof = [&]() {
            ProposalProof proof;
            proof.proposeSeq = proposal.proposeSeq();
            proof.closeTime = static_cast<std::uint32_t>(
                proposal.closeTime().time_since_epoch().count());
            proof.prevLedger = proposal.prevLedger();
            Serializer s;
            signedPosition.add(s);
            proof.positionData = std::move(s);
            proof.signature = Buffer(proposalSig.data(), proposalSig.size());
            return proof;
        };

        if (signedPosition.myCommitment && proposal.proposeSeq() == 0)
            commitProofs_.emplace(valKeys.nodeID, makeProof());
    }
}

ExtensionTickResult
ConsensusExtensions::onTick(TickContext const& ctx)
{
    if (exportEnabled())
    {
        cacheConsensusTxSet(ctx.getTxns());
        verifyPendingExportSigs(ctx.getTxns(), ctx.buildSeq);
    }
    else
    {
        consensusTxSetMap_.reset();
        consensusExportTxns_.clear();
        consensusTxSetHash_.reset();
    }

    return extensionsTick(*this, ctx);
}

}  // namespace ripple
