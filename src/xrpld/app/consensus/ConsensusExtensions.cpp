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
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>
#include <algorithm>
#include <cstring>
#include <random>

namespace ripple {

ConsensusExtensions::ConsensusExtensions(Application& app, beast::Journal j)
    : app_(app), j_(j)
{
}

//------------------------------------------------------------------------------
// RNG Helper Methods

std::size_t
ConsensusExtensions::quorumThreshold() const
{
    // Non-zero entropy is only allowed once a fixed 80% quorum of the active
    // UNL snapshot has committed. Recent proposers are useful for liveness
    // heuristics, but they do not lower this floor.
    auto const base = unlReportNodeIds_.size();
    if (base == 0)
        return 1;  // safety: need at least one commit
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
        hash_set<NodeID> filtered;
        for (auto const& id : proposers)
        {
            if (!includeSelf && id == app_.getValidatorKeys().nodeID)
                continue;
            if (unlReportNodeIds_.count(id))
                filtered.insert(id);
        }
        if (includeSelf)
            filtered.insert(app_.getValidatorKeys().nodeID);
        likelyParticipants_ = std::move(filtered);
        JLOG(j_.trace()) << "RNG: likelyParticipants from recent proposers: "
                         << likelyParticipants_.size() << " (filtered from "
                         << proposers.size() << ", includeSelf=" << includeSelf
                         << ")";
        return;
    }

    // First round (or no recent data): fall back to the active UNL snapshot as
    // our best guess for who may still contribute before timeout.
    if (!unlReportNodeIds_.empty())
    {
        likelyParticipants_ = unlReportNodeIds_;
        JLOG(j_.trace()) << "RNG: likelyParticipants from active UNL: "
                         << likelyParticipants_.size();
        return;
    }

    // No data at all (shouldn't happen — cacheUNLReport falls back to
    // trusted keys). Leave empty; diagnostics will show no liveness hint.
    JLOG(j_.warn()) << "RNG: no likelyParticipants available";
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
    auto threshold = quorumThreshold();
    bool result = pendingCommits_.size() >= threshold;
    JLOG(j_.trace()) << "RNG: hasQuorumOfCommits? " << pendingCommits_.size()
                     << "/" << threshold << " -> " << (result ? "YES" : "no")
                     << " (activeUNL=" << unlReportNodeIds_.size()
                     << ", likelyParticipants=" << likelyParticipants_.size()
                     << ")";
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
    auto const expected = pendingCommits_.size();
    bool result = pendingReveals_.size() >= expected;
    JLOG(j_.trace()) << "RNG: hasMinimumReveals? " << pendingReveals_.size()
                     << "/" << expected << " -> " << (result ? "YES" : "no");
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
    return entropyFailed_ || pendingReveals_.empty() ||
        pendingReveals_.size() < quorumThreshold();
}

