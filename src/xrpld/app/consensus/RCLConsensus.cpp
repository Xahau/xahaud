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
#include <xrpld/app/consensus/RCLConsensus.h>
#include <xrpld/app/consensus/RCLValidations.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/InboundLedgers.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LedgerReplay.h>
#include <xrpld/app/ledger/LocalTxs.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/AmendmentTable.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/LoadFeeTrack.h>
#include <xrpld/app/misc/NegativeUNLVote.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/consensus/Consensus.h>
#include <xrpld/consensus/LedgerTiming.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/overlay/predicates.h>
#include <xrpl/basics/random.h>
#include <xrpl/beast/core/LexicalCast.h>
#include <xrpl/crypto/csprng.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/BuildInfo.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>

namespace ripple {

RCLConsensus::RCLConsensus(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    clock_type const& clock,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : adaptor_(
          app,
          std::move(feeVote),
          ledgerMaster,
          localTxs,
          inboundTransactions,
          validatorKeys,
          journal)
    , consensus_(std::make_unique<Consensus<Adaptor>>(clock, adaptor_, journal))
    , j_(journal)
{
}

RCLConsensus::~RCLConsensus() = default;

RCLConsensus::Adaptor::Adaptor(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    ValidatorKeys const& validatorKeys,
    beast::Journal journal)
    : app_(app)
    , feeVote_(std::move(feeVote))
    , ledgerMaster_(ledgerMaster)
    , localTxs_(localTxs)
    , inboundTransactions_{inboundTransactions}
    , j_(journal)
    , validatorKeys_(validatorKeys)
    , valCookie_(
          1 +
          rand_int(
              crypto_prng(),
              std::numeric_limits<std::uint64_t>::max() - 1))
    , nUnlVote_(validatorKeys_.nodeID, j_, app)
{
    XRPL_ASSERT(
        valCookie_, "ripple::RCLConsensus::Adaptor::Adaptor : nonzero cookie");

    JLOG(j_.info()) << "Consensus engine started (cookie: " +
            std::to_string(valCookie_) + ")";

    if (validatorKeys_.nodeID != beast::zero && validatorKeys_.keys)
    {
        std::stringstream ss;

        JLOG(j_.info()) << "Validator identity: "
                        << toBase58(
                               TokenType::NodePublic,
                               validatorKeys_.keys->masterPublicKey);

        if (validatorKeys_.keys->masterPublicKey !=
            validatorKeys_.keys->publicKey)
        {
            JLOG(j_.debug())
                << "Validator ephemeral signing key: "
                << toBase58(
                       TokenType::NodePublic, validatorKeys_.keys->publicKey)
                << " (seq: " << std::to_string(validatorKeys_.sequence) << ")";
        }
    }
}

// --- ConsensusExtensions helpers ---

ConsensusExtensions&
RCLConsensus::Adaptor::ce()
{
    return app_.getConsensusExtensions();
}

ConsensusExtensions const&
RCLConsensus::Adaptor::ce() const
{
    return app_.getConsensusExtensions();
}

// --- End ConsensusExtensions helpers ---

std::optional<RCLCxLedger>
RCLConsensus::Adaptor::acquireLedger(LedgerHash const& hash)
{
    // we need to switch the ledger we're working from
    auto built = ledgerMaster_.getLedgerByHash(hash);
    if (!built)
    {
        if (acquiringLedger_ != hash)
        {
            // need to start acquiring the correct consensus LCL
            JLOG(j_.warn()) << "Need consensus ledger " << hash;

            // Tell the ledger acquire system that we need the consensus ledger
            acquiringLedger_ = hash;

            app_.getJobQueue().addJob(
                jtADVANCE,
                "getConsensusLedger1",
                [id = hash, &app = app_, this]() {
                    JLOG(j_.debug())
                        << "JOB advanceLedger getConsensusLedger1 started";
                    app.getInboundLedgers().acquireAsync(
                        id, 0, InboundLedger::Reason::CONSENSUS);
                });
        }
        return std::nullopt;
    }

    XRPL_ASSERT(
        !built->open() && built->isImmutable(),
        "ripple::RCLConsensus::Adaptor::acquireLedger : valid ledger state");
    XRPL_ASSERT(
        built->info().hash == hash,
        "ripple::RCLConsensus::Adaptor::acquireLedger : ledger hash match");

    // Notify inbound transactions of the new ledger sequence number
    inboundTransactions_.newRound(built->info().seq);

    return RCLCxLedger(built);
}

void
RCLConsensus::Adaptor::share(RCLCxPeerPos const& peerPos)
{
    protocol::TMProposeSet prop;

    auto const& proposal = peerPos.proposal();

    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());

    //@@start relay-proposal-position-payload
    // Serialize full ExtendedPosition
    Serializer positionData;
    proposal.position().add(positionData);
    auto const posSlice = positionData.slice();
    prop.set_currenttxhash(posSlice.data(), posSlice.size());
    //@@end relay-proposal-position-payload

    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.prevLedger().size());

    auto const pk = peerPos.publicKey().slice();
    prop.set_nodepubkey(pk.data(), pk.size());

