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
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
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
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/ExportShare.h>
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

namespace {

struct ResolvedExportShare
{
    std::shared_ptr<SLE const> latch;
    STTx exportSigningPayload;
};

enum class ExportShareResolutionStatus {
    resolved,
    deferred,
    duplicate,
    invalid
};

struct ExportShareResolution
{
    ExportShareResolutionStatus status;
    std::optional<ResolvedExportShare> value;
};

bool
isPendingExportShare(
    ReadView const& view,
    ExportShare const& share,
    beast::Journal j)
{
    if (share.originLedgerSeq > view.info().seq)
        return false;

    auto const originHash = share.originLedgerSeq == view.info().seq
        ? std::optional<uint256>{view.info().hash}
        : hashOfSeq(view, share.originLedgerSeq, j);
    if (!originHash || *originHash != share.originLedgerHash)
        return false;

    auto const latch =
        view.read(keylet::exportLatch(share.owner, share.originTxn));
    return latch && latch->getType() == ltEXPORT_LATCH &&
        latch->isFieldPresent(sfTransactionHash) &&
        latch->isFieldPresent(sfExportCommitteeHash) &&
        latch->isFieldPresent(sfLastLedgerSequence) &&
        latch->isFieldPresent(sfExportNode) &&
        !latch->isFieldPresent(sfExportSignatureHash) &&
        latch->getAccountID(sfAccount) == share.owner &&
        latch->getFieldH256(sfTransactionHash) == share.originTxn &&
        latch->getFieldU32(sfLedgerSequence) == share.originLedgerSeq &&
        view.info().seq <= latch->getFieldU32(sfLastLedgerSequence);
}

ExportShareResolution
resolveExportShare(
    Application& app,
    ExportShare const& share,
    std::shared_ptr<Ledger const> const& validated,
    beast::Journal j)
{
    if (!validated)
        return {ExportShareResolutionStatus::deferred, std::nullopt};
    if (!validated->rules().enabled(featureExport))
        return {ExportShareResolutionStatus::invalid, std::nullopt};
    if (share.originLedgerSeq > validated->info().seq)
        return {ExportShareResolutionStatus::deferred, std::nullopt};

    auto const canonicalOriginHash =
        share.originLedgerSeq == validated->info().seq
        ? std::optional<uint256>{validated->info().hash}
        : hashOfSeq(*validated, share.originLedgerSeq, j);
    if (!canonicalOriginHash)
        return {ExportShareResolutionStatus::deferred, std::nullopt};
    if (*canonicalOriginHash != share.originLedgerHash)
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    auto const latchKey = keylet::exportLatch(share.owner, share.originTxn);
    auto const latch = validated->read(latchKey);
    if (!latch)
        return {ExportShareResolutionStatus::invalid, std::nullopt};
    if (latch->getType() != ltEXPORT_LATCH ||
        !latch->isFieldPresent(sfTransactionHash) ||
        !latch->isFieldPresent(sfExportCommitteeHash) ||
        !latch->isFieldPresent(sfLastLedgerSequence) ||
        !latch->isFieldPresent(sfAccount) ||
        !latch->isFieldPresent(sfLedgerSequence) ||
        latch->getAccountID(sfAccount) != share.owner ||
        latch->getFieldH256(sfTransactionHash) != share.originTxn ||
        latch->getFieldU32(sfLedgerSequence) != share.originLedgerSeq)
        return {ExportShareResolutionStatus::invalid, std::nullopt};
    if (!latch->isFieldPresent(sfExportNode) ||
        latch->isFieldPresent(sfExportSignatureHash) ||
        validated->info().seq > latch->getFieldU32(sfLastLedgerSequence))
        return {ExportShareResolutionStatus::duplicate, std::nullopt};

    auto const originLedger =
        app.getLedgerMaster().getLedgerByHash(share.originLedgerHash);
    if (!originLedger)
        return {ExportShareResolutionStatus::deferred, std::nullopt};
    if (originLedger->info().seq != share.originLedgerSeq)
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    auto const [outer, _] = originLedger->txRead(share.originTxn);
    if (!outer || outer->getTxnType() != ttEXPORT ||
        !outer->isFieldPresent(sfExportedTxn) ||
        outer->getAccountID(sfAccount) != share.owner)
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    if (!outer->isFieldPresent(sfExportCommitteeHash) ||
        outer->getFieldH256(sfExportCommitteeHash) !=
            latch->getFieldH256(sfExportCommitteeHash))
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    auto const committeeHash = latch->getFieldH256(sfExportCommitteeHash);
    auto const committeeSLE =
        validated->read(keylet::exportCommittee(share.owner, committeeHash));
    if (!committeeSLE || !committeeSLE->isFieldPresent(sfExportCommittee))
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    auto const& roster = committeeSLE->getFieldVL(sfExportCommittee);
    if (!ExportLedgerOps::isMatchingExportCommittee(
            *committeeSLE, share.owner, committeeHash, makeSlice(roster)))
        return {ExportShareResolutionStatus::invalid, std::nullopt};
    auto const committee = resolveExportCommittee(makeSlice(roster));
    if (!committee || share.committeePosition >= committee->members.size())
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    auto const& expectedMaster = committee->members[share.committeePosition];
    auto const resolvedMaster =
        app.validatorManifests().getMasterKey(share.signingKey);
    if (resolvedMaster != expectedMaster)
    {
        // A known signing-to-master binding, or another committee master used
        // at this position, or any other known manifest master is
        // definitively invalid. An otherwise unknown key may become
        // attributable after manifest propagation.
        if (resolvedMaster != share.signingKey ||
            app.validatorManifests().isKnownMasterKey(share.signingKey) ||
            std::find(
                committee->members.begin(),
                committee->members.end(),
                share.signingKey) != committee->members.end())
            return {ExportShareResolutionStatus::invalid, std::nullopt};
        return {ExportShareResolutionStatus::duplicate, std::nullopt};
    }

    auto const baseTarget = ExportLedgerOps::exportIntentTarget(*outer);
    if (!baseTarget)
        return {ExportShareResolutionStatus::invalid, std::nullopt};
    auto const targetNetworkID = baseTarget->isFieldPresent(sfNetworkID)
        ? baseTarget->getFieldU32(sfNetworkID)
        : std::uint32_t{0};
    ExportOriginMemo::Origin const origin{
        app.config().NETWORK_ID, targetNetworkID, share.originTxn};
    auto const identity = ExportOriginMemo::identityForm(*baseTarget, origin);
    auto signingPayload = ExportOriginMemo::releaseForm(
        *baseTarget,
        origin,
        ExportOriginMemo::Anchor{
            share.originLedgerSeq, share.originLedgerHash});
    if (!identity || !signingPayload ||
        ExportResultBuilder::exportIntentHash(identity.value()) !=
            latch->getFieldH256(sfDigest))
        return {ExportShareResolutionStatus::invalid, std::nullopt};

    return {
        ExportShareResolutionStatus::resolved,
        ResolvedExportShare{latch, std::move(signingPayload.value())}};
}

ExportShare
withContribution(
    ExportShare const& context,
    ExportSigCollector::Contribution const& contribution)
{
    return ExportShare{
        context.version,
        context.owner,
        context.originTxn,
        context.originLedgerSeq,
        context.originLedgerHash,
        contribution.position,
        contribution.signingKey,
        contribution.signature};
}

std::map<uint256, std::shared_ptr<SLE const>>
pendingExportLatches(ReadView const& view, LedgerIndex eligibleSeq)
{
    std::map<uint256, std::shared_ptr<SLE const>> result;
    forEachItem(
        view,
        keylet::pendingExports(),
        [&](std::shared_ptr<SLE const> const& latch) {
            if (!latch || latch->getType() != ltEXPORT_LATCH ||
                !latch->isFieldPresent(sfTransactionHash) ||
                !latch->isFieldPresent(sfLedgerSequence) ||
                !latch->isFieldPresent(sfAccount) ||
                !latch->isFieldPresent(sfLastLedgerSequence) ||
                eligibleSeq > latch->getFieldU32(sfLastLedgerSequence) ||
                result.size() >= ExportLimits::maxLiveExportLatches)
                return;
            result.emplace(latch->getFieldH256(sfTransactionHash), latch);
        });
    return result;
}

}  // namespace