bool
ConsensusExtensions::rngEnabled() const
{
    return rngEnabledThisRound_;
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
    JLOG(j_.debug()) << "RNGFINAL: build synthetic txset"
                     << " baseTxSet=" << txns.id() << " seq=" << seq
                     << " commits=" << pendingCommits_.size()
                     << " reveals=" << pendingReveals_.size()
                     << " failed=" << entropyFailed_;

    uint256 finalEntropy;
    bool hasEntropy = false;

    // Keep this entropy-selection logic aligned with onPreBuild().
    // If these paths drift, different nodes can derive different synthetic
    // hashes for the same round, which is especially harmful because this
    // path mutates proposal tx-set identity late in establish.
    if (app_.config().standalone())
    {
        finalEntropy = sha512Half(std::string("standalone-entropy"), seq);
        hasEntropy = true;
    }
    else if (shouldZeroEntropy())
    {
        finalEntropy.zero();
        hasEntropy = true;
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
        }
    }

    if (!hasEntropy)
    {
        JLOG(j_.debug()) << "RNGFINAL: no entropy available for synthetic txset"
                         << " baseTxSet=" << txns.id() << " seq=" << seq;
        return std::nullopt;
    }

    auto const entropyCount = static_cast<std::uint16_t>(
        app_.config().standalone()
            ? 20
            : (shouldZeroEntropy() ? 0 : pendingReveals_.size()));

    STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
        obj.setFieldU32(sfLedgerSequence, seq);
        obj.setAccountID(sfAccount, AccountID{});
        obj.setFieldU32(sfSequence, 0);
        obj.setFieldAmount(sfFee, STAmount{});
        obj.setFieldH256(sfDigest, finalEntropy);
        obj.setFieldU16(sfEntropyCount, entropyCount);
    });

    auto const txID = tx.getTransactionID();
    if (txns.exists(txID))
    {
        JLOG(j_.debug()) << "RNGFINAL: pseudo-tx already in base set"
                         << " txid=" << txID << " txSet=" << txns.id();
        return txns;
    }

    RCLTxSet::MutableTxSet mutableTxSet{txns};
    Serializer ser(512);
    tx.add(ser);
    mutableTxSet.insert(RCLCxTx{make_shamapitem(txID, ser.slice())});
    auto syntheticSet = RCLTxSet{mutableTxSet};
    auto const hash = syntheticSet.id();
    app_.getInboundTransactions().giveSet(hash, syntheticSet.map_, false);

    JLOG(j_.debug()) << "RNGFINAL: built synthetic txset"
                     << " hash=" << hash << " baseTxSet=" << txns.id()
                     << " txid=" << txID << " entropyCount=" << entropyCount;

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
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    map->setUnbacked();

    // NOTE: avoid structured bindings in for-loops containing lambdas —
    // clang-14 (CI) rejects capturing them (P2036R3 not implemented).
    for (auto const& entry : pendingCommits_)
    {
        auto const& nid = entry.first;
        auto const& commit = entry.second;

        if (!isUNLReportMember(nid))
            continue;

        auto kit = nodeIdToKey_.find(nid);
        if (kit == nodeIdToKey_.end())
            continue;

        // Encode the NodeID into sfAccount so onAcquiredSidecarSet can
        // recover it without recomputing (master vs signing key issue).
        AccountID acctId;
        std::memcpy(acctId.data(), nid.data(), acctId.size());

        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfFlags, tfEntropyCommit);
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, acctId);
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, commit);
            obj.setFieldVL(sfSigningPubKey, kit->second.slice());
            auto proofIt = commitProofs_.find(nid);
            if (proofIt != commitProofs_.end())
                obj.setFieldVL(sfBlob, serializeProof(proofIt->second));
        });

        Serializer s(2048);
        tx.add(s);
        map->addItem(
            SHAMapNodeType::tnTRANSACTION_NM,
            make_shamapitem(tx.getTransactionID(), s.slice()));
    }

    map = map->snapShot(false);
    commitSetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built commitSet SHAMap hash=" << hash
                     << " entries=" << pendingCommits_.size();
    return hash;
    //@@end rng-build-commit-set
}

uint256
ConsensusExtensions::buildEntropySet(LedgerIndex seq)
{
    //@@start rng-build-entropy-set
    rngRoundSeq_ = seq;

    auto map =
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    map->setUnbacked();

    // NOTE: avoid structured bindings — clang-14 can't capture them (P2036R3).
    for (auto const& entry : pendingReveals_)
    {
        auto const& nid = entry.first;
        auto const& reveal = entry.second;

        if (!isUNLReportMember(nid))
            continue;

        auto kit = nodeIdToKey_.find(nid);
        if (kit == nodeIdToKey_.end())
            continue;

        AccountID acctId;
        std::memcpy(acctId.data(), nid.data(), acctId.size());

        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfFlags, tfEntropyReveal);
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, acctId);
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, reveal);
            obj.setFieldVL(sfSigningPubKey, kit->second.slice());
            // Intentionally omit sfBlob for reveal-set entries.
            //
            // Reveal proofs are timing-dependent (seq/closeTime/signature can
            // differ while the reveal digest is identical), which makes the
            // entropy-set hash non-deterministic across nodes under packet
            // loss/reordering.  We only need deterministic reveal material
            // (validator identity + digest) for fetch/merge and entropy
            // calculation.
        });

        Serializer s(2048);
        tx.add(s);
        map->addItem(
            SHAMapNodeType::tnTRANSACTION_NM,
            make_shamapitem(tx.getTransactionID(), s.slice()));
    }

    map = map->snapShot(false);
    entropySetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built entropySet SHAMap hash=" << hash
                     << " entries=" << pendingReveals_.size();
    return hash;
    //@@end rng-build-entropy-set
}

uint256
ConsensusExtensions::buildExportSigSet(LedgerIndex seq)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    map->setUnbacked();

    auto const allSigs = exportSigCollector_.snapshotWithSigs();
    std::size_t entryCount = 0;

    for (auto const& [txHash, valSigs] : allSigs)
    {
        for (auto const& [valPK, sigBuf] : valSigs)
        {
            // Each entry: txHash + validatorPK + signature (if available).
            Serializer s;
            s.addBitString(txHash);
            s.addRaw(valPK.slice());
            if (sigBuf.size() > 0)
                s.addRaw(Slice(sigBuf.data(), sigBuf.size()));

            auto const itemHash = sha512Half(txHash, valPK);
            map->addItem(
                SHAMapNodeType::tnTRANSACTION_NM,
                make_shamapitem(itemHash, s.slice()));
            ++entryCount;
        }
    }

    map = map->snapShot(false);
    exportSigSetMap_ = map;

    auto const hash = map->getHash().as_uint256();
    app_.getInboundTransactions().giveSet(hash, map, false);

    JLOG(j_.debug()) << "Export: built exportSigSet SHAMap hash=" << hash
                     << " entries=" << entryCount;
    return hash;
}