    auto const sig = peerPos.signature();
    prop.set_signature(sig.data(), sig.size());

    for (auto const& exportSig : peerPos.exportSignatures())
        prop.add_exportsignatures(exportSig.data(), exportSig.size());

    app_.overlay().relay(prop, peerPos.suppressionID(), peerPos.publicKey());
}

void
RCLConsensus::Adaptor::share(RCLCxTx const& tx)
{
    // If we didn't relay this transaction recently, relay it to all peers
    if (app_.getHashRouter().shouldRelay(tx.id()))
    {
        JLOG(j_.debug()) << "Relaying disputed tx " << tx.id();
        auto const slice = tx.tx_->slice();
        protocol::TMTransaction msg;
        msg.set_rawtransaction(slice.data(), slice.size());
        msg.set_status(protocol::tsNEW);
        msg.set_receivetimestamp(
            app_.timeKeeper().now().time_since_epoch().count());
        static std::set<Peer::id_t> skip{};
        app_.overlay().relay(tx.id(), msg, skip);
    }
    else
    {
        JLOG(j_.debug()) << "Not relaying disputed tx " << tx.id();
    }
}
void
RCLConsensus::Adaptor::propose(RCLCxPeerPos::Proposal const& proposal)
{
    if (!validatorKeys_.keys)
    {
        // Proposal packets are signed consensus messages. Observing nodes can
        // follow consensus, but cannot safely author proposals without a
        // configured validator key.
        JLOG(j_.warn()) << "Skipping proposal without validator keys";
        return;
    }

    JLOG(j_.trace()) << (proposal.isBowOut() ? "We bow out: " : "We propose: ")
                     << ripple::to_string(proposal.prevLedger()) << " -> "
                     << ripple::to_string(proposal.position());

    protocol::TMProposeSet prop;

    auto wirePosition = proposal.position();

    ce().attachExportSignatures(prop, proposal);
    if (prop.exportsignatures_size() > 0)
        wirePosition.exportSignaturesHash =
            proposalExportSignaturesHash(prop.exportsignatures());

    ce().attachParticipantDiagnostics(wirePosition);

    //@@start local-proposal-position-payload
    // Serialize full ExtendedPosition (includes RNG leaves, export signature
    // digest, and signed diagnostics)
    Serializer positionData;
    wirePosition.add(positionData);
    auto const posSlice = positionData.slice();
    prop.set_currenttxhash(posSlice.data(), posSlice.size());
    //@@end local-proposal-position-payload

    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.prevLedger().size());
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());
    prop.set_nodepubkey(
        validatorKeys_.keys->publicKey.data(),
        validatorKeys_.keys->publicKey.size());

    auto sig = signDigest(
        validatorKeys_.keys->publicKey,
        validatorKeys_.keys->secretKey,
        sha512Half(
            HashPrefix::proposal,
            std::uint32_t(proposal.proposeSeq()),
            proposal.closeTime().time_since_epoch().count(),
            proposal.prevLedger(),
            wirePosition));

    prop.set_signature(sig.data(), sig.size());

    auto const suppression = proposalUniqueId(
        wirePosition,
        proposal.prevLedger(),
        proposal.proposeSeq(),
        proposal.closeTime(),
        validatorKeys_.keys->publicKey,
        sig);

    app_.getHashRouter().addSuppression(suppression);

    ce().decorateMessage(prop, proposal, wirePosition, sig);

    app_.overlay().broadcast(prop);
}

void
RCLConsensus::Adaptor::share(RCLTxSet const& txns)
{
    inboundTransactions_.giveSet(txns.id(), txns.map_, false);
}

std::optional<RCLTxSet>
RCLConsensus::Adaptor::acquireTxSet(RCLTxSet::ID const& setId)
{
    if (auto txns = inboundTransactions_.getSet(setId, true))
    {
        return RCLTxSet{std::move(txns)};
    }
    return std::nullopt;
}

bool
RCLConsensus::Adaptor::hasOpenTransactions() const
{
    return !app_.openLedger().empty();
}

std::size_t
RCLConsensus::Adaptor::proposersValidated(LedgerHash const& h) const
{
    return app_.getValidations().numTrustedForLedger(h);
}

std::size_t
RCLConsensus::Adaptor::proposersFinished(
    RCLCxLedger const& ledger,
    LedgerHash const& h) const
{
    RCLValidations& vals = app_.getValidations();
    return vals.getNodesAfter(
        RCLValidatedLedger(ledger.ledger_, vals.adaptor().journal()), h);
}

uint256
RCLConsensus::Adaptor::getPrevLedger(
    uint256 ledgerID,
    RCLCxLedger const& ledger,
    ConsensusMode mode)
{
    RCLValidations& vals = app_.getValidations();
    uint256 netLgr = vals.getPreferred(
        RCLValidatedLedger{ledger.ledger_, vals.adaptor().journal()},
        ledgerMaster_.getValidLedgerIndex());

    if (netLgr != ledgerID)
    {
        if (mode != ConsensusMode::wrongLedger)
            app_.getOPs().consensusViewChange();

        JLOG(j_.debug()) << Json::Compact(app_.getValidations().getJsonTrie());
    }

    return netLgr;
}