bool
ConsensusExtensions::publishExportShareLocked(
    ExportShare const& share,
    LedgerIndex const validatedLedgerSeq,
    uint256 const& validatedLedgerHash)
{
    if (!exportShareServiceStarted_.load(std::memory_order_acquire))
        return false;

    if (exportStreamEmissionSeq_ != validatedLedgerSeq)
    {
        exportStreamEmissionSeq_ = validatedLedgerSeq;
        exportStreamEmittedShares_.clear();
    }

    auto const identity = std::pair{share.originTxn, share.committeePosition};
    if (!exportStreamEmittedShares_.insert(identity).second)
        return false;

    app_.getOPs().pubExportSignature(
        share, validatedLedgerSeq, validatedLedgerHash);
    return true;
}

bool
ConsensusExtensions::onExportShare(ExportShare const& share)
{
    return admitExportShare(share, {}, true).isAccepted();
}

ExportShareAdmission
ConsensusExtensions::onExportShare(
    ExportShare const& share,
    ExportShareChargeHandler deferredCharge)
{
    return admitExportShare(share, std::move(deferredCharge), true);
}

ExportShareAdmission
ConsensusExtensions::deferExportShare(
    ExportShare const& share,
    ExportShareChargeHandler deferredCharge,
    LedgerIndex const validatedLedgerSeq)
{
    if (share.originLedgerSeq <= validatedLedgerSeq ||
        share.originLedgerSeq - validatedLedgerSeq >
            maxDeferredExportShareFutureLedgers_)
        return {ExportShareDisposition::deferred, ExportShareCharge::none};

    std::size_t const serializedBytes = share.serialize().size();
    auto const wireHash = share.wireHash();
    bool retryImmediately = false;

    {
        std::lock_guard lock(deferredExportSharesMutex_);
        if (!exportShareServiceStarted_.load(std::memory_order_acquire))
            return {ExportShareDisposition::duplicate, ExportShareCharge::none};

        // The validated callback may have passed this origin after admission
        // took its initial cursor snapshot. Do not queue behind that completed
        // retry pass.
        if (share.originLedgerSeq <= deferredExportShareRetrySeq_)
            retryImmediately = true;

        if (!retryImmediately)
        {
            if (auto const it = deferredExportShares_.find(wireHash);
                it != deferredExportShares_.end())
            {
                if (!it->second.charge && deferredCharge)
                    it->second.charge = std::move(deferredCharge);
                return {
                    ExportShareDisposition::duplicate, ExportShareCharge::none};
            }

            bool const newOrigin =
                !deferredExportShareOrigins_.contains(share.originTxn);
            if (deferredExportShares_.size() >= maxDeferredExportShares_ ||
                serializedBytes > maxDeferredExportShareBytes_ ||
                deferredExportShareBytes_ >
                    maxDeferredExportShareBytes_ - serializedBytes ||
                (newOrigin &&
                 deferredExportShareOrigins_.size() >=
                     maxDeferredExportShareOrigins_))
                return {
                    ExportShareDisposition::deferred, ExportShareCharge::none};

            deferredExportShareBytes_ += serializedBytes;
            ++deferredExportShareOrigins_[share.originTxn];
            deferredExportShares_.emplace(
                wireHash,
                DeferredExportShare{
                    share, std::move(deferredCharge), serializedBytes});
        }
    }
    if (retryImmediately)
        return admitExportShare(share, std::move(deferredCharge), false);
    return {ExportShareDisposition::deferred, ExportShareCharge::none};
}