bool
ConsensusExtensions::hasPendingExportSigs() const
{
    auto const allSigs = exportSigCollector_.snapshot();
    return !allSigs.empty();
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

//@@start clear-rng-state
void
ConsensusExtensions::clearRngState()
{
    //@@start round-stop-export-reset
    exportSigCollector_.clearRound();
    if (auto const closed = app_.getLedgerMaster().getClosedLedger())
        exportSigCollector_.cleanupStale(closed->info().seq);
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
    pendingRngFetches_.clear();
    unlReportNodeIds_.clear();
    likelyParticipants_.clear();
    commitProofs_.clear();
    proposalProofs_.clear();
    //@@end round-stop-rng-reset
    // Keep the round-level enable latch intact here. Consensus::startRound()
    // calls preStartRound() first to snapshot whether RNG is enabled for the
    // upcoming round, then immediately clears per-round working state.
    // Resetting rngEnabledThisRound_ here would wipe that snapshot before
    // phaseEstablish() can consult it.
}
//@@end clear-rng-state

void
ConsensusExtensions::cacheUNLReport()
{
    unlReportNodeIds_.clear();
    bool const includeSelf = mode_ == ConsensusMode::proposing &&
        app_.getValidatorKeys().keys &&
        app_.getValidatorKeys().nodeID != beast::zero;

    // Try UNL Report from the validated ledger
    if (auto const prevLedger = app_.getLedgerMaster().getValidatedLedger())
    {
        if (auto const sle = prevLedger->read(keylet::UNLReport()))
        {
            if (sle->isFieldPresent(sfActiveValidators))
            {
                for (auto const& obj : sle->getFieldArray(sfActiveValidators))
                {
                    auto const pk = obj.getFieldVL(sfPublicKey);
                    if (publicKeyType(makeSlice(pk)))
                    {
                        unlReportNodeIds_.insert(
                            calcNodeID(PublicKey(makeSlice(pk))));
                    }
                }
            }
        }
    }

    // Fallback to normal UNL if no report or empty
    if (unlReportNodeIds_.empty())
    {
        for (auto const& masterKey : app_.validators().getTrustedMasterKeys())
        {
            unlReportNodeIds_.insert(calcNodeID(masterKey));
        }
    }

    // Only include ourselves when actively proposing. Observers/non-validators
    // do not emit commitments and must not be expected in commit quorum.
    if (includeSelf)
        unlReportNodeIds_.insert(app_.getValidatorKeys().nodeID);
    else
        unlReportNodeIds_.erase(app_.getValidatorKeys().nodeID);

    JLOG(j_.trace()) << "RNG: cacheUNLReport size=" << unlReportNodeIds_.size();
}

bool
ConsensusExtensions::isUNLReportMember(NodeID const& nodeId) const
{
    return unlReportNodeIds_.count(nodeId) > 0;
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
    return pendingRngFetches_.count(hash) > 0;
}
//@@end is-sidecar-set

//@@start handle-acquired-sidecar
//@@start handle-acquired-sidecar-entry
void
ConsensusExtensions::onAcquiredSidecarSet(std::shared_ptr<SHAMap> const& map)
{
    auto const hash = map->getHash().as_uint256();
    pendingRngFetches_.erase(hash);
    //@@end handle-acquired-sidecar-entry

    JLOG(j_.debug()) << "RNGFETCH: handle acquired hash=" << hash
                     << " pending-after-erase=" << pendingRngFetches_.size();

    // Check if this is an export sig set (not an RNG set).
    // Export sig entries are raw blobs (65 bytes: txHash + pubkey),
    // not STTx objects. Detect by inspecting the first leaf.
    {
        // If we already have this exact export sig set, skip.
        if (exportSigSetMap_ &&
            exportSigSetMap_->getHash().as_uint256() == hash)
            return;

        bool isExportSet = false;
        map->visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                // Export sig entries are >= 65 bytes (32 hash + 33 pubkey
                // + optional variable-length signature). RNG entries are
                // serialized STTx objects with different structure.
                // Detect by checking the first 65 bytes contain a valid
                // pubkey at offset 32.
                if (!isExportSet && item->size() >= 65)
                {
                    auto const pkSlice = item->slice().substr(32, 33);
                    if (publicKeyType(pkSlice))
                        isExportSet = true;
                }
            });

        if (isExportSet)
        {
            // Build export tx lookup from open ledger for sig verification.
            auto const openLedger = app_.openLedger().current();
            std::unordered_map<uint256, std::shared_ptr<STTx const>> exportTxns;
            if (openLedger)
            {
                for (auto const& [stx, meta] : openLedger->txs)
                {
                    if (stx && stx->getTxnType() == ttEXPORT)
                        exportTxns.emplace(stx->getTransactionID(), stx);
                }
            }

            std::size_t merged = 0;
            map->visitLeaves(
                [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                    if (item->size() < 65)
                        return;
                    auto const data = item->slice();
                    uint256 txHash;
                    std::memcpy(txHash.data(), data.data(), 32);
                    auto const pkSlice = data.substr(32, 33);
                    if (!publicKeyType(pkSlice))
                        return;

                    PublicKey const valPK{pkSlice};
                    if (!app_.validators().trusted(valPK))
                        return;

                    // Require a real signature (not pubkey-only).
                    if (item->size() <= 65)
                        return;

                    auto const sigSlice = data.substr(65);

                    // Verify the multisign signature against the inner tx.
                    auto const txIt = exportTxns.find(txHash);
                    if (txIt == exportTxns.end() ||
                        !txIt->second->isFieldPresent(sfExportedTxn))
                    {
                        JLOG(j_.debug())
                            << "Export: SHAMap merge — cannot verify sig "
                               "for tx "
                            << txHash << " (not in open ledger) — skipped";
                        return;
                    }

                    try
                    {
                        auto const& exportedObj =
                            const_cast<STTx&>(*txIt->second)
                                .peekAtField(sfExportedTxn)
                                .downcast<STObject>();

                        Serializer innerSer;
                        exportedObj.add(innerSer);
                        SerialIter sit(innerSer.slice());
                        STTx innerTx(std::ref(sit));

                        auto const signerAcctID = calcAccountID(valPK);
                        auto const sigData =
                            buildMultiSigningData(innerTx, signerAcctID);
                        if (!verify(valPK, sigData.slice(), sigSlice))
                        {
                            JLOG(j_.warn())
                                << "Export: SHAMap merge — invalid sig "
                                   "for tx "
                                << txHash << " — rejected";
                            return;
                        }
                    }
                    catch (std::exception const& e)
                    {
                        JLOG(j_.warn())
                            << "Export: SHAMap merge — failed to verify "
                               "sig for tx "
                            << txHash << ": " << e.what();
                        return;
                    }

                    Buffer sigBuf(sigSlice.data(), sigSlice.size());
                    exportSigCollector_.addSignature(txHash, valPK, sigBuf);
                    ++merged;
                });
            JLOG(j_.info()) << "Export: merged " << merged
                            << " verified entries from peer exportSigSet "
                               "hash="
                            << hash;
            return;
        }
    }

    enum class RngSetKind { commit, reveal };
    auto const classifyKind =
        [](std::uint32_t flags) -> std::optional<RngSetKind> {
        auto const hasCommit = (flags & tfEntropyCommit) != 0;
        auto const hasReveal = (flags & tfEntropyReveal) != 0;
        if (hasCommit == hasReveal)
            return std::nullopt;
        return hasCommit ? std::optional<RngSetKind>{RngSetKind::commit}
                         : std::optional<RngSetKind>{RngSetKind::reveal};
    };

    // Determine whether this is a pure commitSet or entropySet. Mixed sets are
    // rejected to avoid cross-type contamination of pending state.
    std::optional<RngSetKind> setKind;
    bool mixedKinds = false;
    map->visitLeaves([&](boost::intrusive_ptr<SHAMapItem const> const& item) {
        try
        {
            SerialIter sit(item->slice());
            auto stx = std::make_shared<STTx const>(std::ref(sit));
            if (stx->getTxnType() != ttCONSENSUS_ENTROPY ||
                !stx->isFieldPresent(sfFlags))
                return;

            auto const entryKind = classifyKind(stx->getFieldU32(sfFlags));
            if (!entryKind)
                return;

            if (!setKind)
                setKind = entryKind;
            else if (*setKind != *entryKind)
                mixedKinds = true;
        }
        catch (std::exception const&)
        {
            // Skip malformed entries
        }
    });

    if (!setKind)
    {
        JLOG(j_.warn()) << "RNGFETCH: acquired set " << hash
                        << " has no recognizable RNG entries";
        return;
    }
    if (mixedKinds)
    {
        JLOG(j_.warn()) << "RNGFETCH: acquired set " << hash
                        << " mixes commit/reveal entries; rejecting";
        return;
    }

    bool const isCommitSet = *setKind == RngSetKind::commit;
    JLOG(j_.debug()) << "RNGFETCH: classified hash=" << hash
                     << " kind=" << (isCommitSet ? "commitSet" : "entropySet");

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
            auto stx = std::make_shared<STTx const>(std::ref(sit));
            if (stx->getTxnType() != ttCONSENSUS_ENTROPY ||
                !stx->isFieldPresent(sfFlags))
                return;

            auto const entryKind = classifyKind(stx->getFieldU32(sfFlags));
            if (!entryKind ||
                ((*entryKind == RngSetKind::commit) != isCommitSet))
                return;

            auto const pk = stx->getFieldVL(sfSigningPubKey);
            PublicKey pubKey(makeSlice(pk));
            auto const digest = stx->getFieldH256(sfDigest);

            // Recover NodeID from sfAccount (encoded by
            // buildCommitSet/buildEntropySet) so we can compare against trusted
            // validator identity.
            auto const acctId = stx->getAccountID(sfAccount);
            NodeID nodeId;
            std::memcpy(nodeId.data(), acctId.data(), nodeId.size());

            if (!isUNLReportMember(nodeId))
            {
                JLOG(j_.debug()) << "RNG: rejecting non-UNL entry from "
                                 << nodeId << " in acquired set";
                return;
            }

            // Bind the claimed nodeId to a trusted validator key identity.
            // This prevents a fetched set from impersonating arbitrary UNL
            // members via sfAccount.
            auto const trustedMaster = app_.validators().getTrustedKey(pubKey);
            if (!trustedMaster)
            {
                JLOG(j_.warn())
                    << "RNG: rejecting untrusted signing key for " << nodeId
                    << " in acquired set (" << sourceTag << ")";
                return;
            }
            if (calcNodeID(*trustedMaster) != nodeId)
            {
                JLOG(j_.warn())
                    << "RNG: rejecting node/key identity mismatch for "
                    << nodeId << " in acquired set (" << sourceTag << ")";
                return;
            }

            std::optional<ProposalProof> parsedProof;
            if (stx->isFieldPresent(sfBlob))
            {
                auto const proofBlob = stx->getFieldVL(sfBlob);
                if (!verifyProof(proofBlob, pubKey, digest, isCommitSet))
                {
                    JLOG(j_.warn()) << "RNG: invalid proof from " << nodeId
                                    << " in acquired set (" << sourceTag << ")";
                    return;
                }
                parsedProof = deserializeProof(proofBlob);
                if (!parsedProof)
                {
                    JLOG(j_.warn())
                        << "RNG: rejecting malformed proof from " << nodeId
                        << " in acquired set (" << sourceTag << ")";
                    return;
                }
            }
            else if (isCommitSet)
            {
                // Commit entries must carry a verifiable proposal proof.
                // Without this, an attacker could inject arbitrary digests
                // for trusted node IDs via fetched sets.
                JLOG(j_.warn())
                    << "RNG: rejecting proofless commit entry from " << nodeId
                    << " in acquired set (" << sourceTag << ")";
                return;
            }

            auto const seq = stx->getFieldU32(sfLedgerSequence);
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
                    << "RNG: rejecting out-of-round entry from " << nodeId
                    << " in acquired set (" << sourceTag << "), seq=" << seq
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
                    JLOG(j_.debug()) << "RNG: rejecting reveal from " << nodeId
                                     << " in acquired set (" << sourceTag
                                     << ") without commitment";
                    return;
                }
                auto const expectedCommit = sha512Half(digest, pubKey, seq);
                if (expectedCommit != commitIt->second)
                {
                    JLOG(j_.warn()) << "RNG: rejecting reveal from " << nodeId
                                    << " in acquired set (" << sourceTag
                                    << ") that does not match commitment";
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
                    commitProofs_.insert_or_assign(nodeId, *parsedProof);
                else if (parsedProof)
                    JLOG(j_.debug()) << "RNG: commit proof from " << nodeId
                                     << " has non-zero proposeSeq="
                                     << parsedProof->proposeSeq
                                     << "; not caching for commitSet rebuild";
            }
            else if (parsedProof)
            {
                proposalProofs_.insert_or_assign(nodeId, *parsedProof);
            }
            ++merged;

            JLOG(j_.trace())
                << "RNG: merged " << (isCommitSet ? "commit" : "reveal")
                << " from " << nodeId;
        }
        catch (std::exception const& ex)
        {
            JLOG(j_.warn()) << "RNG: failed to parse entry from acquired set ("
                            << sourceTag << "): " << ex.what();
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

    JLOG(j_.info()) << "RNGFETCH: merged " << merged << " entries from "
                    << (isCommitSet ? "commitSet" : "entropySet")
                    << " hash=" << hash;
}
//@@end handle-acquired-sidecar

