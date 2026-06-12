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
#include <xrpld/consensus/Consensus.h>
#include <xrpld/consensus/ConsensusExtensionsTick.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/shamap/SHAMap.h>
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
#include <random>

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

ExportTxnLookup
buildExportTxnLookup(SHAMap const& txns, beast::Journal j)
{
    ExportTxnLookup exportTxns;
    txns.visitLeaves([&](boost::intrusive_ptr<SHAMapItem const> const& item) {
        try
        {
            SerialIter sit(item->slice());
            auto stx = std::make_shared<STTx const>(sit);
            if (stx->getTxnType() == ttEXPORT)
                exportTxns.emplace(stx->getTransactionID(), std::move(stx));
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
        if (stx && stx->getTxnType() == ttEXPORT)
            exportTxns.emplace(stx->getTransactionID(), stx);
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

char const*
sidecarKindName(ConsensusExtensions::SidecarKind kind)
{
    switch (kind)
    {
        case ConsensusExtensions::SidecarKind::commit:
            return "commit";
        case ConsensusExtensions::SidecarKind::reveal:
            return "reveal";
        case ConsensusExtensions::SidecarKind::exportSig:
            return "exportSig";
    }
    return "unknown";
}

}  // namespace

std::size_t
ConsensusExtensions::quorumThreshold() const
{
    // Non-zero entropy is only allowed once a fixed 80% quorum of the active
    // UNL snapshot has committed. Recent proposers are useful for liveness
    // heuristics, but they do not lower this floor.
    // Use the shared validator view so RNG and Export use the same denominator.
    auto const base = activeValidatorView()->size();
    if (base == 0)
        return 1;  // safety: need at least one commit
    return calculateQuorumThreshold(base);
}

std::size_t
ConsensusExtensions::exportSigQuorumThreshold() const
{
    auto const base = activeValidatorView()->size();
    if (base == 0)
        return 1;

    // Export sidecar hashes are signed through ExtendedPosition even when RNG
    // is disabled, so a quorum-aligned exportSigSetHash is deterministic
    // enough for Export-only mode. Unanimity would let one active validator
    // veto an otherwise converged export round.
    return calculateQuorumThreshold(base);
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

std::size_t
ConsensusExtensions::pendingRevealCount() const
{
    return pendingReveals_.size();
}

std::size_t
ConsensusExtensions::expectedProposerCount() const
{
    return likelyParticipants_.size();
}

bool
ConsensusExtensions::hasQuorumOfCommits() const
{
    auto const validatorView = activeValidatorView();
    auto const threshold = validatorView->size() == 0
        ? std::size_t{1}
        : calculateQuorumThreshold(validatorView->size());
    auto const proofedCommitCount = std::count_if(
        pendingCommits_.begin(),
        pendingCommits_.end(),
        [this, validatorView](auto const& entry) {
            auto const& nid = entry.first;
            // Commit quorum only counts entries that can be emitted as
            // verifiable sidecar leaves under the shared active view.
            return validatorView->containsNode(nid) &&
                nodeIdToKey_.count(nid) > 0 && commitProofs_.count(nid) > 0;
        });
    bool result = static_cast<std::size_t>(proofedCommitCount) >= threshold;
    JLOG(j_.trace()) << "RNG: commit quorum check"
                     << " proofedCommits=" << proofedCommitCount
                     << " threshold=" << threshold
                     << " result=" << (result ? "yes" : "no")
                     << " pendingCommits=" << pendingCommits_.size()
                     << " activeValidators=" << validatorView->size()
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
    auto const validatorView = activeValidatorView();
    // Reveal quorum targets the commit sidecar set, not every later proposal
    // commitment we heard. That keeps proofless late commits from extending
    // the reveal wait after they were excluded from buildCommitSet().
    auto const expected = std::count_if(
        pendingCommits_.begin(),
        pendingCommits_.end(),
        [this, validatorView](auto const& entry) {
            auto const& nid = entry.first;
            return validatorView->containsNode(nid) &&
                nodeIdToKey_.count(nid) > 0 && commitProofs_.count(nid) > 0;
        });
    auto const revealCount = std::count_if(
        pendingReveals_.begin(),
        pendingReveals_.end(),
        [this, validatorView](auto const& entry) {
            auto const& nid = entry.first;
            return validatorView->containsNode(nid) &&
                pendingCommits_.count(nid) > 0 && commitProofs_.count(nid) > 0;
        });
    bool result = revealCount >= expected;
    JLOG(j_.trace()) << "RNG: reveal quorum check"
                     << " reveals=" << revealCount << " expected=" << expected
                     << " result=" << (result ? "yes" : "no")
                     << " pendingReveals=" << pendingReveals_.size()
                     << " pendingCommits=" << pendingCommits_.size()
                     << " activeValidators=" << validatorView->size();
    return result;
}

bool
ConsensusExtensions::hasAnyReveals() const
{
    return !pendingReveals_.empty();
}

bool
ConsensusExtensions::shouldZeroEntropy() const
{
    if (entropyFailed_ || !entropySetMap_)
        return true;

    auto const leafCount =
        std::distance(entropySetMap_->begin(), entropySetMap_->end());
    return leafCount == 0 || leafCount < quorumThreshold();
}

bool
ConsensusExtensions::rngEnabled() const
{
    return rngEnabledThisRound_;
}

bool
ConsensusExtensions::exportEnabled() const
{
    return exportEnabledThisRound_;
}

bool
ConsensusExtensions::bootstrapFastStartEnabled() const
{
    auto const cfg = app_.getRuntimeConfig().getConfig("*");
    if (cfg && cfg->bootstrapFastStart.has_value())
        return *cfg->bootstrapFastStart;
    return false;
}

bool
ConsensusExtensions::shouldSendExplicitFinalProposal() const
{
    // Explicit-final-proposal policy is node-local and experimental.
    //
    // Default behavior is implicit finalization (no extra seq=4 proposal):
    // entropy pseudo-tx is injected in onAccept/buildLCL.
    //
    // We only enable explicit-final when operators intentionally opt in via
    // runtime config/env for measurement/diagnostics.
    //
    // TBD (2026-03-03): Keep collecting tx-bearing network data before
    // revisiting whether explicit-final can be safely promoted beyond
    // experimental use.
    auto const cfg = app_.getRuntimeConfig().getConfig("*");
    if (cfg && cfg->explicitFinalProposal.has_value())
        return *cfg->explicitFinalProposal;
    return false;
}

std::optional<RCLTxSet>
ConsensusExtensions::buildExplicitFinalProposalTxSet(
    RCLTxSet const& txns,
    LedgerIndex seq)
{
    JLOG(j_.debug()) << "RNGFINAL: build synthetic txSet"
                     << " baseTxSet=" << txns.id() << " seq=" << seq
                     << " commits=" << pendingCommits_.size()
                     << " reveals=" << pendingReveals_.size()
                     << " entropyFailed=" << (entropyFailed_ ? "yes" : "no");

    uint256 finalEntropy;
    bool hasEntropy = false;
    std::uint8_t entropyTier = entropyTierNone;
    std::uint16_t entropyCount = 0;

    // Tier 3 fallback over already-agreed round inputs. Uses the BASE tx
    // set hash (txns.id()) — never the synthetic set's own hash (circular).
    auto const fallbackEntropy = [&] {
        return sha512Half(
            HashPrefix::entropyFallback, roundPrevLedgerHash_, txns.id(), seq);
    };

    // Keep this entropy-selection logic aligned with onPreBuild().
    // If these paths drift, different nodes can derive different synthetic
    // hashes for the same round, which is especially harmful because this
    // path mutates proposal tx-set identity late in establish.
    if (app_.config().standalone())
    {
        finalEntropy = sha512Half(std::string("standalone-entropy"), seq);
        hasEntropy = true;
        entropyTier = entropyTierValidatorQuorum;
        entropyCount = 20;
    }
    else if (shouldZeroEntropy())
    {
        finalEntropy = fallbackEntropy();
        hasEntropy = true;
        entropyTier = entropyTierConsensusFallback;
        entropyCount = 0;
    }
    else
    {
        std::vector<std::pair<PublicKey, uint256>> sorted;
        sorted.reserve(pendingReveals_.size());

        for (auto const& [nodeId, reveal] : pendingReveals_)
        {
            auto it = nodeIdToKey_.find(nodeId);
            if (it != nodeIdToKey_.end())
                sorted.emplace_back(it->second, reveal);
        }

        if (!sorted.empty())
        {
            std::sort(
                sorted.begin(), sorted.end(), [](auto const& a, auto const& b) {
                    return a.first.slice() < b.first.slice();
                });

            Serializer s;
            for (auto const& [key, reveal] : sorted)
            {
                s.addVL(key.slice());
                s.addBitString(reveal);
            }
            finalEntropy = sha512Half(s.slice());
            hasEntropy = true;
            entropyTier = entropyTierValidatorQuorum;
            entropyCount = static_cast<std::uint16_t>(sorted.size());
        }
    }

    if (!hasEntropy)
    {
        // Residual (no usable reveals): fall back rather than skipping, so
        // the synthetic set always carries a fresh entropy pseudo-tx.
        finalEntropy = fallbackEntropy();
        hasEntropy = true;
        entropyTier = entropyTierConsensusFallback;
        entropyCount = 0;
        JLOG(j_.debug()) << "RNGFINAL: fallback entropy for synthetic txSet"
                         << " baseTxSet=" << txns.id() << " seq=" << seq;
    }

    STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
        obj.setFieldU32(sfLedgerSequence, seq);
        obj.setAccountID(sfAccount, AccountID{});
        obj.setFieldU32(sfSequence, 0);
        obj.setFieldAmount(sfFee, STAmount{});
        obj.setFieldH256(sfDigest, finalEntropy);
        obj.setFieldU16(sfEntropyCount, entropyCount);
        obj.setFieldU8(sfEntropyTier, entropyTier);
    });

    auto const txID = tx.getTransactionID();
    // Dedup by type (mirrors onPreBuild): there must never be two entropy
    // pseudo-txs, and a fallback digest derived from a different base set
    // hash would not match by exact txID.
    bool alreadyPresent = false;
    txns.map_->visitLeaves(
        [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
            if (alreadyPresent)
                return;
            try
            {
                SerialIter sit(item->slice());
                STTx const parsed{sit};
                if (parsed.getTxnType() == ttCONSENSUS_ENTROPY)
                    alreadyPresent = true;
            }
            catch (...)
            {
            }
        });
    if (alreadyPresent)
    {
        JLOG(j_.debug()) << "RNGFINAL: entropy pseudo-tx already in base txSet"
                         << " txHash=" << txID << " baseTxSet=" << txns.id();
        return txns;
    }

    RCLTxSet::MutableTxSet mutableTxSet{txns};
    Serializer ser(512);
    tx.add(ser);
    mutableTxSet.insert(RCLCxTx{make_shamapitem(txID, ser.slice())});
    auto syntheticSet = RCLTxSet{mutableTxSet};
    auto const hash = syntheticSet.id();
    app_.getInboundTransactions().giveSet(hash, syntheticSet.map_, false);

    JLOG(j_.debug()) << "RNGFINAL: built synthetic txSet"
                     << " syntheticTxSet=" << hash << " baseTxSet=" << txns.id()
                     << " txHash=" << txID << " entropyCount=" << entropyCount;

    return syntheticSet;
}

uint256
ConsensusExtensions::buildCommitSet(LedgerIndex seq)
{
    //@@start rng-build-commit-set
    // Track the active RNG round explicitly. Nodes in observing/switching
    // mode can have a closed ledger index behind the consensus round while
    // still needing to fetch/merge that round's RNG sets.
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
        // the frozen validator view used by quorum calculation.
        if (!validatorView->containsNode(nid))
            continue;

        auto kit = nodeIdToKey_.find(nid);
        if (kit == nodeIdToKey_.end())
            continue;
        auto proofIt = commitProofs_.find(nid);
        if (proofIt == commitProofs_.end())
            continue;

        // Encode the NodeID into sfAccount so onAcquiredSidecarSet can
        // recover it without recomputing (master vs signing key issue).
        AccountID acctId;
        std::memcpy(acctId.data(), nid.data(), acctId.size());

        STObject sidecar(sfGeneric);
        sidecar.setFieldU8(sfSidecarType, sidecarRngCommit);
        sidecar.setFieldU32(sfLedgerSequence, seq);
        sidecar.setAccountID(sfAccount, acctId);
        sidecar.setFieldH256(sfDigest, commit);
        sidecar.setFieldVL(sfSigningPubKey, kit->second.slice());
        sidecar.setFieldVL(sfBlob, serializeProof(proofIt->second));

        auto const itemKey = sidecar.getHash(HashPrefix::sidecar);
        Serializer s(2048);
        sidecar.add(s);
        map->addItem(
            SHAMapNodeType::tnSIDECAR, make_shamapitem(itemKey, s.slice()));
        ++entryCount;
    }

    map = map->snapShot(false);
    commitSetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built commitSet SHAMap"
                     << " hash=" << hash << " seq=" << seq
                     << " entries=" << entryCount
                     << " pendingCommits=" << pendingCommits_.size()
                     << " activeValidators=" << validatorView->size();
    return hash;
    //@@end rng-build-commit-set
}

uint256
ConsensusExtensions::buildEntropySet(LedgerIndex seq)
{
    //@@start rng-build-entropy-set
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

        // Reveal sidecars must use the same validator view as the commit set
        // so timeout/fetch paths cannot expand the entropy participant set.
        if (!validatorView->containsNode(nid))
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
        // loss/reordering.  We only need deterministic reveal material
        // (validator identity + digest) for fetch/merge and entropy
        // calculation.

        auto const itemKey = sidecar.getHash(HashPrefix::sidecar);
        Serializer s(2048);
        sidecar.add(s);
        map->addItem(
            SHAMapNodeType::tnSIDECAR, make_shamapitem(itemKey, s.slice()));
        ++entryCount;
    }

    map = map->snapShot(false);
    entropySetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built entropySet SHAMap"
                     << " hash=" << hash << " seq=" << seq
                     << " entries=" << entryCount
                     << " pendingReveals=" << pendingReveals_.size()
                     << " activeValidators=" << validatorView->size();
    return hash;
    //@@end rng-build-entropy-set
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
        [this, validatorView](PublicKey const& key) {
            return isActiveValidator(key, *validatorView);
        });
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

            auto const itemKey = sidecar.getHash(HashPrefix::sidecar);
            Serializer s;
            sidecar.add(s);
            map->addItem(
                SHAMapNodeType::tnSIDECAR, make_shamapitem(itemKey, s.slice()));
            ++entryCount;
        }
    }

    map = map->snapShot(false);
    exportSigSetMap_ = map;

    auto const hash = map->getHash().as_uint256();
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
        [this, validatorView](PublicKey const& key) {
            return isActiveValidator(key, *validatorView);
        });
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
ConsensusExtensions::selfSeedReveal()
{
    auto const& valKeys = app_.getValidatorKeys();
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
    //@@start round-stop-export-reset
    exportSigCollector_.clearRound();
    if (auto const closed = app_.getLedgerMaster().getClosedLedger())
        exportSigCollector_.cleanupStale(closed->info().seq);
    if (!exportEnabledThisRound_)
    {
        // Export disabled is an amendment boundary, not a retry boundary.
        // Drop cached signatures so an emergency stop cannot leave old quorum
        // material waiting for a later re-enable.
        exportSigCollector_.clearAll();
    }
    //@@end round-stop-export-reset
    //@@start round-stop-rng-reset
    pendingCommits_.clear();
    pendingReveals_.clear();
    nodeIdToKey_.clear();
    myEntropySecret_ = uint256{};
    entropyFailed_ = false;
    commitSetMap_.reset();
    entropySetMap_.reset();
    exportSigSetMap_.reset();
    rngRoundSeq_.reset();
    consensusTxSetMap_.reset();
    consensusExportTxns_.clear();
    consensusTxSetHash_.reset();
    pendingRngFetches_.clear();
    observedParticipantsHash_.reset();
    observedParticipantsCount_ = 0;
    observedParticipantsBitmapBin_.clear();
    exportSigGateStarted_ = false;
    exportSigGateStart_ = {};
    exportSigConvergenceFailed_ = false;
    likelyParticipants_.clear();
    commitProofs_.clear();
    proposalProofs_.clear();
    //@@end round-stop-rng-reset
    // Keep the round-level enable latches intact here. Consensus::startRound()
    // calls preStartRound() first to snapshot which extensions are enabled for
    // the upcoming round, then immediately clears per-round working state.
    // Resetting these latches here would wipe that snapshot before
    // phaseEstablish() can consult it.
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
    auto const sourceLedger =
        prevLedger ? prevLedger : app_.getLedgerMaster().getValidatedLedger();
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

//@@start is-sidecar-set
bool
ConsensusExtensions::isSidecarSet(uint256 const& hash) const
{
    if (commitSetMap_ && commitSetMap_->getHash().as_uint256() == hash)
        return true;
    if (entropySetMap_ && entropySetMap_->getHash().as_uint256() == hash)
        return true;
    if (exportSigSetMap_ && exportSigSetMap_->getHash().as_uint256() == hash)
        return true;
    return pendingRngFetches_.find(hash) != pendingRngFetches_.end();
}
//@@end is-sidecar-set

//@@start handle-acquired-sidecar
//@@start handle-acquired-sidecar-entry
void
ConsensusExtensions::onAcquiredSidecarSet(std::shared_ptr<SHAMap> const& map)
{
    auto const hash = map->getHash().as_uint256();

    // Look up the expected kind before erasing.
    auto const kindIt = pendingRngFetches_.find(hash);
    auto const kind = (kindIt != pendingRngFetches_.end())
        ? kindIt->second
        : SidecarKind::commit;  // fallback for non-fetch paths
    if (kindIt != pendingRngFetches_.end())
        pendingRngFetches_.erase(kindIt);
    //@@end handle-acquired-sidecar-entry

    JLOG(j_.debug()) << "RNGFETCH: handle acquired"
                     << " hash=" << hash << " kind=" << sidecarKindName(kind)
                     << " pending-after-erase=" << pendingRngFetches_.size();

    // Dispatch by kind — no content-sniffing needed.
    // The kind was recorded at fetch time from the typed call site
    // (commitSetHash / entropySetHash / exportSigSetHash).
    if (kind == SidecarKind::exportSig)
    {
        // If we already have this exact export sig set, skip.
        if (exportSigSetMap_ &&
            exportSigSetMap_->getHash().as_uint256() == hash)
            return;

        {
            auto const useConsensusTxSet =
                static_cast<bool>(consensusTxSetMap_);
            auto const txSource =
                useConsensusTxSet ? "consensus tx set" : "open ledger";
            auto const openLedgerExportTxns = useConsensusTxSet
                ? ExportTxnLookup{}
                : buildOpenLedgerExportTxnLookup(app_);
            auto const& exportTxns =
                useConsensusTxSet ? consensusExportTxns_ : openLedgerExportTxns;
            auto const currentSeq = currentClosedLedgerSeq(app_);

            auto const validatorView = activeValidatorView();
            std::size_t merged = 0;
            map->visitLeaves(
                [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                    try
                    {
                        SerialIter sit(item->slice());
                        STObject sidecar(sit, sfGeneric);

                        // Enforce the self-describing type tag.
                        if (!sidecar.isFieldPresent(sfSidecarType) ||
                            sidecar.getFieldU8(sfSidecarType) !=
                                sidecarExportSig)
                            return;

                        if (!sidecar.isFieldPresent(sfTransactionHash) ||
                            !sidecar.isFieldPresent(sfSigningPubKey))
                            return;

                        auto const txHash =
                            sidecar.getFieldH256(sfTransactionHash);
                        auto const pk = sidecar.getFieldVL(sfSigningPubKey);
                        if (!publicKeyType(makeSlice(pk)))
                            return;

                        PublicKey const valPK{makeSlice(pk)};
                        // Fetched export sidecars are only useful if the signer
                        // is active in the same view that final quorum will
                        // use.
                        if (!isActiveValidator(valPK, *validatorView))
                            return;

                        // Require a real signature (not pubkey-only).
                        if (!sidecar.isFieldPresent(sfTxnSignature))
                            return;

                        // Skip if we already have a verified sig for this
                        // validator (e.g. from the proposal ingestion path).
                        if (exportSigCollector_.hasVerifiedSignature(
                                txHash, valPK))
                            return;

                        auto const sigVL = sidecar.getFieldVL(sfTxnSignature);
                        auto const sigSlice = makeSlice(sigVL);

                        auto const txIt = exportTxns.find(txHash);
                        if (txIt == exportTxns.end())
                        {
                            JLOG(j_.debug())
                                << "Export: SHAMap merge skipped"
                                << " reason=tx-not-found"
                                << " kind=" << sidecarKindName(kind)
                                << " hash=" << hash << " txHash=" << txHash
                                << " txSource=" << txSource;
                            return;
                        }

                        if (!verifyExportSignatureAgainstTx(
                                *txIt->second,
                                valPK,
                                sigSlice,
                                txHash,
                                j_,
                                txSource))
                            return;

                        Buffer sigBuf(sigSlice.data(), sigSlice.size());
                        exportSigCollector_.addVerifiedSignature(
                            txHash, valPK, sigBuf, currentSeq);
                        ++merged;
                    }
                    catch (std::exception const& e)
                    {
                        JLOG(j_.warn())
                            << "Export: SHAMap merge parse failed"
                            << " kind=" << sidecarKindName(kind)
                            << " hash=" << hash << " error=" << e.what();
                    }
                });
            JLOG(j_.info()) << "Export: merged peer exportSigSet"
                            << " hash=" << hash << " entriesMerged=" << merged
                            << " txSource=" << txSource
                            << " currentClosedSeq=" << currentSeq;
            return;
        }
    }

    enum class RngSetKind { commit, reveal };
    std::optional<RngSetKind> setKind;
    if (kind == SidecarKind::commit)
        setKind = RngSetKind::commit;
    else if (kind == SidecarKind::reveal)
        setKind = RngSetKind::reveal;

    if (!setKind)
    {
        JLOG(j_.warn()) << "RNGFETCH: acquired set rejected"
                        << " hash=" << hash << " kind=" << sidecarKindName(kind)
                        << " reason=unrecognized-rng-kind";
        return;
    }

    bool const isCommitSet = *setKind == RngSetKind::commit;
    JLOG(j_.debug()) << "RNGFETCH: classified"
                     << " hash=" << hash << " setKind="
                     << (isCommitSet ? "commitSet" : "entropySet")
                     << " fetchKind=" << sidecarKindName(kind);

    // Union-merge: diff against our local set and add any entries we're
    // missing. Unlike normal txSets which use avalanche voting to resolve
    // disagreements, RNG sets use pure union — every valid UNL entry
    // belongs in the set. Differences arise only from propagation timing,
    // not from conflicting opinions about inclusion.
    auto& localMap = isCommitSet ? commitSetMap_ : entropySetMap_;
    auto& pendingData = isCommitSet ? pendingCommits_ : pendingReveals_;

    std::size_t merged = 0;

    auto mergeEntry = [&](Slice const& entry, char const* sourceTag) {
        try
        {
            SerialIter sit(entry);
            STObject sidecar(sit, sfGeneric);

            if (!sidecar.isFieldPresent(sfSidecarType))
                return;

            auto const entryType = sidecar.getFieldU8(sfSidecarType);
            if ((isCommitSet && entryType != sidecarRngCommit) ||
                (!isCommitSet && entryType != sidecarRngReveal))
                return;

            auto const pk = sidecar.getFieldVL(sfSigningPubKey);
            PublicKey pubKey(makeSlice(pk));
            auto const digest = sidecar.getFieldH256(sfDigest);

            // Recover NodeID from sfAccount (encoded by
            // buildCommitSet/buildEntropySet) so we can compare against trusted
            // validator identity.
            auto const acctId = sidecar.getAccountID(sfAccount);
            NodeID nodeId;
            std::memcpy(nodeId.data(), acctId.data(), nodeId.size());

            if (!isUNLReportMember(nodeId))
            {
                JLOG(j_.debug())
                    << "RNG: rejecting acquired entry"
                    << " reason=non-active-validator"
                    << " kind=" << (isCommitSet ? "commit" : "reveal")
                    << " source=" << sourceTag << " node=" << nodeId
                    << " hash=" << hash;
                return;
            }

            // Bind the claimed nodeId to a trusted validator key identity.
            // This prevents a fetched set from impersonating arbitrary UNL
            // members via sfAccount.
            auto const trustedMaster = app_.validators().getTrustedKey(pubKey);
            if (!trustedMaster)
            {
                JLOG(j_.warn())
                    << "RNG: rejecting acquired entry"
                    << " reason=untrusted-signing-key"
                    << " kind=" << (isCommitSet ? "commit" : "reveal")
                    << " source=" << sourceTag << " node=" << nodeId
                    << " hash=" << hash;
                return;
            }
            if (calcNodeID(*trustedMaster) != nodeId)
            {
                JLOG(j_.warn())
                    << "RNG: rejecting acquired entry"
                    << " reason=node-key-mismatch"
                    << " kind=" << (isCommitSet ? "commit" : "reveal")
                    << " source=" << sourceTag << " node=" << nodeId
                    << " hash=" << hash;
                return;
            }

            std::optional<ProposalProof> parsedProof;
            if (sidecar.isFieldPresent(sfBlob))
            {
                auto const proofBlob = sidecar.getFieldVL(sfBlob);
                if (!verifyProof(proofBlob, pubKey, digest, isCommitSet))
                {
                    JLOG(j_.warn())
                        << "RNG: rejecting acquired entry"
                        << " reason=invalid-proof"
                        << " kind=" << (isCommitSet ? "commit" : "reveal")
                        << " source=" << sourceTag << " node=" << nodeId
                        << " hash=" << hash;
                    return;
                }
                parsedProof = deserializeProof(proofBlob);
                if (!parsedProof)
                {
                    JLOG(j_.warn())
                        << "RNG: rejecting acquired entry"
                        << " reason=malformed-proof"
                        << " kind=" << (isCommitSet ? "commit" : "reveal")
                        << " source=" << sourceTag << " node=" << nodeId
                        << " hash=" << hash;
                    return;
                }
            }
            else if (isCommitSet)
            {
                // Commit entries must carry a verifiable proposal proof.
                // Without this, an attacker could inject arbitrary digests
                // for trusted node IDs via fetched sets.
                JLOG(j_.warn()) << "RNG: rejecting acquired entry"
                                << " reason=missing-commit-proof"
                                << " kind=commit"
                                << " source=" << sourceTag << " node=" << nodeId
                                << " hash=" << hash;
                return;
            }

            auto const seq = sidecar.getFieldU32(sfLedgerSequence);
            auto const expectedSeq = [&]() -> std::optional<LedgerIndex> {
                if (rngRoundSeq_)
                    return rngRoundSeq_;
                if (auto const closed =
                        app_.getLedgerMaster().getClosedLedger())
                    return closed->info().seq + 1;
                return std::nullopt;
            }();
            if (expectedSeq && seq != *expectedSeq)
            {
                JLOG(j_.debug())
                    << "RNG: rejecting acquired entry"
                    << " reason=out-of-round"
                    << " kind=" << (isCommitSet ? "commit" : "reveal")
                    << " source=" << sourceTag << " node=" << nodeId
                    << " hash=" << hash << " seq=" << seq
                    << " expected=" << *expectedSeq
                    << (rngRoundSeq_ ? " (active-round)" : " (closed+1)");
                return;
            }

            if (isCommitSet)
            {
                auto const existingCommit = pendingCommits_.find(nodeId);
                if (existingCommit != pendingCommits_.end() &&
                    existingCommit->second != digest)
                {
                    // A changed commitment invalidates any previously accepted
                    // reveal for this node in the same round.
                    pendingReveals_.erase(nodeId);
                    proposalProofs_.erase(nodeId);
                }
            }
            else
            {
                auto const commitIt = pendingCommits_.find(nodeId);
                if (commitIt == pendingCommits_.end())
                {
                    JLOG(j_.debug())
                        << "RNG: rejecting acquired entry"
                        << " reason=reveal-without-commitment"
                        << " kind=reveal"
                        << " source=" << sourceTag << " node=" << nodeId
                        << " hash=" << hash << " seq=" << seq;
                    return;
                }
                auto const expectedCommit = sha512Half(digest, pubKey, seq);
                if (expectedCommit != commitIt->second)
                {
                    JLOG(j_.warn())
                        << "RNG: rejecting acquired entry"
                        << " reason=reveal-commitment-mismatch"
                        << " kind=reveal"
                        << " source=" << sourceTag << " node=" << nodeId
                        << " hash=" << hash << " seq=" << seq;
                    return;
                }
            }

            pendingData[nodeId] = digest;
            nodeIdToKey_.insert_or_assign(nodeId, pubKey);
            // Preserve fetched proofs so any subsequent local rebuild emits
            // byte-identical SHAMap leaves for these entries.
            if (isCommitSet)
            {
                if (parsedProof && parsedProof->proposeSeq == 0)
                {
                    commitProofs_.insert_or_assign(nodeId, *parsedProof);
                }
                else if (parsedProof)
                {
                    JLOG(j_.debug())
                        << "RNG: commit proof not cached"
                        << " reason=nonzero-propose-seq"
                        << " source=" << sourceTag << " node=" << nodeId
                        << " hash=" << hash
                        << " proposeSeq=" << parsedProof->proposeSeq
                        << " seq=" << seq;
                }
            }
            else if (parsedProof)
            {
                proposalProofs_.insert_or_assign(nodeId, *parsedProof);
            }
            ++merged;

            JLOG(j_.trace()) << "RNG: merged acquired entry"
                             << " kind=" << (isCommitSet ? "commit" : "reveal")
                             << " source=" << sourceTag << " node=" << nodeId
                             << " hash=" << hash << " seq=" << seq;
        }
        catch (std::exception const& ex)
        {
            JLOG(j_.warn()) << "RNG: acquired entry parse failed"
                            << " kind=" << (isCommitSet ? "commit" : "reveal")
                            << " source=" << sourceTag << " hash=" << hash
                            << " error=" << ex.what();
        }
    };

    if (localMap)
    {
        SHAMap::Delta delta;
        localMap->compare(*map, delta, 65536);

        for (auto const& [key, pair] : delta)
        {
            // pair.first = our entry, pair.second = their entry.
            // If we don't have it (pair.first is null), merge it.
            if (!pair.first && pair.second)
                mergeEntry(pair.second->slice(), "diff");
        }
    }
    else
    {
        // We don't have a local set yet — extract all entries.
        map->visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                mergeEntry(item->slice(), "visit");
            });
    }

    JLOG(j_.info()) << "RNGFETCH: merged acquired set"
                    << " hash=" << hash
                    << " kind=" << (isCommitSet ? "commit" : "reveal")
                    << " setKind=" << (isCommitSet ? "commitSet" : "entropySet")
                    << " entriesMerged=" << merged;
}
//@@end handle-acquired-sidecar