void
ConsensusExtensions::retryDeferredExportShares(
    LedgerIndex const validatedLedgerSeq)
{
    std::vector<DeferredExportShare> retry;
    {
        std::lock_guard lock(deferredExportSharesMutex_);
        if (!exportShareServiceStarted_.load(std::memory_order_acquire))
            return;
        deferredExportShareRetrySeq_ =
            std::max(deferredExportShareRetrySeq_, validatedLedgerSeq);
        ++deferredExportShareRetriesInFlight_;
        for (auto it = deferredExportShares_.begin();
             it != deferredExportShares_.end();)
        {
            if (it->second.share.originLedgerSeq > validatedLedgerSeq)
            {
                ++it;
                continue;
            }

            auto const origin = it->second.share.originTxn;
            deferredExportShareBytes_ -= it->second.serializedBytes;
            auto const originIt = deferredExportShareOrigins_.find(origin);
            if (originIt != deferredExportShareOrigins_.end() &&
                --originIt->second == 0)
                deferredExportShareOrigins_.erase(originIt);
            retry.push_back(std::move(it->second));
            it = deferredExportShares_.erase(it);
        }
    }

    scope_exit retryDone([this] {
        std::lock_guard lock(deferredExportSharesMutex_);
        if (--deferredExportShareRetriesInFlight_ == 0)
            deferredExportShareRetriesDone_.notify_all();
    });

    for (auto& deferred : retry)
    {
        auto const result = admitExportShare(deferred.share, {}, false);
        if (result.isAccepted())
        {
            auto const frame = deferred.share.serialize();
            protocol::TMExportShares message;
            message.add_shares(frame.data(), frame.size());
            app_.overlay().broadcast(message);
        }
        else if (
            result.disposition == ExportShareDisposition::invalid &&
            result.charge != ExportShareCharge::none && deferred.charge)
        {
            deferred.charge(result.charge);
        }
    }
}

void
ConsensusExtensions::clearDeferredExportShares()
{
    std::lock_guard lock(deferredExportSharesMutex_);
    deferredExportShares_.clear();
    deferredExportShareOrigins_.clear();
    deferredExportShareBytes_ = 0;
}

ExportShareAdmission
ConsensusExtensions::admitExportShare(
    ExportShare const& share,
    ExportShareChargeHandler deferredCharge,
    bool const allowDeferral)
{
    if (!exportShareServiceStarted_.load(std::memory_order_acquire))
        return {ExportShareDisposition::duplicate, ExportShareCharge::none};

    auto const validated = app_.getLedgerMaster().getValidatedLedger();
    if (allowDeferral && validated &&
        share.originLedgerSeq > validated->info().seq)
        return deferExportShare(
            share, std::move(deferredCharge), validated->info().seq);

    auto resolved = resolveExportShare(app_, share, validated, j_);
    if (resolved.status == ExportShareResolutionStatus::deferred)
        return {ExportShareDisposition::deferred, ExportShareCharge::none};
    if (resolved.status == ExportShareResolutionStatus::duplicate)
        return {ExportShareDisposition::duplicate, ExportShareCharge::none};
    if (resolved.status == ExportShareResolutionStatus::invalid ||
        !resolved.value || !validated)
        return {
            ExportShareDisposition::invalid, ExportShareCharge::invalidData};

    if (!postValidationExportSigCollector_.registerOrigin(
            share.originTxn, validated->info().seq))
        return {ExportShareDisposition::deferred, ExportShareCharge::none};

    ExportSigCollector::Contribution contribution{
        share.committeePosition, share.signingKey, share.signature};
    auto admission = postValidationExportSigCollector_.beginAttributedAdmission(
        share.originTxn, std::move(contribution), validated->info().seq);
    if (admission.result != ExportSigCollector::BeginResult::verify ||
        !admission.ticket)
    {
        switch (admission.result)
        {
            case ExportSigCollector::BeginResult::duplicate:
            case ExportSigCollector::BeginResult::conflicted:
                return {
                    ExportShareDisposition::duplicate, ExportShareCharge::none};
            case ExportSigCollector::BeginResult::unknownOrigin:
            case ExportSigCollector::BeginResult::capacity:
                return {
                    ExportShareDisposition::deferred, ExportShareCharge::none};
            case ExportSigCollector::BeginResult::malformed:
                return {
                    ExportShareDisposition::invalid,
                    ExportShareCharge::invalidData};
            case ExportSigCollector::BeginResult::verify:
                break;
        }
        return {
            ExportShareDisposition::invalid, ExportShareCharge::invalidData};
    }

    auto const signer = calcAccountID(share.signingKey);
    auto const data =
        buildMultiSigningData(resolved.value->exportSigningPayload, signer);
    auto const signatureVerified = verify(
        share.signingKey,
        data.slice(),
        Slice{share.signature.data(), share.signature.size()});
    auto outcome = postValidationExportSigCollector_.admitContribution(
        std::move(*admission.ticket), signatureVerified, validated->info().seq);
    if (outcome.result == ExportSigCollector::AdmitResult::accepted)
    {
        std::lock_guard streamLock(exportStreamMutex_);
        if (!exportShareServiceStarted_.load(std::memory_order_acquire))
            return {ExportShareDisposition::duplicate, ExportShareCharge::none};
        auto const latest = app_.getLedgerMaster().getValidatedLedger();
        if (!latest || !isPendingExportShare(*latest, share, j_))
            return {ExportShareDisposition::duplicate, ExportShareCharge::none};
        publishExportShareLocked(
            share, latest->info().seq, latest->info().hash);
        return {ExportShareDisposition::accepted, ExportShareCharge::none};
    }
    if (outcome.result == ExportSigCollector::AdmitResult::invalid)
        return {
            ExportShareDisposition::invalid,
            ExportShareCharge::invalidSignature};
    if (outcome.result != ExportSigCollector::AdmitResult::conflicted ||
        !outcome.priorContribution || !outcome.conflictingContribution)
        return {ExportShareDisposition::duplicate, ExportShareCharge::none};

    // Propagate both valid encodings so every honest collector can observe the
    // same absorbing conflict even if it missed the first frame.
    protocol::TMExportShares conflict;
    for (auto const* contribution :
         {&*outcome.priorContribution, &*outcome.conflictingContribution})
    {
        auto const frame = withContribution(share, *contribution).serialize();
        conflict.add_shares(frame.data(), frame.size());
    }
    app_.overlay().broadcast(conflict);
    return {ExportShareDisposition::duplicate, ExportShareCharge::none};
}