void
ConsensusExtensions::fetchRngSetIfNeeded(std::optional<uint256> const& hash)
{
    if (!hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip reason=no-hash";
        return;
    }
    if (*hash == uint256{})
    {
        JLOG(j_.trace()) << "RNGFETCH: skip reason=zero-hash";
        return;
    }

    // Check if we already have this set
    if (commitSetMap_ && commitSetMap_->getHash().as_uint256() == *hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip reason=already-local-commit hash="
                         << *hash;
        return;
    }
    if (entropySetMap_ && entropySetMap_->getHash().as_uint256() == *hash)
    {
        JLOG(j_.trace()) << "RNGFETCH: skip reason=already-local-entropy hash="
                         << *hash;
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
                << "RNGFETCH: pending fetch completed, merging hash=" << *hash;
            onAcquiredSidecarSet(existing);
        }
        else
        {
            JLOG(j_.debug()) << "RNGFETCH: still pending hash=" << *hash;
        }
        return;
    }

    // Check if InboundTransactions already has it
    if (auto existing = app_.getInboundTransactions().getSet(*hash, false))
    {
        JLOG(j_.debug()) << "RNGFETCH: local cache hit, merging hash=" << *hash;
        onAcquiredSidecarSet(existing);
        return;
    }

    // Trigger network fetch
    JLOG(j_.debug()) << "RNGFETCH: triggering network fetch hash=" << *hash;
    pendingRngFetches_.insert(*hash);
    if (auto immediate = app_.getInboundTransactions().getSet(*hash, true))
    {
        JLOG(j_.debug()) << "RNGFETCH: immediate fetch hit, merging hash="
                         << *hash;
        onAcquiredSidecarSet(immediate);
    }
}

