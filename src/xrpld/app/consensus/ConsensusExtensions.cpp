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
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/AmendmentTable.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/consensus/Consensus.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/random.h>
#include <xrpl/basics/scope.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/crypto/csprng.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SidecarType.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/ValidatorBitset.h>
#include <xrpl/protocol/digest.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>

namespace ripple {

ConsensusExtensions::ConsensusExtensions(Application& app, beast::Journal j)
    : app_(app), j_(j)
{
}

//------------------------------------------------------------------------------
// RNG Helper Methods

namespace {

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

Blob
buildEntropyContributorMask(
    std::vector<PublicKey> const& orderedMasterKeys,
    hash_set<NodeID> const& contributors)
{
    return makeValidatorBitset(orderedMasterKeys.size(), [&](std::size_t i) {
        return contributors.count(calcNodeID(orderedMasterKeys[i])) != 0;
    });
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
    char const* source)
{
    SerialIter sit(entry);
    STObject sidecar(sit, sfGeneric);

    if (!sidecar.isFieldPresent(sfSidecarType))
        return std::nullopt;

    auto const sidecarHash = sidecar.getHash(HashPrefix::sidecar);
    if (sidecarHash != itemKey)
    {
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

}  // namespace

std::size_t
ConsensusExtensions::quorumThreshold() const
{
    // Validator_quorum entropy uses a fixed 80% threshold over the effective
    // active UNL snapshot. Tier 2 participant_aligned entropy has its own
    // lower intersection-safe floor; recent proposers are useful for liveness
    // heuristics, but they do not lower either threshold.
    auto const base = activeValidatorView()->size();
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

    if (effectiveViewSize > 0 && participantCount == effectiveViewSize)
        return entropyTierValidatorFull;

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

    // A seq-0 proof makes the round's signing key authoritative for this
    // validator. A later manifest may legitimately rotate the live mapping,
    // but it must not retarget already accepted commit/reveal material.
    if (hasProofedCommit(nodeId))
    {
        auto const key = nodeIdToKey_.find(nodeId);
        if (key == nodeIdToKey_.end() || key->second != publicKey)
        {
            JLOG(j_.warn()) << "RNG: rejecting contribution"
                            << " reason=key-changed-after-proofed-commit"
                            << " kind=" << kindName << " source=" << sourceTag
                            << " node=" << nodeId;
            return false;
        }
    }

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

    //@@start rng-reveal-commitment-proof-gate
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
    //@@end rng-reveal-commitment-proof-gate

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
    uint256 const& buildTxSetHash,
    LedgerIndex seq) const
{
    //@@start entropy-selector-fallback
    // Tier 1 fallback: consensus-bound deterministic digest over already-agreed
    // round inputs. buildTxSetHash is the sanitized pre-injection live-build
    // set hash: the digest must never depend on a set that could contain a
    // supplied or derived extension pseudo-tx.
    auto const fallback = [&]() -> EntropySelection {
        return {
            sha512Half(
                HashPrefix::entropyFallback,
                roundPrevLedgerHash_,
                buildTxSetHash,
                seq),
            entropyTierConsensusFallback,
            0,
            0,
            {}};
    };
    //@@end entropy-selector-fallback

    // Standalone: synthetic deterministic entropy so the entropy_cr_* draws
    // work.
    //@@start entropy-selector-standalone
    if (app_.config().standalone())
    {
        auto const cfg = app_.getRuntimeConfig().getConsensusTestConfig();
        auto const tier = static_cast<std::uint8_t>(std::clamp(
            cfg && cfg->standaloneEntropyTier ? *cfg->standaloneEntropyTier
                                              : entropyTierValidatorFull,
            0,
            static_cast<int>(entropyTierValidatorFull)));
        auto const toU16 = [](int value) {
            return static_cast<std::uint16_t>(std::clamp(
                value,
                0,
                static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
        };
        auto const count = toU16(
            cfg && cfg->standaloneEntropyCount ? *cfg->standaloneEntropyCount
                                               : 20);
        auto const denominator = toU16(
            cfg && cfg->standaloneEntropyDenominator
                ? *cfg->standaloneEntropyDenominator
                : 20);
        Blob contributors((denominator + 7) / 8, 0);
        auto const contributorCount = std::min(count, denominator);
        for (std::uint16_t i = 0; i < contributorCount; ++i)
            contributors[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        return {
            sha512Half(std::string("standalone-entropy"), seq),
            tier,
            count,
            denominator,
            contributors};
    }
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

    //@@start entropy-selector-accepted-map-consumption
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
    // content-address/type contract as snapshot construction. Do not resolve
    // sfSigningPubKey through live manifests here: ingress already
    // authenticated the signing-key -> master-NodeID binding before the local
    // snapshot was built, and re-reading mutable manifests at materialization
    // would reintroduce a local-state fork surface.
    std::vector<std::pair<PublicKey, uint256>> sorted;
    hash_set<NodeID> contributors;
    hash_set<PublicKey> signingKeys;
    bool malformedContributorSet = false;
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
                if (!admitted)
                {
                    malformedContributorSet = true;
                    return;
                }
                if (admitted->type != sidecarRngReveal)
                {
                    malformedContributorSet = true;
                    return;
                }
                auto const& sidecar = admitted->sidecar;
                if (!sidecar.isFieldPresent(sfAccount))
                {
                    malformedContributorSet = true;
                    return;
                }
                auto const account = sidecar.getAccountID(sfAccount);
                auto const nodeId = NodeID::fromVoid(account.data());
                if (!validatorView->containsNode(nodeId))
                {
                    malformedContributorSet = true;
                    return;
                }
                if (!contributors.insert(nodeId).second)
                {
                    malformedContributorSet = true;
                    return;
                }

                auto const pk = sidecar.getFieldVL(sfSigningPubKey);
                if (!publicKeyType(makeSlice(pk)))
                {
                    malformedContributorSet = true;
                    return;
                }
                PublicKey const signingKey{makeSlice(pk)};
                if (!signingKeys.insert(signingKey).second)
                {
                    malformedContributorSet = true;
                    return;
                }
                sorted.emplace_back(signingKey, sidecar.getFieldH256(sfDigest));
            }
            catch (...)
            {
                malformedContributorSet = true;
            }
        });

    // Residual: the gate passed but no leaf parsed — fall back rather than
    // skip, so a fresh ConsensusEntropy input is always injected.
    if (sorted.empty())
        return fallback();
    if (malformedContributorSet || contributors.size() != sorted.size())
        return fallback();
    //@@end entropy-selector-accepted-map-consumption

    //@@start entropy-selector-normalize-digest-mask
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
    auto const contributorMask = buildEntropyContributorMask(
        validatorView->orderedMasterKeys, contributors);
    //@@end entropy-selector-normalize-digest-mask

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
        return {
            digest,
            static_cast<std::uint8_t>(tier),
            count,
            denominator,
            contributorMask};
    return fallback();
    //@@end entropy-selector-tier-ladder
}

bool
ConsensusExtensions::rngEnabled() const
{
    return rngEnabledThisRound_;
}

uint256
ConsensusExtensions::txnOrderingSalt(
    uint256 const& buildTxSetHash,
    LedgerIndex seq) const
{
    if (!rngEnabled())
        return buildTxSetHash;

    auto const selection = selectEntropy(buildTxSetHash, seq);
    return sha512Half(
        HashPrefix::entropyTxnOrder,
        buildTxSetHash,
        selection.digest,
        selection.tier,
        selection.count,
        selection.denominator);
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
    // Roots are advertised in proposals, but these maps are same-process
    // materialization caches, not peer-fetchable transaction-set candidates.
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
    // Roots are advertised in proposals, but these maps are same-process
    // materialization caches, not peer-fetchable transaction-set candidates.
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built entropySet SHAMap"
                     << " hash=" << hash << " seq=" << seq
                     << " entries=" << entryCount
                     << " pendingReveals=" << pendingReveals_.size()
                     << " activeValidators=" << validatorView->size();
    return hash;
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
ConsensusExtensions::clearRngState()
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
    observedParticipantsHash_.reset();
    observedParticipantsCount_ = 0;
    observedParticipantsBitmapBin_.clear();
    likelyParticipants_.clear();
    commitProofs_.clear();
    roundPrevLedgerHash_ = uint256{};
    //@@end round-stop-rng-reset
    // Keep the round-level enable latch intact here. onRoundStart() refreshes
    // it from the consensus parent.
}

void
ConsensusExtensions::onReplayBuild()
{
    clearRngState();
}
//@@end clear-rng-state

void
ConsensusExtensions::cacheUNLReport(
    std::shared_ptr<Ledger const> const& prevLedger)
{
    auto view = makeActiveValidatorView(prevLedger);
    auto const size = view->size();
    auto const fromUNLReport = view->fromUNLReport;
    activeValidatorView_ = std::move(view);

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
    // filter and the entropy thresholds.
    auto const& valKeys = app_.getValidatorKeys();
    if (!valKeys.keys || valKeys.nodeID == beast::zero)
        return false;
    return activeValidatorView()->containsNode(valKeys.nodeID);
}

ConsensusExtensions::ActiveValidatorViewPtr
ConsensusExtensions::activeValidatorView() const
{
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
    //@@start participant-diagnostics-feature-gate
    if (!rngEnabled())
        return;

    if (observedParticipantsHash_)
        pos.observedParticipantsHash = observedParticipantsHash_;
    //@@end participant-diagnostics-feature-gate
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

ConsensusExtensions::LiveBuildTxSet
ConsensusExtensions::makeLiveBuildTxSet(RCLTxSet const& agreedTxs) const
{
    RCLTxSet::MutableTxSet mutableSet{agreedTxs};
    std::vector<uint256> suppliedEntropy;

    for (auto const& item : *agreedTxs.map_)
    {
        try
        {
            STTx const tx{SerialIter{item.slice()}};
            if (tx.getTxnType() == ttCONSENSUS_ENTROPY)
                suppliedEntropy.push_back(item.key());
        }
        catch (std::exception const&)
        {
            // Preserve malformed entries here. The existing canonical-set
            // construction path records their parse failure separately.
        }
    }

    for (auto const& id : suppliedEntropy)
        mutableSet.erase(id);

    return LiveBuildTxSet{RCLTxSet{mutableSet}, suppliedEntropy.size()};
}

void
ConsensusExtensions::onPreBuild(
    CanonicalTXSet& retriableTxs,
    LedgerIndex seq,
    uint256 const& buildTxSetHash)
{
    //@@start extension-live-pseudo-authority
    // The agreed user transaction set never authorizes synthetic extension
    // state. In a live build, discard every supplied extension pseudo and
    // derive the canonical synthetic stream from accepted extension evidence.
    // Ledger replay bypasses onPreBuild and consumes its persisted order.
    std::size_t suppliedEntropy = 0;
    for (auto it = retriableTxs.begin(); it != retriableTxs.end();)
    {
        auto const& tx = it->second;
        if (tx && tx->getTxnType() == ttCONSENSUS_ENTROPY)
        {
            ++suppliedEntropy;
            it = retriableTxs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (suppliedEntropy != 0)
    {
        JLOG(j_.error())
            << "ConsensusExtensions: discarded supplied synthetic txs"
            << " seq=" << seq << " entropy=" << suppliedEntropy;
    }
    //@@end extension-live-pseudo-authority

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
        // same entropy for the same accepted round inputs. buildTxSetHash is
        // the sanitized pre-injection live-build set hash.
        auto const selection = selectEntropy(buildTxSetHash, seq);
        uint256 const finalEntropy = selection.digest;
        std::uint8_t const entropyTier = selection.tier;
        std::uint16_t const entropyCount = selection.count;
        std::uint16_t const entropyDenominator = selection.denominator;
        Blob const& entropyContributors = selection.contributors;
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
                obj.setFieldVL(sfEntropyContributors, entropyContributors);
                obj.setFieldU8(sfEntropyTier, entropyTier);
            });

            retriableTxs.insert(std::make_shared<STTx>(std::move(tx)));
            //@@end rng-inject-pseudotx-core
        }
        //@@end rng-inject-pseudotx
    }

    // The next onRoundStart clears this round's extension state.
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
    auto const& rules = prevLedger.ledger_->rules();
    //@@start round-extension-feature-latches
    setRngEnabledThisRound(rules.enabled(featureConsensusEntropy));
    //@@end round-extension-feature-latches
    clearRngState();

    roundPrevLedgerHash_ = prevLedger.ledger_->info().hash;
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
    Slice const& signature)
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
                    << " observedParticipantsHash="
                    << (pos.observedParticipantsHash
                            ? to_string(*pos.observedParticipantsHash)
                            : std::string{"none"})
                    << " myCommitment=" << (pos.myCommitment ? "yes" : "no")
                    << " myReveal=" << (pos.myReveal ? "yes" : "no");
}

//@@start rng-bootstrap-commitment
void
ConsensusExtensions::decoratePosition(
    ExtendedPosition& pos,
    std::shared_ptr<Ledger const> const& prevLedger,
    bool proposing)
{
    //@@start rng-decorate-position-feature-gate
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
    //@@end rng-decorate-position-feature-gate

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
    return extensionsTick(*this, ctx);
}

}  // namespace ripple