auto
RCLConsensus::Adaptor::onClose(
    RCLCxLedger const& ledger,
    NetClock::time_point const& closeTime,
    ConsensusMode mode) -> Result
{
    const bool wrongLCL = mode == ConsensusMode::wrongLedger;
    const bool proposing = mode == ConsensusMode::proposing;

    notify(protocol::neCLOSING_LEDGER, ledger, !wrongLCL);

    auto const& prevLedger = ledger.ledger_;

    ledgerMaster_.applyHeldTransactions();
    // Tell the ledger master not to acquire the ledger we're probably building
    ledgerMaster_.setBuildingLedger(prevLedger->info().seq + 1);

    auto initialLedger = app_.openLedger().current();

    auto initialSet =
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    initialSet->setUnbacked();

    // Build SHAMap containing all transactions in our open ledger
    for (auto const& tx : initialLedger->txs)
    {
        JLOG(j_.trace()) << "Adding open ledger TX "
                         << tx.first->getTransactionID();
        Serializer s(2048);
        tx.first->add(s);
        initialSet->addItem(
            SHAMapNodeType::tnTRANSACTION_NM,
            make_shamapitem(tx.first->getTransactionID(), s.slice()));
    }

    // Add pseudo-transactions to the set
    if (app_.config().standalone() || (proposing && !wrongLCL))
    {
        if (prevLedger->isFlagLedger())
        {
            // previous ledger was flag ledger, add fee and amendment
            // pseudo-transactions
            auto validations = app_.validators().negativeUNLFilter(
                app_.getValidations().getTrustedForLedger(
                    prevLedger->info().parentHash, prevLedger->seq() - 1));
            if (validations.size() >= app_.validators().quorum())
            {
                feeVote_->doVoting(prevLedger, validations, initialSet);
                app_.getAmendmentTable().doVoting(
                    prevLedger, validations, initialSet, j_);
            }
        }
        else if (
            prevLedger->isVotingLedger() &&
            prevLedger->rules().enabled(featureNegativeUNL))
        {
            // previous ledger was a voting ledger,
            // so the current consensus session is for a flag ledger,
            // add negative UNL pseudo-transactions
            //@@start negative-unl-vote-trusted-denominator
            nUnlVote_.doVoting(
                prevLedger,
                app_.validators().getTrustedMasterKeys(),
                app_.getValidations(),
                initialSet);
            //@@end negative-unl-vote-trusted-denominator
        }
    }

    // Now we need an immutable snapshot
    initialSet = initialSet->snapShot(false);

    if (!wrongLCL)
    {
        LedgerIndex const seq = prevLedger->info().seq + 1;
        RCLCensorshipDetector<TxID, LedgerIndex>::TxIDSeqVec proposed;

        initialSet->visitLeaves(
            [&proposed,
             seq](boost::intrusive_ptr<SHAMapItem const> const& item) {
                proposed.emplace_back(item->key(), seq);
            });

        censorshipDetector_.propose(std::move(proposed));
    }

    // Needed because of the move below.
    auto const setHash = initialSet->getHash().as_uint256();

    ExtendedPosition pos{setHash};

    ce().decoratePosition(pos, prevLedger, proposing);

    return Result{
        std::move(initialSet),
        RCLCxPeerPos::Proposal{
            initialLedger->info().parentHash,
            RCLCxPeerPos::Proposal::seqJoin,
            std::move(pos),
            closeTime,
            app_.timeKeeper().closeTime(),
            validatorKeys_.nodeID}};
}

void
RCLConsensus::Adaptor::onForceAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value&& consensusJson)
{
    doAccept(
        result,
        prevLedger,
        closeResolution,
        rawCloseTimes,
        mode,
        std::move(consensusJson));
}

void
RCLConsensus::Adaptor::onAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration const& closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value&& consensusJson,
    const bool validating)
{
    app_.getJobQueue().addJob(
        jtACCEPT,
        "acceptLedger",
        [=, this, cj = std::move(consensusJson)]() mutable {
            //@@start do-accept-freeze-contract
            // Note that no lock is held or acquired during this job.
            // This is because generic Consensus guarantees that once a ledger
            // is accepted, the consensus results and capture by reference state
            // will not change until startRound is called (which happens via
            // endConsensus).
            //@@end do-accept-freeze-contract
            RclConsensusLogger clog("onAccept", validating, j_);
            this->doAccept(
                result,
                prevLedger,
                closeResolution,
                rawCloseTimes,
                mode,
                std::move(cj));
            this->app_.getOPs().endConsensus(clog.ss());
        });
}