void
ConsensusExtensions::onValidatedLedger(
    LedgerIndex const seq,
    uint256 const& hash) noexcept
{
    if (!exportShareServiceStarted_.load(std::memory_order_acquire))
        return;

    try
    {
        auto const eventLedger = app_.getLedgerMaster().getLedgerByHash(hash);
        auto const validated = app_.getLedgerMaster().getValidatedLedger();
        if (!eventLedger || eventLedger->info().seq != seq || !validated ||
            validated->info().seq < seq ||
            !validated->rules().enabled(featureExport))
            return;
        auto const canonicalEventHash = seq == validated->info().seq
            ? std::optional<uint256>{validated->info().hash}
            : hashOfSeq(*validated, seq, j_);
        if (!canonicalEventHash || *canonicalEventHash != hash)
            return;

        auto const cfg = app_.getRuntimeConfig().getConsensusTestConfig();
        if (cfg && cfg->noExportSig && *cfg->noExportSig)
            return;

        postValidationExportSigCollector_.cleanupStale(validated->info().seq);

        // Replay the retained share view once at each validated cursor. Shares
        // admitted concurrently are serialized by exportStreamMutex_: they
        // either appear in this replay or publish afterward as edge events.
        {
            std::lock_guard streamLock(exportStreamMutex_);
            if (!exportShareServiceStarted_.load(std::memory_order_acquire))
                return;
            if (lastExportReplaySeq_.load(std::memory_order_relaxed) <
                validated->info().seq)
            {
                lastExportReplaySeq_.store(
                    validated->info().seq, std::memory_order_relaxed);
                auto const live =
                    pendingExportLatches(*validated, validated->info().seq);
                auto const unionSnapshot =
                    postValidationExportSigCollector_.fullUnionSnapshot();
                for (auto const& [origin, latch] : live)
                {
                    auto const it = unionSnapshot.find(origin);
                    if (it == unionSnapshot.end())
                        continue;

                    auto const originSeq = latch->getFieldU32(sfLedgerSequence);
                    auto const originHash = originSeq == validated->info().seq
                        ? std::optional<uint256>{validated->info().hash}
                        : hashOfSeq(*validated, originSeq, j_);
                    if (!originHash)
                        continue;

                    for (auto const& contribution : it->second)
                    {
                        publishExportShareLocked(
                            ExportShare{
                                ExportShare::currentVersion,
                                latch->getAccountID(sfAccount),
                                origin,
                                originSeq,
                                *originHash,
                                contribution.position,
                                contribution.signingKey,
                                contribution.signature},
                            validated->info().seq,
                            validated->info().hash);
                    }
                }
            }
        }

        // Retained replay owns the start of this cursor. Deferred relay shares
        // are reconsidered afterward, so newly accepted entries publish as
        // edge events before this node authors its own shares below.
        retryDeferredExportShares(validated->info().seq);

        std::vector<ExportShare> shares;
        shares.reserve(ExportLimits::maxLiveExportLatches);
        auto const& keys = app_.getValidatorKeys();
        if (keys.keys && keys.nodeID != beast::zero)
        {
            forEachItem(
                *validated,
                keylet::pendingExports(),
                [&](std::shared_ptr<SLE const> const& latch) {
                    try
                    {
                        if (!latch ||
                            shares.size() >=
                                ExportLimits::maxLiveExportLatches ||
                            latch->getType() != ltEXPORT_LATCH ||
                            !latch->isFieldPresent(sfTransactionHash) ||
                            !latch->isFieldPresent(sfExportCommitteeHash) ||
                            !latch->isFieldPresent(sfLastLedgerSequence) ||
                            validated->info().seq >
                                latch->getFieldU32(sfLastLedgerSequence))
                            return;

                        auto const origin =
                            latch->getFieldH256(sfTransactionHash);
                        auto const originSeq =
                            latch->getFieldU32(sfLedgerSequence);
                        auto const originHash =
                            originSeq == validated->info().seq
                            ? std::optional<uint256>{validated->info().hash}
                            : hashOfSeq(*validated, originSeq, j_);
                        if (!originHash)
                            return;

                        auto const committeeHash =
                            latch->getFieldH256(sfExportCommitteeHash);
                        auto const committeeSLE =
                            validated->read(keylet::exportCommittee(
                                latch->getAccountID(sfAccount), committeeHash));
                        if (!committeeSLE ||
                            !committeeSLE->isFieldPresent(sfExportCommittee))
                            return;
                        auto const& roster =
                            committeeSLE->getFieldVL(sfExportCommittee);
                        if (!ExportLedgerOps::isMatchingExportCommittee(
                                *committeeSLE,
                                latch->getAccountID(sfAccount),
                                committeeHash,
                                makeSlice(roster)))
                            return;

                        auto const committee =
                            resolveExportCommittee(makeSlice(roster));
                        auto const position = committee
                            ? committee->position(keys.keys->masterPublicKey)
                            : std::nullopt;
                        if (!committee || !position)
                            return;
                        if (postValidationExportSigCollector_.positionStatus(
                                origin, *position) !=
                            ExportSigCollector::PositionStatus::empty)
                            return;

                        auto const originLedger =
                            app_.getLedgerMaster().getLedgerByHash(*originHash);
                        if (!originLedger)
                            return;
                        auto const [outer, _] = originLedger->txRead(origin);
                        if (!outer)
                            return;
                        auto const baseTarget =
                            ExportLedgerOps::exportIntentTarget(*outer);
                        if (!baseTarget)
                            return;

                        auto const targetNetworkID =
                            baseTarget->isFieldPresent(sfNetworkID)
                            ? baseTarget->getFieldU32(sfNetworkID)
                            : std::uint32_t{0};
                        auto signingPayload = ExportOriginMemo::releaseForm(
                            *baseTarget,
                            ExportOriginMemo::Origin{
                                app_.config().NETWORK_ID,
                                targetNetworkID,
                                origin},
                            ExportOriginMemo::Anchor{originSeq, *originHash});
                        if (!signingPayload)
                            return;

                        auto const signer = calcAccountID(keys.keys->publicKey);
                        auto const data = buildMultiSigningData(
                            signingPayload.value(), signer);
                        auto const signature = sign(
                            keys.keys->publicKey,
                            keys.keys->secretKey,
                            data.slice());
                        ExportShare share{
                            ExportShare::currentVersion,
                            latch->getAccountID(sfAccount),
                            origin,
                            originSeq,
                            *originHash,
                            *position,
                            keys.keys->publicKey,
                            signature};
                        if (onExportShare(share))
                            shares.push_back(std::move(share));
                    }
                    catch (std::exception const& e)
                    {
                        JLOG(j_.warn())
                            << "Export: skipped malformed pending latch"
                            << " ledgerSeq=" << validated->info().seq
                            << " error=" << e.what();
                    }
                });
        }

        for (std::size_t first = 0; first < shares.size();
             first += ExportLimits::maxExportSharesPerRelay)
        {
            protocol::TMExportShares message;
            auto const last = std::min(
                shares.size(), first + ExportLimits::maxExportSharesPerRelay);
            for (auto i = first; i < last; ++i)
            {
                auto const frame = shares[i].serialize();
                message.add_shares(frame.data(), frame.size());
            }
            app_.overlay().broadcast(message);
        }
    }
    catch (std::exception const& e)
    {
        JLOG(j_.error()) << "Export: validated release failed seq=" << seq
                         << " hash=" << hash << " error=" << e.what();
    }
    catch (...)
    {
        JLOG(j_.error()) << "Export: validated release failed seq=" << seq
                         << " hash=" << hash << " error=unknown";
    }
}