void
ConsensusExtensions::fetchRngSetIfNeeded(
    std::optional<uint256> const& hash,
    SidecarKind kind)
{
    if (!hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip"
                         << " kind=" << sidecarKindName(kind)
                         << " reason=no-hash";
        return;
    }
    if (*hash == uint256{})
    {
        JLOG(j_.trace()) << "RNGFETCH: skip"
                         << " kind=" << sidecarKindName(kind)
                         << " hash=" << *hash << " reason=zero-hash";
        return;
    }

    // Check if we already have this set
    if (commitSetMap_ && commitSetMap_->getHash().as_uint256() == *hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip"
                         << " kind=" << sidecarKindName(kind)
                         << " hash=" << *hash << " reason=already-local-commit";
        return;
    }
    if (entropySetMap_ && entropySetMap_->getHash().as_uint256() == *hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip"
                         << " kind=" << sidecarKindName(kind)
                         << " hash=" << *hash
                         << " reason=already-local-entropy";
        return;
    }
    if (exportSigSetMap_ && exportSigSetMap_->getHash().as_uint256() == *hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip"
                         << " kind=" << sidecarKindName(kind)
                         << " hash=" << *hash
                         << " reason=already-local-exportSig";
        return;
    }

    // Check if already fetching
    if (pendingRngFetches_.count(*hash))
    {
        // Keep polling InboundTransactions while pending, so we can merge as
        // soon as the asynchronous fetch completes.
        if (auto existing = app_.getInboundTransactions().getSet(*hash, false))
        {
            JLOG(j_.debug())
                << "RNGFETCH: pending fetch completed"
                << " kind=" << sidecarKindName(kind) << " hash=" << *hash;
            onAcquiredSidecarSet(existing);
        }
        else
        {
            JLOG(j_.debug())
                << "RNGFETCH: still pending"
                << " kind=" << sidecarKindName(kind) << " hash=" << *hash;
        }
        return;
    }

    // Check if InboundTransactions already has it
    if (auto existing = app_.getInboundTransactions().getSet(*hash, false))
    {
        JLOG(j_.debug()) << "RNGFETCH: local cache hit"
                         << " kind=" << sidecarKindName(kind)
                         << " hash=" << *hash;
        // Record the kind so onAcquiredSidecarSet can look it up.
        pendingRngFetches_.emplace(*hash, kind);
        onAcquiredSidecarSet(existing);
        return;
    }

    // Trusted proposals advertise the sidecar root; acquisition is
    // content-addressed, so peers can only supply nodes matching that root.
    // Per-leaf trust/schema checks happen when the completed map is merged.
    JLOG(j_.debug()) << "RNGFETCH: triggering network fetch"
                     << " kind=" << sidecarKindName(kind) << " hash=" << *hash;
    pendingRngFetches_.emplace(*hash, kind);
    if (auto immediate = app_.getInboundTransactions().getSet(
            *hash, true, InboundSetKind::sidecar))
    {
        JLOG(j_.debug()) << "RNGFETCH: immediate fetch hit"
                         << " kind=" << sidecarKindName(kind)
                         << " hash=" << *hash;
        onAcquiredSidecarSet(immediate);
    }
}