void
RCLConsensus::Adaptor::doAccept(
    Result const& result,
    RCLCxLedger const& prevLedger,
    NetClock::duration closeResolution,
    ConsensusCloseTimes const& rawCloseTimes,
    ConsensusMode const& mode,
    Json::Value&& consensusJson)
{
    prevProposers_ = result.proposers;
    prevRoundTime_ = result.roundTime.read();

    bool closeTimeCorrect;

    const bool proposing = mode == ConsensusMode::proposing;
    const bool haveCorrectLCL = mode != ConsensusMode::wrongLedger;
    const bool consensusFail = result.state == ConsensusState::MovedOn;

    auto consensusCloseTime = result.position.closeTime();

    if (consensusCloseTime == NetClock::time_point{})
    {
        // We agreed to disagree on the close time
        using namespace std::chrono_literals;
        consensusCloseTime = prevLedger.closeTime() + 1s;
        closeTimeCorrect = false;
    }
    else
    {
        // We agreed on a close time
        consensusCloseTime = effCloseTime(
            consensusCloseTime, closeResolution, prevLedger.closeTime());
        closeTimeCorrect = true;
    }

    JLOG(j_.debug()) << "Report: Prop=" << (proposing ? "yes" : "no")
                     << " val=" << (validating_ ? "yes" : "no")
                     << " corLCL=" << (haveCorrectLCL ? "yes" : "no")
                     << " fail=" << (consensusFail ? "yes" : "no");
    JLOG(j_.debug()) << "Report: Prev = " << prevLedger.id() << ":"
                     << prevLedger.seq();

    //--------------------------------------------------------------------------
    std::set<TxID> failed;

    // Select replay before choosing the transaction stream used to derive the
    // ledger. Replay consumes the persisted ordered stream. A live build uses
    // a sanitized view of the agreed set so supplied extension pseudos cannot
    // influence fallback entropy, transaction ordering, or ledger state.
    auto replayData = ledgerMaster_.releaseReplay();
    auto const consensusTxSetHash = result.txns.id();
    auto const liveBuild = replayData
        ? std::optional<ConsensusExtensions::LiveBuildTxSet>{}
        : std::optional<ConsensusExtensions::LiveBuildTxSet>{
              ce().makeLiveBuildTxSet(result.txns)};
    auto const& buildTxs = liveBuild ? liveBuild->txns : result.txns;
    auto const buildTxSetHash = buildTxs.id();

    if (liveBuild &&
        (liveBuild->suppliedEntropy != 0 ||
         liveBuild->suppliedExportWitnesses != 0))
    {
        JLOG(j_.error())
            << "ConsensusExtensions: excluded supplied synthetic txs from "
               "live build"
            << " seq=" << (prevLedger.seq() + 1)
            << " consensusSet=" << consensusTxSetHash
            << " buildSet=" << buildTxSetHash
            << " entropy=" << liveBuild->suppliedEntropy
            << " exportWitnesses=" << liveBuild->suppliedExportWitnesses;
    }

    // We want to put transactions in an unpredictable but deterministic order.
    // ConsensusEntropy extends the sanitized live-build salt with the selected
    // ledger entropy; when disabled this remains that sanitized set hash.
    //
    // FIXME: Use a std::vector and a custom sorter instead of CanonicalTXSet?
    //@@start txn-ordering-salt-build-inputs
    auto const buildSeq = prevLedger.seq() + 1;
    CanonicalTXSet retriableTxs{ce().txnOrderingSalt(buildTxSetHash, buildSeq)};

    JLOG(j_.debug()) << "Building canonical tx set: " << retriableTxs.key();

    for (auto const& item : *buildTxs.map_)
    {
        try
        {
            retriableTxs.insert(
                std::make_shared<STTx const>(SerialIter{item.slice()}));
            JLOG(j_.debug()) << "    Tx: " << item.key();
        }
        catch (std::exception const& ex)
        {
            failed.insert(item.key());
            JLOG(j_.warn())
                << "    Tx: " << item.key() << " throws: " << ex.what();
        }
    }
    //@@end txn-ordering-salt-build-inputs

    //@@start auxiliary-pre-build-injection
    // Inject extension pseudo-transactions only for a live build. Entropy and
    // Export witness injection are independently gated inside onPreBuild;
    // export-only rounds still need this hook even when RNG is off.
    //@@start accept-time-cleanup-disabled
    if (replayData)
    {
        ce().onReplayBuild();
    }
    else if (ce().rngEnabled() || ce().exportEnabled())
    {
        ce().onPreBuild(retriableTxs, buildSeq, buildTxSetHash);
    }
    else
    {
        ce().clearRngState();
    }
    //@@end accept-time-cleanup-disabled

    //@@start buildlcl-after-extension-state
    auto built = buildLCL(
        prevLedger,
        retriableTxs,
        std::move(replayData),
        consensusCloseTime,
        closeTimeCorrect,
        closeResolution,
        result.roundTime.read(),
        failed);
    //@@end buildlcl-after-extension-state
    //@@end auxiliary-pre-build-injection

    auto const newLCLHash = built.id();
    JLOG(j_.debug()) << "Built ledger #" << built.seq() << ": " << newLCLHash;

    // Tell directly connected peers that we have a new LCL
    notify(protocol::neACCEPTED_LEDGER, built, haveCorrectLCL);

    // As long as we're in sync with the network, attempt to detect attempts
    // at censorship of transaction by tracking which ones don't make it in
    // after a period of time.
    if (haveCorrectLCL && result.state == ConsensusState::Yes)
    {
        std::vector<TxID> accepted;

        result.txns.map_->visitLeaves(
            [&accepted](boost::intrusive_ptr<SHAMapItem const> const& item) {
                accepted.push_back(item->key());
            });

        // Track all the transactions which failed or were marked as retriable
        for (auto const& r : retriableTxs)
            failed.insert(r.first.getTXID());

        censorshipDetector_.check(
            std::move(accepted),
            [curr = built.seq(),
             j = app_.journal("CensorshipDetector"),
             &failed](uint256 const& id, LedgerIndex seq) {
                if (failed.count(id))
                    return true;

                auto const wait = curr - seq;

                if (wait && (wait % censorshipWarnInternal == 0))
                {
                    std::ostringstream ss;
                    ss << "Potential Censorship: Eligible tx " << id
                       << ", which we are tracking since ledger " << seq
                       << " has not been included as of ledger " << curr << ".";

                    JLOG(j.warn()) << ss.str();
                }

                return false;
            });
    }

    if (validating_)
        validating_ = ledgerMaster_.isCompatible(
            *built.ledger_, j_.warn(), "Not validating");

    if (validating_ && !consensusFail &&
        app_.getValidations().canValidateSeq(built.seq()))
    {
        validate(built, result.txns, proposing);
        JLOG(j_.info()) << "CNF Val " << newLCLHash;
    }
    else
        JLOG(j_.info()) << "CNF buildLCL " << newLCLHash;

    // See if we can accept a ledger as fully-validated
    ledgerMaster_.consensusBuilt(
        built.ledger_, result.txns.id(), std::move(consensusJson));

    //-------------------------------------------------------------------------
    {
        // Apply disputed transactions that didn't get in
        //
        // The first crack of transactions to get into the new
        // open ledger goes to transactions proposed by a validator
        // we trust but not included in the consensus set.
        //
        // These are done first because they are the most likely
        // to receive agreement during consensus. They are also
        // ordered logically "sooner" than transactions not mentioned
        // in the previous consensus round.
        //
        bool anyDisputes = false;
        for (auto const& [_, dispute] : result.disputes)
        {
            (void)_;
            if (!dispute.getOurVote())
            {
                // we voted NO
                try
                {
                    JLOG(j_.debug())
                        << "Test applying disputed transaction that did"
                        << " not get in " << dispute.tx().id();

                    SerialIter sit(dispute.tx().tx_->slice());
                    auto txn = std::make_shared<STTx const>(sit);

                    // Disputed pseudo-transactions that were not accepted
                    // can't be successfully applied in the next ledger
                    if (isPseudoTx(*txn))
                        continue;

                    retriableTxs.insert(txn);

                    anyDisputes = true;
                }
                catch (std::exception const& ex)
                {
                    JLOG(j_.debug()) << "Failed to apply transaction we voted "
                                        "NO on. Exception: "
                                     << ex.what();
                }
            }
        }

        // Build new open ledger
        std::unique_lock lock{app_.getMasterMutex(), std::defer_lock};
        std::unique_lock sl{ledgerMaster_.peekMutex(), std::defer_lock};
        std::lock(lock, sl);

        auto const lastVal = ledgerMaster_.getValidatedLedger();
        std::optional<Rules> rules;
        if (lastVal)
            rules = makeRulesGivenLedger(*lastVal, app_.config().features);
        else
            rules.emplace(app_.config().features);
        app_.openLedger().accept(
            app_,
            *rules,
            built.ledger_,
            localTxs_.getTxSet(),
            anyDisputes,
            retriableTxs,
            tapNONE,
            "consensus",
            [&](OpenView& view, beast::Journal j) {
                return app_.getTxQ().accept(app_, view);
            });

        // Signal a potential fee change to subscribers after the open ledger
        // is created
        app_.getOPs().reportFeeChange();
    }

    //-------------------------------------------------------------------------
    {
        ledgerMaster_.switchLCL(built.ledger_);

        // Do these need to exist?
        XRPL_ASSERT(
            ledgerMaster_.getClosedLedger()->info().hash == built.id(),
            "ripple::RCLConsensus::Adaptor::doAccept : ledger hash match");
        XRPL_ASSERT(
            app_.openLedger().current()->info().parentHash == built.id(),
            "ripple::RCLConsensus::Adaptor::doAccept : parent hash match");
    }

    //-------------------------------------------------------------------------
    // we entered the round with the network,
    // see how close our close time is to other node's
    //  close time reports, and update our clock.
    if ((mode == ConsensusMode::proposing ||
         mode == ConsensusMode::observing) &&
        !consensusFail)
    {
        auto closeTime = rawCloseTimes.self;

        JLOG(j_.info()) << "We closed at "
                        << closeTime.time_since_epoch().count();
        using usec64_t = std::chrono::duration<std::uint64_t>;
        usec64_t closeTotal =
            std::chrono::duration_cast<usec64_t>(closeTime.time_since_epoch());
        int closeCount = 1;

        for (auto const& [t, v] : rawCloseTimes.peers)
        {
            JLOG(j_.info()) << std::to_string(v) << " time votes for "
                            << std::to_string(t.time_since_epoch().count());
            closeCount += v;
            closeTotal +=
                std::chrono::duration_cast<usec64_t>(t.time_since_epoch()) * v;
        }

        closeTotal += usec64_t(closeCount / 2);  // for round to nearest
        closeTotal /= closeCount;

        // Use signed times since we are subtracting
        using duration = std::chrono::duration<std::int32_t>;
        using time_point = std::chrono::time_point<NetClock, duration>;
        auto offset = time_point{closeTotal} -
            std::chrono::time_point_cast<duration>(closeTime);
        JLOG(j_.info()) << "Our close offset is estimated at " << offset.count()
                        << " (" << closeCount << ")";

        app_.timeKeeper().adjustCloseTime(offset);
    }
}