void
ConsensusExtensions::startExportShareService()
{
    clearDeferredExportShares();
    {
        std::lock_guard lock(deferredExportSharesMutex_);
        deferredExportShareRetrySeq_ = 0;
    }
    std::lock_guard streamLock(exportStreamMutex_);
    exportStreamEmissionSeq_ = 0;
    exportStreamEmittedShares_.clear();
    lastExportReplaySeq_.store(0, std::memory_order_relaxed);
    exportShareServiceStarted_.store(true, std::memory_order_release);
}

void
ConsensusExtensions::stopExportShareService() noexcept
{
    exportShareServiceStarted_.store(false, std::memory_order_release);
    {
        std::unique_lock lock(deferredExportSharesMutex_);
        deferredExportShareRetriesDone_.wait(
            lock, [this] { return deferredExportShareRetriesInFlight_ == 0; });
        deferredExportShares_.clear();
        deferredExportShareOrigins_.clear();
        deferredExportShareBytes_ = 0;
        deferredExportShareRetrySeq_ = 0;
    }
    {
        std::lock_guard streamLock(exportStreamMutex_);
        exportStreamEmissionSeq_ = 0;
        exportStreamEmittedShares_.clear();
    }
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
ConsensusExtensions::exportRootAlignmentThreshold() const
{
    return exportRootAlignmentThreshold(*activeValidatorView());
}

std::size_t
ConsensusExtensions::exportRootAlignmentThreshold(
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

    // Standalone: synthetic deterministic entropy so the entropy_cr_* draws work.
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
    // skip, so a fresh ConsensusEntropy entry always exists.
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
    return rngEnabledThisRound_.load(std::memory_order_relaxed);
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
ConsensusExtensions::exportEnabled() const
{
    return exportEnabledThisRound_.load(std::memory_order_relaxed);
}

bool
ConsensusExtensions::exportFinalizationViewAnchored() const
{
    return app_.config().standalone() || activeValidatorView()->fromUNLReport;
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
    buildingLedgerSeq_ = seq;

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
    buildingLedgerSeq_ = seq;

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

    auto const validated = app_.getLedgerMaster().getValidatedLedger();
    auto const live = validated
        ? pendingExportLatches(*validated, seq)
        : std::map<uint256, std::shared_ptr<SLE const>>{};
    auto const allSigs = postValidationExportSigCollector_.fullUnionSnapshot();
    std::size_t entryCount = 0;

    for (auto const& [origin, contributions] : allSigs)
    {
        if (live.find(origin) == live.end())
            continue;

        for (auto const& contribution : contributions)
        {
            STObject sidecar(sfGeneric);
            sidecar.setFieldU8(sfSidecarType, sidecarExportSig);
            sidecar.setFieldH256(sfTransactionHash, origin);
            sidecar.setFieldU32(sfTransactionIndex, contribution.position);
            sidecar.setFieldVL(
                sfSigningPubKey, contribution.signingKey.slice());
            sidecar.setFieldVL(
                sfTxnSignature,
                Slice{
                    contribution.signature.data(),
                    contribution.signature.size()});

            map->addItem(SHAMapNodeType::tnSIDECAR, makeSidecarItem(sidecar));
            ++entryCount;
        }
    }

    auto const maxExportSidecarLeaves =
        ExportLimits::maxLiveExportLatches * ExportLimits::maxCommitteeMembers;
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
                     << " liveOrigins=" << live.size();
    return hash;
}

bool
ConsensusExtensions::hasPendingExportSigs() const
{
    auto const validated = app_.getLedgerMaster().getValidatedLedger();
    if (!validated)
        return false;
    auto candidateSeq = buildingLedgerSeq_;
    if (!candidateSeq)
    {
        if (validated->info().seq == std::numeric_limits<LedgerIndex>::max())
            return false;
        candidateSeq = validated->info().seq + 1;
    }
    auto const live = pendingExportLatches(*validated, *candidateSeq);
    auto const allSigs = postValidationExportSigCollector_.fullUnionSnapshot();
    return std::any_of(allSigs.begin(), allSigs.end(), [&](auto const& entry) {
        return live.find(entry.first) != live.end();
    });
}

bool
ConsensusExtensions::hasEligiblePendingExports() const
{
    auto const validated = app_.getLedgerMaster().getValidatedLedger();
    if (!validated)
        return false;
    auto candidateSeq = buildingLedgerSeq_;
    if (!candidateSeq)
    {
        if (validated->info().seq == std::numeric_limits<LedgerIndex>::max())
            return false;
        candidateSeq = validated->info().seq + 1;
    }
    return !pendingExportLatches(*validated, *candidateSeq).empty();
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

std::optional<ConsensusExtensions::ExportWitnessMaterial>
ConsensusExtensions::agreedExportWitness(
    STTx const& exportSigningPayload,
    uint256 const& origin,
    std::size_t committeeSize,
    std::size_t threshold) const
{
    if (committeeSize == 0 ||
        committeeSize > ExportLimits::maxCommitteeMembers ||
        ExportLimits::committeeQuorumThreshold(committeeSize) != threshold)
        return std::nullopt;

    auto const acceptedHash = acceptedExportSigSetHash_
        ? *acceptedExportSigSetHash_
        : app_.config().standalone() && exportSigSetMap_
        ? exportSigSetMap_->getHash().as_uint256()
        : uint256{};
    if (acceptedHash.isZero())
        return std::nullopt;
    std::shared_ptr<SHAMap> agreedMap;
    if (exportSigSetMap_ &&
        exportSigSetMap_->getHash().as_uint256() == acceptedHash)
        agreedMap = exportSigSetMap_;
    else
        agreedMap = app_.getInboundTransactions().getSet(acceptedHash, false);

    if (!agreedMap || agreedMap->mapType() != SHAMapType::SIDECAR ||
        agreedMap->getHash().as_uint256() != acceptedHash)
        return std::nullopt;

    ExportWitnessMaterial material;
    std::set<std::uint32_t> positions;
    hash_set<AccountID> signerAccounts;
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
                    acceptedHash,
                    j_,
                    "Export",
                    "exportSigSet",
                    "witness",
                    ExportLimits::maxExportSignatureSidecarBytes,
                    &invalid);
                if (!admitted || admitted->type != sidecarExportSig)
                {
                    invalid = true;
                    return;
                }

                auto const& sidecar = admitted->sidecar;
                if (!sidecar.isFieldPresent(sfTransactionHash) ||
                    !sidecar.isFieldPresent(sfTransactionIndex) ||
                    !sidecar.isFieldPresent(sfSigningPubKey) ||
                    !sidecar.isFieldPresent(sfTxnSignature))
                {
                    invalid = true;
                    return;
                }
                if (sidecar.getFieldH256(sfTransactionHash) != origin)
                    return;

                auto const position = sidecar.getFieldU32(sfTransactionIndex);
                if (position >= committeeSize ||
                    !positions.insert(position).second)
                {
                    invalid = true;
                    return;
                }

                auto const keyBytes = sidecar.getFieldVL(sfSigningPubKey);
                if (!publicKeyType(makeSlice(keyBytes)))
                {
                    invalid = true;
                    return;
                }
                PublicKey const key{makeSlice(keyBytes)};
                auto const signer = calcAccountID(key);
                if (!signerAccounts.insert(signer).second)
                {
                    invalid = true;
                    return;
                }

                auto const signature = sidecar.getFieldVL(sfTxnSignature);
                auto const data =
                    buildMultiSigningData(exportSigningPayload, signer);
                if (signature.empty() ||
                    signature.size() >
                        ExportLimits::maxCanonicalExportSignatureBytes ||
                    !verify(key, data.slice(), makeSlice(signature)))
                {
                    invalid = true;
                    return;
                }

                auto const [_, inserted] = material.signatures.emplace(
                    static_cast<std::uint16_t>(position),
                    ExportResultBuilder::PositionedSignature{
                        key, Buffer{signature.data(), signature.size()}});
                if (!inserted)
                {
                    invalid = true;
                    return;
                }
            }
            catch (std::exception const& e)
            {
                JLOG(j_.warn())
                    << "Export: witness sidecar parse failed"
                    << " origin=" << origin << " setHash=" << acceptedHash
                    << " error=" << e.what();
                invalid = true;
            }
        });

    if (invalid || material.signatures.size() < threshold ||
        material.signatures.size() > ExportLimits::maxCommitteeMembers)
        return std::nullopt;
    return material;
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
    buildingLedgerSeq_.reset();
    roundPrevLedgerHash_ = uint256{};
    observedParticipantsHash_.reset();
    observedParticipantsCount_ = 0;
    observedParticipantsBitmapBin_.clear();
    likelyParticipants_.clear();
    commitProofs_.clear();
    //@@end round-stop-rng-reset
    // Keep the round-level enable latches intact here. onRoundStart() refreshes
    // them from the consensus parent before clearing so boundary cleanup, such
    // as the export-disabled signature wipe, observes the new round's rules.
}