void
ConsensusExtensions::fetchSidecarsIfNeeded(ExtendedPosition const& peerPos)
{
    fetchRngSetIfNeeded(peerPos.commitSetHash, SidecarKind::commit);
    fetchRngSetIfNeeded(peerPos.entropySetHash, SidecarKind::reveal);
    fetchRngSetIfNeeded(peerPos.exportSigSetHash, SidecarKind::exportSig);
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

    auto const hash = s.getSHA512Half();
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
    if (!rngEnabledThisRound_ && !exportEnabledThisRound_)
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
    std::size_t upgraded = 0;
    for (auto const& [txHash, stx] : consensusExportTxns_)
    {
        auto const unverified =
            exportSigCollector_.unverifiedSignatures(txHash);
        for (auto const& [valPK, sigBuf] : unverified)
        {
            if (!isActiveValidator(valPK, *validatorView))
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
    JLOG(j_.info()) << "RNG: injectEntropy"
                    << " seq=" << seq << " commits=" << pendingCommits_.size()
                    << " reveals=" << pendingReveals_.size()
                    << " entropyFailed=" << (entropyFailed_ ? "yes" : "no")
                    << " quorum=" << quorumThreshold() << " entropySetHash="
                    << (entropySetMap_
                            ? to_string(entropySetMap_->getHash().as_uint256())
                            : std::string{"none"});

    uint256 finalEntropy;
    bool hasEntropy = false;
    std::uint8_t entropyTier = entropyTierNone;
    std::uint16_t entropyCount = 0;

    // Tier 3 fallback: consensus-bound deterministic digest derived from
    // already-agreed round inputs. txSetHash is the BASE (pre-injection)
    // consensus tx set hash — the digest must never depend on a set that
    // could contain the pseudo-tx carrying it (circular).
    auto const fallbackEntropy = [&] {
        return sha512Half(
            HashPrefix::entropyFallback, roundPrevLedgerHash_, txSetHash, seq);
    };

    //@@start rng-inject-entropy-selection
    // Calculate entropy from collected reveals
    if (app_.config().standalone())
    {
        // Standalone mode: generate synthetic deterministic entropy
        // so that Hook APIs (dice/random) work for testing.
        finalEntropy = sha512Half(std::string("standalone-entropy"), seq);
        hasEntropy = true;
        entropyTier = entropyTierValidatorQuorum;
        entropyCount = 20;  // synthetic: high enough for any hook minimum
        JLOG(j_.info()) << "RNG: standalone synthetic entropy"
                        << " seq=" << seq << " entropy=" << finalEntropy;
    }
    else if (shouldZeroEntropy())
    {
        // Liveness fallback (Tier 3): consensus-bound deterministic entropy
        // instead of zero. EntropyTier/EntropyCount mark it fallback-grade —
        // user-influenceable via tx submission, never for value-bearing use.
        // shouldZeroEntropy() covers: pipeline failure, no reveals,
        // or sub-quorum reveals (too easily influenced by a minority).
        finalEntropy = fallbackEntropy();
        hasEntropy = true;
        entropyTier = entropyTierConsensusFallback;
        entropyCount = 0;
        JLOG(j_.warn()) << "RNG: injecting FALLBACK entropy"
                        << " seq=" << seq << " reason=fallback"
                        << " entropy=" << finalEntropy
                        << " reveals=" << pendingReveals_.size()
                        << " threshold=" << quorumThreshold()
                        << " entropyFailed=" << (entropyFailed_ ? "yes" : "no")
                        << " hasEntropySet=" << (entropySetMap_ ? "yes" : "no");
    }
    else if (entropySetMap_)
    {
        // Compute entropy from the agreed-upon entropySet SHAMap
        // rather than from local pendingReveals_.  The map's hash
        // was published in proposals and converged via fetch/merge,
        // so all nodes with the same entropySetHash produce the
        // same entropy — preventing reveal-subset divergence at
        // timeout boundaries.
        //
        // Each leaf is an STObject(sfGeneric) sidecar with sfSigningPubKey
        // (validator key) and sfDigest (the reveal).
        std::vector<std::pair<PublicKey, uint256>> sorted;
        entropySetMap_->visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                try
                {
                    SerialIter sit(item->slice());
                    STObject obj(sit, sfGeneric);
                    auto const pk = obj.getFieldVL(sfSigningPubKey);
                    if (!publicKeyType(makeSlice(pk)))
                        return;
                    sorted.emplace_back(
                        PublicKey(makeSlice(pk)), obj.getFieldH256(sfDigest));
                }
                catch (...)
                {
                }
            });

        if (!sorted.empty())
        {
            std::sort(
                sorted.begin(), sorted.end(), [](auto const& a, auto const& b) {
                    return a.first.slice() < b.first.slice();
                });

            // Mix all reveals into final entropy
            Serializer s;
            for (auto const& [key, reveal] : sorted)
            {
                s.addVL(key.slice());
                s.addBitString(reveal);
            }
            finalEntropy = sha512Half(s.slice());
            hasEntropy = true;
            entropyTier = entropyTierValidatorQuorum;
            entropyCount = static_cast<std::uint16_t>(sorted.size());

            JLOG(j_.info())
                << "RNG: injecting entropy"
                << " seq=" << seq << " entropy=" << finalEntropy
                << " entropyCount=" << sorted.size() << " source=entropySetMap"
                << " entropySetHash=" << entropySetMap_->getHash().as_uint256();
        }
    }

    if (!hasEntropy)
    {
        // Residual: an entropy set passed the gate but yielded no parseable
        // leaves. Fall back rather than skipping injection so a fresh
        // ConsensusEntropy entry exists on every RNG-enabled ledger.
        finalEntropy = fallbackEntropy();
        hasEntropy = true;
        entropyTier = entropyTierConsensusFallback;
        entropyCount = 0;
        JLOG(j_.warn()) << "RNG: injecting FALLBACK entropy"
                        << " seq=" << seq << " reason=unparseable-entropy-set"
                        << " entropy=" << finalEntropy;
    }
    //@@end rng-inject-entropy-selection

    //@@start rng-inject-pseudotx
    // Synthesize and inject the pseudo-transaction
    if (hasEntropy)
    {
        // Design note: this is the canonical/implicit path that materializes
        // the synthetic entropy-bearing tx-set in production.
        //
        // Why here (onAccept/buildLCL) instead of mutating proposals earlier?
        // - Consensus agreement is keyed by proposal txSetHash during
        //   establish. Late mutation of txSetHash in establish can fragment
        //   votes under loss/reordering (base hash vs synthetic hash).
        // - Injecting at accept preserves robust convergence semantics: peers
        //   agree on the base transaction set first, then deterministically
        //   derive/apply the entropy pseudo-tx for ledger construction.
        //
        // Explicit-final (seq=4 synthetic proposal) remains an optional
        // experiment for observability/perf testing and is default-off.
        // TBD (2026-03-03): revisit only with stronger evidence that explicit
        // publication can be made stable under tx-bearing, lossy networks.

        //@@start rng-inject-pseudotx-core
        // Account Zero convention for pseudo-transactions (same as ttFEE, etc)
        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, finalEntropy);
            obj.setFieldU16(sfEntropyCount, entropyCount);
            obj.setFieldU8(sfEntropyTier, entropyTier);
        });

        auto const txID = tx.getTransactionID();
        // Value-based dedup. There must never be two entropy pseudo-txs, but
        // when one is already present (explicit-final, or a peer's agreed
        // set) it must be VALIDATED as the exact pseudo-tx we would have
        // produced — not merely "same type". Injection is deterministic, so
        // every honest node derives the identical pseudo-tx (identical txID)
        // for the same agreed inputs. A present-but-different entropy pseudo-tx
        // is therefore a determinism violation (version skew or a divergent/
        // malicious peer) and must be surfaced, not silently trusted.
        auto const existing = std::find_if(
            retriableTxs.begin(), retriableTxs.end(), [](auto const& entry) {
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
                // pseudo-tx, so we cannot replace it without forking off the
                // agreed ledger; keep it, but loudly flag the mismatch.
                JLOG(j_.error())
                    << "RNG: entropy pseudo-tx MISMATCH"
                    << " seq=" << seq << " reason=determinism-violation"
                    << " ourTxHash=" << txID << " ourDigest=" << finalEntropy
                    << " ourTier=" << static_cast<int>(entropyTier)
                    << " ourCount=" << entropyCount
                    << " presentTxHash=" << existingID << " presentDigest="
                    << existing->second->getFieldH256(sfDigest)
                    << " presentTier="
                    << static_cast<int>(
                           existing->second->getFieldU8(sfEntropyTier))
                    << " presentCount="
                    << existing->second->getFieldU16(sfEntropyCount);
            }
        }
        else
        {
            retriableTxs.insert(std::make_shared<STTx>(std::move(tx)));
        }
        //@@end rng-inject-pseudotx-core
    }
    //@@end rng-inject-pseudotx

    //@@start accept-time-cleanup-success
    // Reset RNG state for next round
    clearRngState();
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

    //@@start rng-harvest-trust-and-reveal-verification
    // Reject data from validators not in the active UNL
    if (!isUNLReportMember(nodeId))
    {
        JLOG(j_.trace()) << "RNG: rejecting proposal data"
                         << " reason=non-active-validator"
                         << " node=" << nodeId << " proposeSeq=" << proposeSeq
                         << " prevLedger=" << prevLedger;
        return;
    }

    // RuntimeConfig: randomly drop RNG claims for testing
    auto& rc = app_.getRuntimeConfig();
    if (rc.active())
    {
        if (auto cfg = rc.getConfig("*"))
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

    // Store nodeId -> publicKey mapping for deterministic ordering
    nodeIdToKey_.insert_or_assign(nodeId, publicKey);

    //@@start rng-harvest-commit
    // Harvest commitment if present
    if (position.myCommitment)
    {
        auto [it, inserted] =
            pendingCommits_.emplace(nodeId, *position.myCommitment);
        if (!inserted && it->second != *position.myCommitment)
        {
            JLOG(j_.warn())
                << "RNG: validator changed commitment"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " old=" << it->second << " new=" << *position.myCommitment;
            it->second = *position.myCommitment;

            // commitProofs_ stores seq=0 proofs. If a validator changes its
            // commitment later in the round, that old proof no longer matches
            // the new digest and must not be embedded into a fetched commitSet.
            commitProofs_.erase(nodeId);

            // Any reveal accepted against the prior commitment is now stale.
            // Drop it so reveal quorum cannot be satisfied by mismatched data.
            if (pendingReveals_.erase(nodeId) > 0)
                proposalProofs_.erase(nodeId);
        }
        else if (inserted)
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
        auto commitIt = pendingCommits_.find(nodeId);
        if (commitIt == pendingCommits_.end())
        {
            // No commitment on record — cannot verify. Ignore to prevent
            // grinding attacks where a validator skips the commit phase.
            JLOG(j_.warn())
                << "RNG: rejecting reveal"
                << " reason=no-commitment"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " prevLedger=" << prevLedger;
            return;
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
        auto const calculated = sha512Half(*position.myReveal, publicKey, seq);

        if (calculated != commitIt->second)
        {
            JLOG(j_.warn())
                << "RNG: rejecting reveal"
                << " reason=commitment-mismatch"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " seq=" << seq << " expected=" << commitIt->second
                << " calculated=" << calculated;
            return;
        }

        auto [it, inserted] =
            pendingReveals_.emplace(nodeId, *position.myReveal);
        if (!inserted && it->second != *position.myReveal)
        {
            JLOG(j_.warn())
                << "RNG: validator changed reveal"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " old=" << it->second << " new=" << *position.myReveal;
            it->second = *position.myReveal;
        }
        else if (inserted)
        {
            JLOG(j_.trace())
                << "RNG: harvested reveal"
                << " node=" << nodeId << " proposeSeq=" << proposeSeq
                << " seq=" << seq << " reveal=" << *position.myReveal;
        }
    }
    //@@end rng-harvest-reveal-verification
    //@@end rng-harvest-trust-and-reveal-verification

    // Store proposal proofs for embedding in SHAMap entries.
    // commitProofs_: only seq=0 (commitments always ride on seq=0,
    //   so all nodes store the same proof → deterministic commitSet).
    // proposalProofs_: latest proof carrying a reveal (for entropySet).
    if (position.myCommitment || position.myReveal)
    {
        auto makeProof = [&]() {
            ProposalProof proof;
            proof.proposeSeq = proposeSeq;
            proof.closeTime = static_cast<std::uint32_t>(
                closeTime.time_since_epoch().count());
            proof.prevLedger = prevLedger;
            Serializer s;
            position.add(s);
            proof.positionData = std::move(s);
            proof.signature = Buffer(signature.data(), signature.size());
            return proof;
        };

        if (position.myCommitment && proposeSeq == 0)
            commitProofs_.emplace(nodeId, makeProof());

        if (position.myReveal)
            proposalProofs_[nodeId] = makeProof();
    }
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
        SerialIter sit(makeSlice(proofBlob));

        auto proposeSeq = sit.get32();
        auto closeTime = sit.get32();
        auto prevLedger = sit.get256();
        auto positionData = sit.getVL();
        auto signature = sit.getVL();

        // Deserialize ExtendedPosition from the proof
        SerialIter posIter(makeSlice(positionData));
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
            HashPrefix::proposal, proposeSeq, closeTime, prevLedger, position);

        // Verify the proposal signature
        return verifyDigest(publicKey, signingHash, makeSlice(signature));
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
    roundPrevLedgerHash_ = prevLedger.ledger_->info().hash;
    cacheUNLReport(prevLedger.ledger_);
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
ConsensusExtensions::onAcceptComplete()
{
    // Cleanup deferred to onRoundStart. This hook exists so extensions
    // can optionally do eager cleanup or emit metrics at accept time.
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
            [this, validatorView](PublicKey const& pk) {
                return isActiveValidator(pk, *validatorView);
            },
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

    uint256 proposalPrevLedger;
    std::memcpy(
        proposalPrevLedger.data(),
        wireMsg.previousledger().data(),
        uint256::size());

    std::vector<std::string> exportSignatures;
    exportSignatures.reserve(wireMsg.exportsignatures_size());
    for (int i = 0; i < wireMsg.exportsignatures_size(); ++i)
        exportSignatures.push_back(wireMsg.exportsignatures(i));

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

    setMode(ConsensusMode::proposing);
    cacheUNLReport(prevLedger);
    generateEntropySecret();

    auto const& valKeys = app_.getValidatorKeys();
    pos.myCommitment = sha512Half(
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
    // RuntimeConfig no_export_sig disables sig attachment (testing sub-quorum).
    {
        auto& rc = app_.getRuntimeConfig();
        if (rc.active())
        {
            if (auto cfg = rc.getConfig("*"))
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

    auto const openLedger = app_.openLedger().current();
    if (!openLedger || !openLedger->rules().enabled(featureExport))
        return;

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
        if (!stx || stx->getTxnType() != ttEXPORT)
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

        // Only attach our sig on the first proposal this round.
        if (!exportSigCollector_.markSent(txHash))
            continue;

        //@@start export-compute-proposal-sig
        Buffer sigBuf;
        if (stx->isFieldPresent(sfExportedTxn))
        {
            auto const& exportedObj = const_cast<STTx&>(*stx)
                                          .peekAtField(sfExportedTxn)
                                          .downcast<STObject>();

            Serializer innerSer;
            exportedObj.add(innerSer);
            SerialIter sit(innerSer.slice());

            try
            {
                STTx innerTx(std::ref(sit));
                auto sigData = buildMultiSigningData(innerTx, signerAcctID);
                sigBuf = sign(valPK, valSK, sigData.slice());
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

    // Store our own proposal proof for embedding in SHAMap entries.
    // commitProofs_ gets seq=0 only (deterministic commitSet).
    // proposalProofs_ gets the latest with a reveal (for entropySet).
    if (signedPosition.myCommitment || signedPosition.myReveal)
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

        if (signedPosition.myReveal)
            proposalProofs_[valKeys.nodeID] = makeProof();
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