void
RCLConsensus::Adaptor::notify(
    protocol::NodeEvent ne,
    RCLCxLedger const& ledger,
    bool haveCorrectLCL)
{
    protocol::TMStatusChange s;

    if (!haveCorrectLCL)
        s.set_newevent(protocol::neLOST_SYNC);
    else
        s.set_newevent(ne);

    s.set_ledgerseq(ledger.seq());
    s.set_networktime(app_.timeKeeper().now().time_since_epoch().count());
    s.set_ledgerhashprevious(
        ledger.parentID().begin(),
        std::decay_t<decltype(ledger.parentID())>::bytes);
    s.set_ledgerhash(
        ledger.id().begin(), std::decay_t<decltype(ledger.id())>::bytes);

    std::uint32_t uMin, uMax;
    if (!ledgerMaster_.getFullValidatedRange(uMin, uMax))
    {
        uMin = 0;
        uMax = 0;
    }
    else
    {
        // Don't advertise ledgers we're not willing to serve
        uMin = std::max(uMin, ledgerMaster_.getEarliestFetch());
    }
    s.set_firstseq(uMin);
    s.set_lastseq(uMax);
    app_.overlay().foreach(
        send_always(std::make_shared<Message>(s, protocol::mtSTATUS_CHANGE)));
    JLOG(j_.trace()) << "send status change to peer";
}