void
ConsensusExtensions::clearRngState()
{
    //@@start round-stop-export-reset
    if (!exportEnabled())
    {
        // Export disabled marks a pre-activation amendment boundary. Drop any
        // locally cached material that cannot belong to an enabled ancestry.
        postValidationExportSigCollector_.clearAll();
    }
    exportSigSetMap_.reset();
    acceptedExportSigSetHash_.reset();
    proposalPublishedExportShares_.clear();
    exportSigGateStarted_ = false;
    exportSigGateStart_ = {};
    exportSigConvergenceFailed_ = false;
    //@@end round-stop-export-reset

    clearRngStatePreservingExport();
}

void
ConsensusExtensions::onReplayBuild()
{
    if (rngEnabled() || exportEnabled())
        clearRngStatePreservingExport();
    else
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
    //@@start participant-diagnostics-feature-gate
    if (!rngEnabled() && !exportEnabled())
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
    std::vector<uint256> suppliedExportWitnesses;

    for (auto const& item : *agreedTxs.map_)
    {
        try
        {
            STTx const tx{SerialIter{item.slice()}};
            if (tx.getTxnType() == ttCONSENSUS_ENTROPY)
                suppliedEntropy.push_back(item.key());
            else if (tx.getTxnType() == ttEXPORT_SIGNATURES)
                suppliedExportWitnesses.push_back(item.key());
        }
        catch (std::exception const&)
        {
            // Preserve malformed entries here. The existing canonical-set
            // construction path records their parse failure separately.
        }
    }

    for (auto const& id : suppliedEntropy)
        mutableSet.erase(id);
    for (auto const& id : suppliedExportWitnesses)
        mutableSet.erase(id);

    return LiveBuildTxSet{
        RCLTxSet{mutableSet},
        suppliedEntropy.size(),
        suppliedExportWitnesses.size()};
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
    std::size_t suppliedExportWitnesses = 0;
    for (auto it = retriableTxs.begin(); it != retriableTxs.end();)
    {
        auto const& tx = it->second;
        if (tx && tx->getTxnType() == ttCONSENSUS_ENTROPY)
        {
            ++suppliedEntropy;
            it = retriableTxs.erase(it);
        }
        else if (tx && tx->getTxnType() == ttEXPORT_SIGNATURES)
        {
            ++suppliedExportWitnesses;
            it = retriableTxs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (suppliedEntropy != 0 || suppliedExportWitnesses != 0)
    {
        JLOG(j_.error())
            << "ConsensusExtensions: discarded supplied synthetic txs"
            << " seq=" << seq << " entropy=" << suppliedEntropy
            << " exportWitnesses=" << suppliedExportWitnesses;
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

    if (exportEnabled() && exportFinalizationViewAnchored())
    {
        //@@start export-later-ledger-witness-materialization
        // Standalone has no peer-position gate to build and accept a sidecar
        // root. Materialize the same locally verified union that
        // agreedExportWitness() explicitly accepts in standalone mode.
        if (app_.config().standalone() && hasPendingExportSigs())
            buildExportSigSet(seq);

        auto const parent =
            app_.getLedgerMaster().getLedgerByHash(roundPrevLedgerHash_);
        auto const validated = app_.getLedgerMaster().getValidatedLedger();
        bool parentExtendsValidated =
            parent && validated && validated->info().seq <= parent->info().seq;
        if (parentExtendsValidated &&
            validated->info().seq < parent->info().seq)
        {
            auto const validatedHash =
                hashOfSeq(*parent, validated->info().seq, j_);
            parentExtendsValidated =
                validatedHash && *validatedHash == validated->info().hash;
        }
        else if (parentExtendsValidated)
        {
            parentExtendsValidated =
                parent->info().hash == validated->info().hash;
        }

        if (parentExtendsValidated)
        {
            auto const pending = pendingExportLatches(*parent, seq);
            for (auto const& [origin, latch] : pending)
            {
                if (!latch->isFieldPresent(sfExportCommitteeHash) ||
                    !latch->isFieldPresent(sfLastLedgerSequence) ||
                    seq > latch->getFieldU32(sfLastLedgerSequence))
                    continue;

                auto const originSeq = latch->getFieldU32(sfLedgerSequence);
                auto const originHash = hashOfSeq(*parent, originSeq, j_);
                auto const validatedOriginHash =
                    originSeq == validated->info().seq
                    ? std::optional<uint256>{validated->info().hash}
                    : hashOfSeq(*validated, originSeq, j_);
                if (!originHash || !validatedOriginHash ||
                    *originHash != *validatedOriginHash)
                    continue;

                auto const originLedger =
                    app_.getLedgerMaster().getLedgerByHash(*originHash);
                if (!originLedger)
                    continue;
                auto const [outer, _] = originLedger->txRead(origin);
                if (!outer || outer->getTxnType() != ttEXPORT)
                    continue;
                auto const baseTarget =
                    ExportLedgerOps::exportIntentTarget(*outer);
                if (!baseTarget)
                    continue;

                auto const committeeHash =
                    latch->getFieldH256(sfExportCommitteeHash);
                auto const committeeSLE = parent->read(keylet::exportCommittee(
                    latch->getAccountID(sfAccount), committeeHash));
                if (!committeeSLE ||
                    !committeeSLE->isFieldPresent(sfExportCommittee))
                    continue;

                auto const& roster =
                    committeeSLE->getFieldVL(sfExportCommittee);
                if (!ExportLedgerOps::isMatchingExportCommittee(
                        *committeeSLE,
                        latch->getAccountID(sfAccount),
                        committeeHash,
                        makeSlice(roster)))
                    continue;
                auto const committee =
                    resolveExportCommittee(makeSlice(roster));
                if (!committee)
                    continue;

                auto const targetNetworkID =
                    baseTarget->isFieldPresent(sfNetworkID)
                    ? baseTarget->getFieldU32(sfNetworkID)
                    : std::uint32_t{0};
                auto const signingPayload = ExportOriginMemo::releaseForm(
                    *baseTarget,
                    ExportOriginMemo::Origin{
                        app_.config().NETWORK_ID, targetNetworkID, origin},
                    ExportOriginMemo::Anchor{originSeq, *originHash});
                if (!signingPayload)
                    continue;

                auto material = agreedExportWitness(
                    signingPayload.value(),
                    origin,
                    committee->members.size(),
                    committee->quorum);
                if (!material)
                    continue;

                try
                {
                    auto witness = ExportResultBuilder::buildSignatureWitness(
                        origin,
                        signingPayload.value(),
                        material->signatures,
                        committee->members.size(),
                        seq);
                    retriableTxs.insert(
                        std::make_shared<STTx>(std::move(witness)));
                }
                catch (std::invalid_argument const& e)
                {
                    // Admission and the sidecar caps make this unreachable for
                    // valid state. Keep one inconsistent Export from aborting
                    // the entire ledger build if those bounds ever drift.
                    JLOG(j_.error())
                        << "Export: skipping invalid witness materialization"
                        << " origin=" << origin << " buildSeq=" << seq
                        << " reason=" << e.what();
                }
            }
        }
        //@@end export-later-ledger-witness-materialization
    }
    else if (exportEnabled())
    {
        // Admission currently makes this unreachable for a live latch because
        // UNLReport state persists in descendants. Retain the materialization
        // guard so a future report-expiry or clearing rule cannot silently
        // turn node-local configured trust into ledger-defining authority.
        JLOG(j_.warn()) << "Export: skipping witness materialization"
                        << " reason=no-unl-report"
                        << " buildSeq=" << seq;
    }

    //@@start accept-time-cleanup-success
    // Export's ledger-defining signature witnesses are now self-contained
    // transactions in the stream; no later apply step reads sidecar memory.
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
    auto const& rules = prevLedger.ledger_->rules();
    //@@start round-extension-feature-latches
    setRngEnabledThisRound(rules.enabled(featureConsensusEntropy));
    setExportEnabledThisRound(rules.enabled(featureExport));
    //@@end round-extension-feature-latches
    clearRngState();

    roundPrevLedgerHash_ = prevLedger.ledger_->info().hash;
    buildingLedgerSeq_ = prevLedger.ledger_->info().seq + 1;
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
    uint256 const&,
    std::vector<std::string> const& exportSignatures,
    char const*)
{
    if (!exportEnabled() || exportSignatures.empty() ||
        exportSignatures.size() > ExportLimits::maxExportSharesPerRelay)
        return 0;

    std::size_t accepted = 0;
    for (auto const& bytes : exportSignatures)
    {
        auto const share = ExportShare::parse(makeSlice(bytes));
        if (!share || share->signingKey != senderPK ||
            share->serialize().slice() != makeSlice(bytes))
            continue;
        if (onExportShare(*share))
            ++accepted;
    }
    return accepted;
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
            ExportLimits::maxSerializedExportShareBytes)
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

//@@start export-sig-attachment
void
ConsensusExtensions::attachExportSignatures(
    protocol::TMProposeSet& prop,
    RCLCxPeerPos::Proposal const& proposal)
{
    if (!exportEnabled())
        return;

    auto const cfg = app_.getRuntimeConfig().getConsensusTestConfig();
    if (cfg && cfg->noExportSig && *cfg->noExportSig)
        return;

    auto const& keys = app_.getValidatorKeys();
    auto const validated = app_.getLedgerMaster().getValidatedLedger();
    if (!keys.keys || keys.nodeID == beast::zero || !validated ||
        !validated->rules().enabled(featureExport) || !buildingLedgerSeq_ ||
        proposal.prevLedger() != roundPrevLedgerHash_)
        return;

    auto const live = pendingExportLatches(*validated, *buildingLedgerSeq_);
    auto const snapshot = postValidationExportSigCollector_.fullUnionSnapshot();
    std::size_t attached = 0;
    for (auto const& [origin, contributions] : snapshot)
    {
        auto const latchIt = live.find(origin);
        if (latchIt == live.end())
            continue;
        auto const& latch = latchIt->second;
        auto const originSeq = latch->getFieldU32(sfLedgerSequence);
        auto const originHash = originSeq == validated->info().seq
            ? std::optional<uint256>{validated->info().hash}
            : hashOfSeq(*validated, originSeq, j_);
        if (!originHash)
            continue;

        for (auto const& contribution : contributions)
        {
            if (attached >= ExportLimits::maxExportSharesPerRelay)
                return;
            if (contribution.signingKey != keys.keys->publicKey)
                continue;
            auto const identity =
                std::pair<uint256, ExportSigCollector::Position>{
                    origin, contribution.position};
            if (proposalPublishedExportShares_.count(identity) != 0)
                continue;

            ExportShare share{
                ExportShare::currentVersion,
                latch->getAccountID(sfAccount),
                origin,
                originSeq,
                *originHash,
                contribution.position,
                contribution.signingKey,
                contribution.signature};
            if (resolveExportShare(app_, share, validated, j_).status !=
                ExportShareResolutionStatus::resolved)
                continue;

            auto const frame = share.serialize();
            prop.add_exportsignatures(frame.data(), frame.size());
            proposalPublishedExportShares_.insert(identity);
            ++attached;
        }
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
    return extensionsTick(*this, ctx);
}

}  // namespace ripple