void
ConsensusExtensions::fetchSidecarsIfNeeded(ExtendedPosition const& peerPos)
{
    fetchRngSetIfNeeded(peerPos.commitSetHash);
    fetchRngSetIfNeeded(peerPos.entropySetHash);
}

void
ConsensusExtensions::onPreBuild(CanonicalTXSet& retriableTxs, LedgerIndex seq)
{
    JLOG(j_.info()) << "RNG: injectEntropy seq=" << seq
                    << " commits=" << pendingCommits_.size()
                    << " reveals=" << pendingReveals_.size()
                    << " failed=" << entropyFailed_;

    uint256 finalEntropy;
    bool hasEntropy = false;

    //@@start rng-inject-entropy-selection
    // Calculate entropy from collected reveals
    if (app_.config().standalone())
    {
        // Standalone mode: generate synthetic deterministic entropy
        // so that Hook APIs (dice/random) work for testing.
        finalEntropy = sha512Half(std::string("standalone-entropy"), seq);
        hasEntropy = true;
        JLOG(j_.info()) << "RNG: Standalone synthetic entropy " << finalEntropy
                        << " for ledger " << seq;
    }
    else if (shouldZeroEntropy())
    {
        // Liveness fallback: inject zero entropy.
        // Hooks MUST check for zero to know entropy is unavailable.
        // shouldZeroEntropy() covers: pipeline failure, no reveals,
        // or sub-quorum reveals (too easily influenced by a minority).
        finalEntropy.zero();
        hasEntropy = true;
        JLOG(j_.warn()) << "RNG: Injecting ZERO entropy (fallback) for ledger "
                        << seq << " (reveals=" << pendingReveals_.size()
                        << " threshold=" << quorumThreshold() << ")";
    }
    else
    {
        // Sort reveals deterministically by Validator Public Key
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

            // Mix all reveals into final entropy
            Serializer s;
            for (auto const& [key, reveal] : sorted)
            {
                s.addVL(key.slice());
                s.addBitString(reveal);
            }
            finalEntropy = sha512Half(s.slice());
            hasEntropy = true;

            JLOG(j_.info()) << "RNG: Injecting entropy " << finalEntropy
                            << " from " << sorted.size() << " reveals"
                            << " for ledger " << seq;
        }
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
        auto const entropyCount = static_cast<std::uint16_t>(
            app_.config().standalone()
                ? 20  // synthetic: high enough for Hook APIs (need >= 5)
                : (shouldZeroEntropy() ? 0 : pendingReveals_.size()));
        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, finalEntropy);
            obj.setFieldU16(sfEntropyCount, entropyCount);
        });

        auto const txID = tx.getTransactionID();
        auto alreadyPresent = std::any_of(
            retriableTxs.begin(), retriableTxs.end(), [&](auto const& entry) {
                return entry.first.getTXID() == txID;
            });
        if (alreadyPresent)
        {
            JLOG(j_.debug())
                << "RNG: entropy pseudo-tx already present, skip duplicate "
                << txID;
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
    JLOG(j_.trace()) << "RNG: harvestRngData from " << nodeId
                     << " commit=" << (position.myCommitment ? "yes" : "no")
                     << " reveal=" << (position.myReveal ? "yes" : "no");

    //@@start rng-harvest-trust-and-reveal-verification
    // Reject data from validators not in the active UNL
    if (!isUNLReportMember(nodeId))
    {
        JLOG(j_.trace()) << "RNG: rejecting data from non-UNL validator "
                         << nodeId;
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
                        << "RNG: TESTING dropping claim from " << nodeId;
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
                << "Validator " << nodeId << " changed commitment from "
                << it->second << " to " << *position.myCommitment;
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
            JLOG(j_.trace()) << "Harvested commitment from " << nodeId << ": "
                             << *position.myCommitment;
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
            JLOG(j_.warn()) << "RNG: rejecting reveal from " << nodeId
                            << " (no commitment on record)";
            return;
        }

        // Verify Hash(reveal | pubKey | seq) == commitment
        auto const prevLgr = app_.getLedgerMaster().getLedgerByHash(prevLedger);
        if (!prevLgr)
        {
            JLOG(j_.warn()) << "RNG: cannot verify reveal from " << nodeId
                            << " (prevLedger not available)";
            return;
        }

        auto const seq = prevLgr->info().seq + 1;
        auto const calculated = sha512Half(*position.myReveal, publicKey, seq);

        if (calculated != commitIt->second)
        {
            JLOG(j_.warn()) << "RNG: fraudulent reveal from " << nodeId
                            << " (does not match commitment)";
            return;
        }

        auto [it, inserted] =
            pendingReveals_.emplace(nodeId, *position.myReveal);
        if (!inserted && it->second != *position.myReveal)
        {
            JLOG(j_.warn()) << "Validator " << nodeId << " changed reveal from "
                            << it->second << " to " << *position.myReveal;
            it->second = *position.myReveal;
        }
        else if (inserted)
        {
            JLOG(j_.trace()) << "Harvested reveal from " << nodeId << ": "
                             << *position.myReveal;
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
    cacheUNLReport();
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
                    << " myCommitment=" << (pos.myCommitment ? "yes" : "no")
                    << " myReveal=" << (pos.myReveal ? "yes" : "no");
}

//@@start peer-harvest-export-sigs
void
ConsensusExtensions::onTrustedPeerMessage(
    ::protocol::TMProposeSet const& wireMsg)
{
    if (wireMsg.exportsignatures_size() == 0)
        return;

    // Bind export sig pubkeys to the proposal sender.  Validators only
    // sign for themselves (see decorateMessage), so every blob's embedded
    // pubkey must match the proposal's nodepubkey.  Reject the entire
    // proposal's export sigs on any mismatch — a single impersonation
    // attempt means the sender is malicious.
    //
    // Two-pass: validate all blobs first, then commit — ensures no partial
    // state if a later blob fails the sender binding check.
    auto const senderSlice = makeSlice(wireMsg.nodepubkey());
    if (!publicKeyType(senderSlice))
        return;
    PublicKey const senderPK{senderSlice};

    if (!app_.validators().trusted(senderPK))
        return;

    // Pass 1: validate all blobs.
    for (int i = 0; i < wireMsg.exportsignatures_size(); ++i)
    {
        auto const& blob = wireMsg.exportsignatures(i);
        if (blob.size() < 65)
            continue;

        auto const pkSlice = makeSlice(blob).substr(32, 33);
        if (!publicKeyType(pkSlice))
            continue;

        if (PublicKey{pkSlice} != senderPK)
        {
            JLOG(j_.warn())
                << "Export: rejecting sigs from proposal — embedded pubkey "
                   "does not match sender";
            return;
        }
    }

    // Pass 2: verify multisign signatures and commit.
    // Look up each export tx from the open ledger to reconstruct the
    // signing data.  This runs on a jtPROPOSAL_t job queue thread
    // (post-C1 fix), so the verify() cost doesn't block the IO strand
    // or the single-threaded transactor.
    auto const openLedger = app_.openLedger().current();

    // Build a txHash -> STTx lookup for ttEXPORT txns in the open ledger.
    // Avoids O(N*M) iteration when processing multiple export sig blobs.
    std::unordered_map<uint256, std::shared_ptr<STTx const>> exportTxns;
    if (openLedger)
    {
        for (auto const& [stx, meta] : openLedger->txs)
        {
            if (stx && stx->getTxnType() == ttEXPORT)
                exportTxns.emplace(stx->getTransactionID(), stx);
        }
    }

    auto const signerAcctID = calcAccountID(senderPK);

    for (int i = 0; i < wireMsg.exportsignatures_size(); ++i)
    {
        auto const& blob = wireMsg.exportsignatures(i);
        // Each entry: txnHash (32) + validator pubkey (33) + sig (var)
        if (blob.size() < 65)
            continue;

        uint256 txHash;
        std::memcpy(txHash.data(), blob.data(), 32);

        if (blob.size() <= 65)
        {
            // Pubkey-only entry (no real signature) — skip.
            // Only verified sigs are stored in the collector.
            continue;
        }

        auto const fullSlice = makeSlice(blob);
        auto const sigSlice = fullSlice.substr(65);

        // Verify the multisign signature against the inner tx.
        // The ttEXPORT must be in our open ledger — validators only
        // sign exports they see in their open ledger (decorateMessage),
        // and we only receive proposals after consensus has started on
        // the same transaction set.  If the tx isn't found, reject —
        // don't store unverified sigs.
        auto const txIt = exportTxns.find(txHash);
        if (txIt == exportTxns.end() ||
            !txIt->second->isFieldPresent(sfExportedTxn))
        {
            JLOG(j_.debug()) << "Export: cannot verify sig for tx " << txHash
                             << " (not in open ledger) — rejected";
            continue;
        }

        try
        {
            auto const& exportedObj = const_cast<STTx&>(*txIt->second)
                                          .peekAtField(sfExportedTxn)
                                          .downcast<STObject>();

            Serializer innerSer;
            exportedObj.add(innerSer);
            SerialIter sit(innerSer.slice());
            STTx innerTx(std::ref(sit));

            auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
            if (!verify(senderPK, sigData.slice(), sigSlice))
            {
                JLOG(j_.warn()) << "Export: invalid multisign sig for tx "
                                << txHash << " — rejected";
                continue;
            }
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn()) << "Export: failed to verify sig for tx " << txHash
                            << ": " << e.what();
            continue;
        }

        Buffer sigBuf(sigSlice.data(), sigSlice.size());
        exportSigCollector_.addSignature(txHash, senderPK, sigBuf);
    }
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
        JLOG(j_.debug()) << "RNG: decoratePosition skipped (proposing="
                         << proposing << " amendment="
                         << prevLedger->rules().enabled(featureConsensusEntropy)
                         << ")";
        return;
    }

    setMode(ConsensusMode::proposing);
    cacheUNLReport();
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

    JLOG(j_.info()) << "RNG: decoratePosition bootstrap seq="
                    << (prevLedger->info().seq + 1)
                    << " commitment=" << *pos.myCommitment;
}
//@@end rng-bootstrap-commitment