RCLCxLedger
RCLConsensus::Adaptor::buildLCL(
    RCLCxLedger const& previousLedger,
    CanonicalTXSet& retriableTxs,
    std::unique_ptr<LedgerReplay> replayData,
    NetClock::time_point closeTime,
    bool closeTimeCorrect,
    NetClock::duration closeResolution,
    std::chrono::milliseconds roundTime,
    std::set<TxID>& failedTxs)
{
    std::shared_ptr<Ledger> built = [&]() {
        if (replayData)
        {
            XRPL_ASSERT(
                replayData->parent()->info().hash == previousLedger.id(),
                "ripple::RCLConsensus::Adaptor::buildLCL : parent hash match");
            auto built = buildLedger(*replayData, tapNONE, app_, j_);
            auto const expectedHash = replayData->replay()->info().hash;
            if (!built || built->info().hash != expectedHash)
            {
                JLOG(j_.error()) << "Replay build produced wrong ledger"
                                 << " expected=" << expectedHash << " actual="
                                 << (built ? to_string(built->info().hash)
                                           : std::string{"none"});
                Throw<std::runtime_error>("Cannot replay ledger");
            }
            return built;
        }
        return buildLedger(
            previousLedger.ledger_,
            closeTime,
            closeTimeCorrect,
            closeResolution,
            app_,
            retriableTxs,
            failedTxs,
            j_);
    }();

    // Update fee computations based on accepted txs
    using namespace std::chrono_literals;
    app_.getTxQ().processClosedLedger(app_, *built, roundTime > 5s);

    // And stash the ledger in the ledger master
    if (ledgerMaster_.storeLedger(built))
        JLOG(j_.debug()) << "Consensus built ledger we already had";
    else if (app_.getInboundLedgers().find(built->info().hash))
        JLOG(j_.debug()) << "Consensus built ledger we were acquiring";
    else
        JLOG(j_.debug()) << "Consensus built new ledger";
    return RCLCxLedger{std::move(built)};
}

void
RCLConsensus::Adaptor::validate(
    RCLCxLedger const& ledger,
    RCLTxSet const& txns,
    bool proposing)
{
    if (!validatorKeys_.keys)
    {
        // preStartRound normally prevents this path. Keep validate() itself
        // fail-closed so future call sites cannot dereference an observer key.
        JLOG(j_.warn()) << "Skipping validation without validator keys";
        return;
    }

    using namespace std::chrono_literals;

    auto validationTime = app_.timeKeeper().closeTime();
    if (validationTime <= lastValidationTime_)
        validationTime = lastValidationTime_ + 1s;
    lastValidationTime_ = validationTime;

    auto v = std::make_shared<STValidation>(
        lastValidationTime_,
        validatorKeys_.keys->publicKey,
        validatorKeys_.keys->secretKey,
        validatorKeys_.nodeID,
        [&](STValidation& v) {
            v.setFieldH256(sfLedgerHash, ledger.id());
            v.setFieldH256(sfConsensusHash, txns.id());

            v.setFieldU32(sfLedgerSequence, ledger.seq());

            if (proposing)
                v.setFlag(vfFullValidation);

            if (ledger.ledger_->rules().enabled(featureHardenedValidations))
            {
                // Attest to the hash of what we consider to be the last fully
                // validated ledger. This may be the hash of the ledger we are
                // validating here, and that's fine.
                if (auto const vl = ledgerMaster_.getValidatedLedger())
                    v.setFieldH256(sfValidatedHash, vl->info().hash);

                v.setFieldU64(sfCookie, valCookie_);

                // Report our server version every flag ledger:
                if (ledger.ledger_->isVotingLedger())
                    v.setFieldU64(
                        sfServerVersion, BuildInfo::getEncodedVersion());
            }

            // Report our load
            {
                auto const& ft = app_.getFeeTrack();
                auto const fee = std::max(ft.getLocalFee(), ft.getClusterFee());
                if (fee > ft.getLoadBase())
                    v.setFieldU32(sfLoadFee, fee);
            }

            // If the next ledger is a flag ledger, suggest fee changes and
            // new features:
            if (ledger.ledger_->isVotingLedger())
            {
                // Fees:
                feeVote_->doValidation(
                    ledger.ledger_->fees(), ledger.ledger_->rules(), v);

                // Amendments
                // FIXME: pass `v` and have the function insert the array
                // directly?
                auto const amendments = app_.getAmendmentTable().doValidation(
                    getEnabledAmendments(*ledger.ledger_));

                if (!amendments.empty())
                    v.setFieldV256(
                        sfAmendments, STVector256(sfAmendments, amendments));
            }
        });

    auto const serialized = v->getSerialized();

    // suppress it if we receive it
    app_.getHashRouter().addSuppression(sha512Half(makeSlice(serialized)));

    handleNewValidation(app_, v, "local");

    // Broadcast validation to all peers.
    protocol::TMValidation val;
    val.set_validation(serialized.data(), serialized.size());
    app_.overlay().broadcast(val);

    // Publish to all our subscribers:
    app_.getOPs().pubValidation(v);
}

void
RCLConsensus::Adaptor::onModeChange(ConsensusMode before, ConsensusMode after)
{
    JLOG(j_.info()) << "Consensus mode change before=" << to_string(before)
                    << ", after=" << to_string(after);

    // If we were proposing but aren't any longer, we need to reset the
    // censorship tracking to avoid bogus warnings.
    if ((before == ConsensusMode::proposing ||
         before == ConsensusMode::observing) &&
        before != after)
        censorshipDetector_.reset();

    mode_ = after;
    ce().setMode(after);
}

ConsensusPhase
RCLConsensus::phase() const
{
    std::lock_guard _{mutex_};
    return consensus_->phase();
}

bool
RCLConsensus::extensionsBusy() const
{
    // ConsensusExtensions state is mutated by timer, peer-proposal and
    // local sidecar snapshot paths under this mutex. Busy polling observes
    // the same state, so it must share the same synchronization boundary.
    std::lock_guard _{mutex_};
    return consensus_->extensionsBusy();
}

RCLCxLedger::ID
RCLConsensus::prevLedgerID() const
{
    std::lock_guard _{mutex_};
    return consensus_->prevLedgerID();
}

Json::Value
RCLConsensus::getJson(bool full) const
{
    Json::Value ret;
    {
        std::lock_guard _{mutex_};
        ret = consensus_->getJson(full);
    }
    ret["validating"] = adaptor_.validating();
    return ret;
}

void
RCLConsensus::timerEntry(
    NetClock::time_point const& now,
    std::unique_ptr<std::stringstream> const& clog)
{
    try
    {
        std::lock_guard _{mutex_};
        consensus_->timerEntry(now, clog);
    }
    catch (SHAMapMissingNode const& mn)
    {
        // This should never happen
        std::stringstream ss;
        ss << "During consensus timerEntry: " << mn.what();
        JLOG(j_.error()) << ss.str();
        CLOG(clog) << ss.str();
        Rethrow();
    }
}

void
RCLConsensus::gotTxSet(NetClock::time_point const& now, RCLTxSet const& txSet)
{
    try
    {
        std::lock_guard _{mutex_};
        consensus_->gotTxSet(now, txSet);
    }
    catch (SHAMapMissingNode const& mn)
    {
        // This should never happen
        JLOG(j_.error()) << "During consensus gotTxSet: " << mn.what();
        Rethrow();
    }
}

//! @see Consensus::simulate

void
RCLConsensus::simulate(
    NetClock::time_point const& now,
    std::optional<std::chrono::milliseconds> consensusDelay)
{
    std::lock_guard _{mutex_};
    consensus_->simulate(now, consensusDelay);
}

bool
RCLConsensus::peerProposal(
    NetClock::time_point const& now,
    RCLCxPeerPos const& newProposal)
{
    std::lock_guard _{mutex_};
    return consensus_->peerProposal(now, newProposal);
}