//@@start export-sig-attachment
void
ConsensusExtensions::decorateMessage(
    protocol::TMProposeSet& prop,
    RCLCxPeerPos::Proposal const& proposal,
    Buffer const& proposalSig)
{
    auto const& valKeys = app_.getValidatorKeys();

    // Self-seed our own reveal so we count toward reveal quorum
    // (harvestRngData only sees peer proposals, not our own).
    if (proposal.position().myReveal)
    {
        pendingReveals_[valKeys.nodeID] = *proposal.position().myReveal;
        nodeIdToKey_.insert_or_assign(valKeys.nodeID, valKeys.keys->publicKey);
        JLOG(j_.trace()) << "RNG: self-seeded reveal for " << valKeys.nodeID;
    }

    // Store our own proposal proof for embedding in SHAMap entries.
    // commitProofs_ gets seq=0 only (deterministic commitSet).
    // proposalProofs_ gets the latest with a reveal (for entropySet).
    if (proposal.position().myCommitment || proposal.position().myReveal)
    {
        auto makeProof = [&]() {
            ProposalProof proof;
            proof.proposeSeq = proposal.proposeSeq();
            proof.closeTime = static_cast<std::uint32_t>(
                proposal.closeTime().time_since_epoch().count());
            proof.prevLedger = proposal.prevLedger();
            Serializer s;
            proposal.position().add(s);
            proof.positionData = std::move(s);
            proof.signature = Buffer(proposalSig.data(), proposalSig.size());
            return proof;
        };

        if (proposal.position().myCommitment && proposal.proposeSeq() == 0)
            commitProofs_.emplace(valKeys.nodeID, makeProof());

        if (proposal.position().myReveal)
            proposalProofs_[valKeys.nodeID] = makeProof();
    }

    // Attach export signatures for any ttEXPORT txns in the open ledger.
    // Gated on featureExport amendment.
    // XAHAUD_NO_EXPORT_SIG=1 disables sig attachment (for testing sub-quorum).
    if (auto const* noSig = std::getenv("XAHAUD_NO_EXPORT_SIG");
        noSig && std::string(noSig) == "1")
    {
        JLOG(j_.debug()) << "Export: XAHAUD_NO_EXPORT_SIG=1, skipping sigs";
        return;
    }

    auto const openLedger = app_.openLedger().current();
    if (!openLedger || !openLedger->rules().enabled(featureExport))
        return;

    auto const& valPK = valKeys.keys->publicKey;
    auto const& valSK = valKeys.keys->secretKey;
    auto const signerAcctID = calcAccountID(valPK);

    for (auto const& [stx, meta] : openLedger->txs)
    {
        if (!stx || stx->getTxnType() != ttEXPORT)
            continue;

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
                JLOG(j_.warn()) << "Export: failed to sign inner tx " << txHash
                                << ": " << e.what();
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
        //@@end export-attach-wire-sigs

        exportSigCollector_.addSignature(txHash, valPK, sigBuf);

        JLOG(j_.debug()) << "Export: attached sig for " << txHash
                         << " to proposal (sigLen=" << sigBuf.size() << ")";
    }
}
//@@end export-sig-attachment

ExtensionTickResult
ConsensusExtensions::onTick(TickContext const& ctx)
{
    return extensionsTick(*this, ctx);
}

}  // namespace ripple