//@@start pre-start-round
bool
RCLConsensus::Adaptor::preStartRound(
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowTrusted)
{
    //@@start pre-start-round-extension-latches
    ce().setRngEnabledThisRound(
        prevLgr.ledger_->rules().enabled(featureConsensusEntropy));
    ce().setExportEnabledThisRound(
        prevLgr.ledger_->rules().enabled(featureExport));
    //@@end pre-start-round-extension-latches

    JLOG(j_.trace()) << "RNGGATE: preStartRound"
                     << " prevSeq=" << prevLgr.seq()
                     << " buildSeq=" << (prevLgr.seq() + 1)
                     << " rngEnabled=" << (ce().rngEnabled() ? "yes" : "no")
                     << " exportEnabled="
                     << (ce().exportEnabled() ? "yes" : "no");

    // We have a key, we do not want out of sync validations after a restart
    // and are not amendment blocked.
    validating_ = validatorKeys_.keys &&
        prevLgr.seq() >= app_.getMaxDisallowedLedger() &&
        !app_.getOPs().isBlocked();

    // If we are not running in standalone mode and there's a configured UNL,
    // check to make sure that it's not expired.
    if (validating_ && !app_.config().standalone() && app_.validators().count())
    {
        auto const when = app_.validators().expires();

        if (!when || *when < app_.timeKeeper().now())
        {
            JLOG(j_.error()) << "Voluntarily bowing out of consensus process "
                                "because of an expired validator list.";
            validating_ = false;
        }
    }

    const bool synced = app_.getOPs().getOperatingMode() == OperatingMode::FULL;

    if (validating_)
    {
        JLOG(j_.info()) << "Entering consensus process, validating, synced="
                        << (synced ? "yes" : "no");
    }
    else
    {
        // Otherwise we just want to monitor the validation process.
        JLOG(j_.info()) << "Entering consensus process, watching, synced="
                        << (synced ? "yes" : "no");
    }

    // Notify inbound ledgers that we are starting a new round
    inboundTransactions_.newRound(prevLgr.seq());

    // Notify NegativeUNLVote that new validators are added
    if (prevLgr.ledger_->rules().enabled(featureNegativeUNL) &&
        !nowTrusted.empty())
        nUnlVote_.newValidators(prevLgr.seq() + 1, nowTrusted);

    bool const proposing = validating_ && synced;

    JLOG(j_.info()) << "STARTDIAG: preStartRound"
                    << " mode=" << app_.getOPs().strOperatingMode()
                    << " synced=" << (synced ? "yes" : "no")
                    << " validating=" << (validating_ ? "yes" : "no")
                    << " proposing=" << (proposing ? "yes" : "no")
                    << " seq=" << (prevLgr.seq() + 1);

    // propose only if we're in sync with the network (and validating)
    return proposing;
}
//@@end pre-start-round

bool
RCLConsensus::Adaptor::haveValidated() const
{
    return ledgerMaster_.haveValidated();
}

LedgerIndex
RCLConsensus::Adaptor::getValidLedgerIndex() const
{
    return ledgerMaster_.getValidLedgerIndex();
}

std::pair<std::size_t, hash_set<RCLConsensus::Adaptor::NodeKey_t>>
RCLConsensus::Adaptor::getQuorumKeys() const
{
    return app_.validators().getQuorumKeys();
}

std::size_t
RCLConsensus::Adaptor::laggards(
    Ledger_t::Seq const seq,
    hash_set<RCLConsensus::Adaptor::NodeKey_t>& trustedKeys) const
{
    return app_.getValidations().laggards(seq, trustedKeys);
}

bool
RCLConsensus::Adaptor::validator() const
{
    return validatorKeys_.keys.has_value();
}

void
RCLConsensus::Adaptor::updateOperatingMode(std::size_t const positions) const
{
    if (!positions && app_.getOPs().isFull())
    {
        JLOG(j_.warn()) << "STARTDIAG: updateOperatingMode demoting"
                        << " from=FULL"
                        << " to=CONNECTED"
                        << " positions=" << positions;
        app_.getOPs().setMode(OperatingMode::CONNECTED);
    }
}

void
RCLConsensus::startRound(
    NetClock::time_point const& now,
    RCLCxLedger::ID const& prevLgrId,
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowUntrusted,
    hash_set<NodeID> const& nowTrusted,
    std::unique_ptr<std::stringstream> const& clog)
{
    std::lock_guard _{mutex_};
    consensus_->startRound(
        now,
        prevLgrId,
        prevLgr,
        nowUntrusted,
        adaptor_.preStartRound(prevLgr, nowTrusted),
        clog);
}

RclConsensusLogger::RclConsensusLogger(
    const char* label,
    const bool validating,
    beast::Journal j)
    : j_(j)
{
    if (!validating && !j.info())
        return;
    start_ = std::chrono::steady_clock::now();
    ss_ = std::make_unique<std::stringstream>();
    header_ = "ConsensusLogger ";
    header_ += label;
    header_ += ": ";
}

RclConsensusLogger::~RclConsensusLogger()
{
    if (!ss_)
        return;
    auto const duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_);
    ss_->seekg(0, std::ios::beg);

    std::string line;
    while (std::getline(*ss_, line, '.'))
    {
        boost::algorithm::trim(line);
        if (!line.empty())
        {
            JLOG(j_.debug()) << header_ << line << ".";
        }
    }
    JLOG(j_.debug()) << header_ << "Total duration: " << duration.count()
                     << "ms.";
}
}  // namespace ripple
