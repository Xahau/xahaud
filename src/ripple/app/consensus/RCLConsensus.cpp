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

#include <ripple/app/consensus/RCLConsensus.h>
#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/ledger/BuildLedger.h>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/InboundTransactions.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LocalTxs.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/CanonicalTXSet.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/LoadFeeTrack.h>
#include <ripple/app/misc/NegativeUNLVote.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/misc/ValidatorKeys.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/basics/random.h>
#include <ripple/beast/core/LexicalCast.h>
#include <ripple/consensus/LedgerTiming.h>
#include <ripple/crypto/csprng.h>
#include <ripple/nodestore/DatabaseShard.h>
#include <ripple/overlay/Overlay.h>
#include <ripple/overlay/predicates.h>
#include <ripple/protocol/BuildInfo.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/TxFormats.h>
#include <ripple/protocol/digest.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace ripple {

RCLConsensus::RCLConsensus(
    Application& app,
    std::unique_ptr<FeeVote>&& feeVote,
    LedgerMaster& ledgerMaster,
    LocalTxs& localTxs,
    InboundTransactions& inboundTransactions,
    Consensus<Adaptor>::clock_type const& clock,
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
    , consensus_(clock, adaptor_, journal)
    , j_(journal)
{
}

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
    assert(valCookie_ != 0);

    JLOG(j_.info()) << "Consensus engine started (cookie: " +
            std::to_string(valCookie_) + ")";

    if (validatorKeys_.nodeID != beast::zero)
    {
        std::stringstream ss;

        JLOG(j_.info()) << "Validator identity: "
                        << toBase58(
                               TokenType::NodePublic,
                               validatorKeys_.masterPublicKey);

        if (validatorKeys_.masterPublicKey != validatorKeys_.publicKey)
        {
            JLOG(j_.debug())
                << "Validator ephemeral signing key: "
                << toBase58(TokenType::NodePublic, validatorKeys_.publicKey)
                << " (seq: " << std::to_string(validatorKeys_.sequence) << ")";
        }
    }
}

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

    assert(!built->open() && built->isImmutable());
    assert(built->info().hash == hash);

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

    // Serialize full ExtendedPosition (includes RNG leaves)
    Serializer positionData;
    proposal.position().add(positionData);
    auto const posSlice = positionData.slice();
    prop.set_currenttxhash(posSlice.data(), posSlice.size());

    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.prevLedger().size());

    auto const pk = peerPos.publicKey().slice();
    prop.set_nodepubkey(pk.data(), pk.size());

    auto const sig = peerPos.signature();
    prop.set_signature(sig.data(), sig.size());

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
    JLOG(j_.trace()) << (proposal.isBowOut() ? "We bow out: " : "We propose: ")
                     << ripple::to_string(proposal.prevLedger()) << " -> "
                     << ripple::to_string(proposal.position());

    protocol::TMProposeSet prop;

    // Serialize full ExtendedPosition (includes RNG leaves)
    Serializer positionData;
    proposal.position().add(positionData);
    auto const posSlice = positionData.slice();
    prop.set_currenttxhash(posSlice.data(), posSlice.size());

    JLOG(j_.info()) << "RNG: propose seq=" << proposal.proposeSeq()
                    << " wireBytes=" << posSlice.size() << " commit="
                    << (proposal.position().myCommitment ? "yes" : "no")
                    << " reveal="
                    << (proposal.position().myReveal ? "yes" : "no");

    // Self-seed our own reveal so we count toward reveal quorum
    // (harvestRngData only sees peer proposals, not our own).
    if (proposal.position().myReveal)
    {
        auto const ownNodeId = validatorKeys_.nodeID;
        pendingReveals_[ownNodeId] = *proposal.position().myReveal;
        nodeIdToKey_[ownNodeId] = validatorKeys_.publicKey;
        JLOG(j_.debug()) << "RNG: self-seeded reveal for " << ownNodeId;
    }

    prop.set_previousledger(
        proposal.prevLedger().begin(), proposal.prevLedger().size());
    prop.set_proposeseq(proposal.proposeSeq());
    prop.set_closetime(proposal.closeTime().time_since_epoch().count());
    prop.set_nodepubkey(
        validatorKeys_.publicKey.data(), validatorKeys_.publicKey.size());

    auto sig = signDigest(
        validatorKeys_.publicKey,
        validatorKeys_.secretKey,
        proposal.signingHash());

    prop.set_signature(sig.data(), sig.size());

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
            proof.signature = Buffer(sig.data(), sig.size());
            return proof;
        };

        if (proposal.position().myCommitment && proposal.proposeSeq() == 0)
            commitProofs_.emplace(validatorKeys_.nodeID, makeProof());

        if (proposal.position().myReveal)
            proposalProofs_[validatorKeys_.nodeID] = makeProof();
    }

    auto const suppression = proposalUniqueId(
        proposal.position(),
        proposal.prevLedger(),
        proposal.proposeSeq(),
        proposal.closeTime(),
        validatorKeys_.publicKey,
        sig);

    app_.getHashRouter().addSuppression(suppression);

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
                    prevLedger, validations, initialSet);
            }
        }
        else if (
            prevLedger->isVotingLedger() &&
            prevLedger->rules().enabled(featureNegativeUNL))
        {
            // previous ledger was a voting ledger,
            // so the current consensus session is for a flag ledger,
            // add negative UNL pseudo-transactions
            nUnlVote_.doVoting(
                prevLedger,
                app_.validators().getTrustedMasterKeys(),
                app_.getValidations(),
                initialSet);
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

    // Bootstrap commit-reveal: generate entropy and include commitment
    // in our very first proposal so peers can collect it during consensus.
    //
    // This is gated on `proposing` — a node that just restarted enters
    // as proposing=false (observing) and must watch at least one full
    // round before consensus promotes it to proposing.  During those
    // observation rounds it cannot contribute to the RNG pipeline at
    // all: no commitment, no reveal, no SHAMap entries.  The surviving
    // proposers will close those rounds with fewer commits (possibly
    // falling back to ZERO entropy) until the rejoiner starts proposing.
    if (proposing && prevLedger->rules().enabled(featureConsensusEntropy))
    {
        cacheUNLReport();
        generateEntropySecret();
        pos.myCommitment = sha512Half(
            myEntropySecret_,
            validatorKeys_.publicKey,
            prevLedger->info().seq + 1);

        // Seed our own commitment into pendingCommits_ so we count
        // toward quorum (harvestRngData only sees peer proposals).
        auto const ownNodeId = validatorKeys_.nodeID;
        pendingCommits_[ownNodeId] = *pos.myCommitment;
        nodeIdToKey_[ownNodeId] = validatorKeys_.publicKey;

        JLOG(j_.info()) << "RNG: onClose bootstrap seq="
                        << (prevLedger->info().seq + 1)
                        << " commitment=" << *pos.myCommitment;
    }
    else
    {
        JLOG(j_.debug()) << "RNG: onClose skipped (proposing=" << proposing
                         << " amendment="
                         << prevLedger->rules().enabled(featureConsensusEntropy)
                         << ")";
    }

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
    Json::Value&& consensusJson)
{
    app_.getJobQueue().addJob(
        jtACCEPT,
        "acceptLedger",
        [=, this, cj = std::move(consensusJson)]() mutable {
            // Note that no lock is held or acquired during this job.
            // This is because generic Consensus guarantees that once a ledger
            // is accepted, the consensus results and capture by reference state
            // will not change until startRound is called (which happens via
            // endConsensus).
            this->doAccept(
                result,
                prevLedger,
                closeResolution,
                rawCloseTimes,
                mode,
                std::move(cj));
            this->app_.getOPs().endConsensus();
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

    // We want to put transactions in an unpredictable but deterministic order:
    // we use the hash of the set.
    //
    // FIXME: Use a std::vector and a custom sorter instead of CanonicalTXSet?
    CanonicalTXSet retriableTxs{result.txns.map_->getHash().as_uint256()};

    JLOG(j_.debug()) << "Building canonical tx set: " << retriableTxs.key();

    for (auto const& item : *result.txns.map_)
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

    // Inject consensus entropy pseudo-transaction (if amendment enabled)
    // This must happen before buildLCL so the entropy tx is in the ledger
    if (prevLedger.ledger_->rules().enabled(featureConsensusEntropy))
        injectEntropyPseudoTx(retriableTxs, prevLedger.seq() + 1);
    else
        clearRngState();

    auto built = buildLCL(
        prevLedger,
        retriableTxs,
        consensusCloseTime,
        closeTimeCorrect,
        closeResolution,
        result.roundTime.read(),
        failed);

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
        assert(ledgerMaster_.getClosedLedger()->info().hash == built.id());
        assert(app_.openLedger().current()->info().parentHash == built.id());
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
    NetClock::time_point closeTime,
    bool closeTimeCorrect,
    NetClock::duration closeResolution,
    std::chrono::milliseconds roundTime,
    std::set<TxID>& failedTxs)
{
    std::shared_ptr<Ledger> built = [&]() {
        if (auto const replayData = ledgerMaster_.releaseReplay())
        {
            assert(replayData->parent()->info().hash == previousLedger.id());
            return buildLedger(*replayData, tapNONE, app_, j_);
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
    using namespace std::chrono_literals;

    auto validationTime = app_.timeKeeper().closeTime();
    if (validationTime <= lastValidationTime_)
        validationTime = lastValidationTime_ + 1s;
    lastValidationTime_ = validationTime;

    auto v = std::make_shared<STValidation>(
        lastValidationTime_,
        validatorKeys_.publicKey,
        validatorKeys_.secretKey,
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

    // Broadcast to all our peers:
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
}

Json::Value
RCLConsensus::getJson(bool full) const
{
    Json::Value ret;
    {
        std::lock_guard _{mutex_};
        ret = consensus_.getJson(full);
    }
    ret["validating"] = adaptor_.validating();
    return ret;
}

void
RCLConsensus::timerEntry(NetClock::time_point const& now)
{
    try
    {
        std::lock_guard _{mutex_};
        consensus_.timerEntry(now);
    }
    catch (SHAMapMissingNode const& mn)
    {
        // This should never happen
        JLOG(j_.error()) << "During consensus timerEntry: " << mn.what();
        Rethrow();
    }
}

void
RCLConsensus::gotTxSet(NetClock::time_point const& now, RCLTxSet const& txSet)
{
    try
    {
        std::lock_guard _{mutex_};
        consensus_.gotTxSet(now, txSet);
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
    consensus_.simulate(now, consensusDelay);
}

bool
RCLConsensus::peerProposal(
    NetClock::time_point const& now,
    RCLCxPeerPos const& newProposal)
{
    std::lock_guard _{mutex_};
    return consensus_.peerProposal(now, newProposal);
}

bool
RCLConsensus::Adaptor::preStartRound(
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowTrusted)
{
    // We have a key, we do not want out of sync validations after a restart
    // and are not amendment blocked.
    validating_ = validatorKeys_.publicKey.size() != 0 &&
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

    // propose only if we're in sync with the network (and validating)
    return validating_ && synced;
}

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
    return !validatorKeys_.publicKey.empty();
}

void
RCLConsensus::Adaptor::updateOperatingMode(std::size_t const positions) const
{
    if (!positions && app_.getOPs().isFull())
        app_.getOPs().setMode(OperatingMode::CONNECTED);
}

//------------------------------------------------------------------------------
// RNG Helper Methods

std::size_t
RCLConsensus::Adaptor::quorumThreshold() const
{
    // Prefer expected proposers (recent proposers ∩ UNL) — this
    // adapts to actual network conditions rather than relying on
    // the potentially stale UNL Report.  Falls back to full
    // UNL Report for cold boot (first round).
    //
    // Round 1: threshold based on full UNL (conservative)
    // Round 2+: threshold based on who actually proposed last round
    auto const base = expectedProposers_.empty() ? unlReportNodeIds_.size()
                                                 : expectedProposers_.size();
    if (base == 0)
        return 1;  // safety: need at least one commit
    return (base * 80 + 99) / 100;
}

void
RCLConsensus::Adaptor::setExpectedProposers(hash_set<NodeID> proposers)
{
    if (!proposers.empty())
    {
        // Intersect with active UNL — only expect commits from
        // validators we trust.  Non-UNL proposers are ignored.
        hash_set<NodeID> filtered;
        for (auto const& id : proposers)
        {
            if (unlReportNodeIds_.count(id))
                filtered.insert(id);
        }
        filtered.insert(validatorKeys_.nodeID);
        expectedProposers_ = std::move(filtered);
        JLOG(j_.debug()) << "RNG: expectedProposers from recent proposers: "
                         << expectedProposers_.size() << " (filtered from "
                         << proposers.size() << ")";
        return;
    }

    // First round (no previous proposers): fall back to UNL Report.
    // cacheUNLReport() was called just before this, so it's populated.
    if (!unlReportNodeIds_.empty())
    {
        expectedProposers_ = unlReportNodeIds_;
        JLOG(j_.debug()) << "RNG: expectedProposers from UNL Report: "
                         << expectedProposers_.size();
        return;
    }

    // No data at all (shouldn't happen — cacheUNLReport falls back to
    // trusted keys).  Leave empty → hasQuorumOfCommits uses 80% fallback.
    JLOG(j_.warn()) << "RNG: no expectedProposers available";
}

std::size_t
RCLConsensus::Adaptor::pendingCommitCount() const
{
    return pendingCommits_.size();
}

bool
RCLConsensus::Adaptor::hasQuorumOfCommits() const
{
    if (!expectedProposers_.empty())
    {
        // Wait for commits from all expected proposers.
        // rngPIPELINE_TIMEOUT is the safety valve for dead nodes.
        for (auto const& id : expectedProposers_)
        {
            if (pendingCommits_.find(id) == pendingCommits_.end())
            {
                JLOG(j_.debug())
                    << "RNG: hasQuorumOfCommits? " << pendingCommits_.size()
                    << "/" << expectedProposers_.size() << " -> no";
                return false;
            }
        }
        JLOG(j_.debug()) << "RNG: hasQuorumOfCommits? "
                         << pendingCommits_.size() << "/"
                         << expectedProposers_.size()
                         << " -> YES (all expected)";
        return true;
    }

    // Fallback: 80% of active UNL (cold boot, no expected set)
    auto threshold = quorumThreshold();
    bool result = pendingCommits_.size() >= threshold;
    JLOG(j_.debug()) << "RNG: hasQuorumOfCommits? " << pendingCommits_.size()
                     << "/" << threshold << " -> " << (result ? "YES" : "no")
                     << " (80% fallback)";
    return result;
}

bool
RCLConsensus::Adaptor::hasMinimumReveals() const
{
    // Wait for reveals from ALL committers, not just 80%.  The commit
    // set is deterministic (SHAMap agreed), so we know exactly which
    // validators should reveal.  Waiting for all of them ensures every
    // node builds the same entropy set.  rngPIPELINE_TIMEOUT in
    // Consensus.h is the safety valve for nodes that crash/partition
    // between commit and reveal.
    auto const expected = pendingCommits_.size();
    bool result = pendingReveals_.size() >= expected;
    JLOG(j_.debug()) << "RNG: hasMinimumReveals? " << pendingReveals_.size()
                     << "/" << expected << " -> " << (result ? "YES" : "no");
    return result;
}

bool
RCLConsensus::Adaptor::hasAnyReveals() const
{
    return !pendingReveals_.empty();
}

uint256
RCLConsensus::Adaptor::buildCommitSet(LedgerIndex seq)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    map->setUnbacked();

    for (auto const& [nodeId, commit] : pendingCommits_)
    {
        if (!isUNLReportMember(nodeId))
            continue;

        auto kit = nodeIdToKey_.find(nodeId);
        if (kit == nodeIdToKey_.end())
            continue;

        // Encode the NodeID into sfAccount so handleAcquiredRngSet can
        // recover it without recomputing (master vs signing key issue).
        AccountID acctId;
        std::memcpy(acctId.data(), nodeId.data(), acctId.size());

        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfFlags, tfEntropyCommit);
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, acctId);
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, commit);
            obj.setFieldVL(sfSigningPubKey, kit->second.slice());
            auto proofIt = commitProofs_.find(nodeId);
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
    inboundTransactions_.giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built commitSet SHAMap hash=" << hash
                     << " entries=" << pendingCommits_.size();
    return hash;
}

uint256
RCLConsensus::Adaptor::buildEntropySet(LedgerIndex seq)
{
    auto map =
        std::make_shared<SHAMap>(SHAMapType::TRANSACTION, app_.getNodeFamily());
    map->setUnbacked();

    for (auto const& [nodeId, reveal] : pendingReveals_)
    {
        if (!isUNLReportMember(nodeId))
            continue;

        auto kit = nodeIdToKey_.find(nodeId);
        if (kit == nodeIdToKey_.end())
            continue;

        AccountID acctId;
        std::memcpy(acctId.data(), nodeId.data(), acctId.size());

        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfFlags, tfEntropyReveal);
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, acctId);
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, reveal);
            obj.setFieldVL(sfSigningPubKey, kit->second.slice());
            auto proofIt = proposalProofs_.find(nodeId);
            if (proofIt != proposalProofs_.end())
                obj.setFieldVL(sfBlob, serializeProof(proofIt->second));
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
    inboundTransactions_.giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built entropySet SHAMap hash=" << hash
                     << " entries=" << pendingReveals_.size();
    return hash;
}

void
RCLConsensus::Adaptor::generateEntropySecret()
{
    // Generate cryptographically secure random entropy
    crypto_prng()(myEntropySecret_.data(), myEntropySecret_.size());
    entropyFailed_ = false;
}

uint256
RCLConsensus::Adaptor::getEntropySecret() const
{
    return myEntropySecret_;
}

void
RCLConsensus::Adaptor::setEntropyFailed()
{
    entropyFailed_ = true;
}

PublicKey const&
RCLConsensus::Adaptor::validatorKey() const
{
    return validatorKeys_.publicKey;
}

void
RCLConsensus::Adaptor::clearRngState()
{
    pendingCommits_.clear();
    pendingReveals_.clear();
    nodeIdToKey_.clear();
    myEntropySecret_ = uint256{};
    entropyFailed_ = false;
    commitSetMap_.reset();
    entropySetMap_.reset();
    pendingRngFetches_.clear();
    unlReportNodeIds_.clear();
    expectedProposers_.clear();
    commitProofs_.clear();
    proposalProofs_.clear();
}

void
RCLConsensus::Adaptor::cacheUNLReport()
{
    unlReportNodeIds_.clear();

    // Try UNL Report from the validated ledger
    if (auto const prevLedger = ledgerMaster_.getValidatedLedger())
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

    // Always include ourselves
    unlReportNodeIds_.insert(validatorKeys_.nodeID);

    JLOG(j_.debug()) << "RNG: cacheUNLReport size=" << unlReportNodeIds_.size();
}

bool
RCLConsensus::Adaptor::isUNLReportMember(NodeID const& nodeId) const
{
    return unlReportNodeIds_.count(nodeId) > 0;
}

bool
RCLConsensus::Adaptor::isRngSet(uint256 const& hash) const
{
    if (commitSetMap_ && commitSetMap_->getHash().as_uint256() == hash)
        return true;
    if (entropySetMap_ && entropySetMap_->getHash().as_uint256() == hash)
        return true;
    return pendingRngFetches_.count(hash) > 0;
}

void
RCLConsensus::Adaptor::handleAcquiredRngSet(std::shared_ptr<SHAMap> const& map)
{
    auto const hash = map->getHash().as_uint256();
    pendingRngFetches_.erase(hash);

    JLOG(j_.debug()) << "RNG: handleAcquiredRngSet hash=" << hash;

    // Determine if this is a commitSet or entropySet by inspecting entries
    bool isCommitSet = false;
    bool isEntropySet = false;

    map->visitLeaves([&](boost::intrusive_ptr<SHAMapItem const> const& item) {
        try
        {
            // Skip prefix (4 bytes) when deserializing
            SerialIter sit(item->slice());
            auto stx = std::make_shared<STTx const>(std::ref(sit));
            auto flags = stx->getFieldU32(sfFlags);
            if (flags & tfEntropyCommit)
                isCommitSet = true;
            else if (flags & tfEntropyReveal)
                isEntropySet = true;
        }
        catch (std::exception const&)
        {
            // Skip malformed entries
        }
    });

    if (!isCommitSet && !isEntropySet)
    {
        JLOG(j_.warn()) << "RNG: acquired set " << hash
                        << " has no recognizable RNG entries";
        return;
    }

    // Union-merge: diff against our local set and add any entries we're
    // missing. Unlike normal txSets which use avalanche voting to resolve
    // disagreements, RNG sets use pure union — every valid UNL entry
    // belongs in the set. Differences arise only from propagation timing,
    // not from conflicting opinions about inclusion.
    auto& localMap = isCommitSet ? commitSetMap_ : entropySetMap_;
    auto& pendingData = isCommitSet ? pendingCommits_ : pendingReveals_;

    std::size_t merged = 0;

    if (localMap)
    {
        SHAMap::Delta delta;
        localMap->compare(*map, delta, 65536);

        for (auto const& [key, pair] : delta)
        {
            // pair.first = our entry, pair.second = their entry
            // If we don't have it (pair.first is null), merge it
            if (!pair.first && pair.second)
            {
                try
                {
                    SerialIter sit(pair.second->slice());
                    auto stx = std::make_shared<STTx const>(std::ref(sit));

                    auto pk = stx->getFieldVL(sfSigningPubKey);
                    PublicKey pubKey(makeSlice(pk));
                    auto digest = stx->getFieldH256(sfDigest);

                    // Recover NodeID from sfAccount (encoded by
                    // buildCommitSet/buildEntropySet) to avoid
                    // master-vs-signing key mismatch.
                    auto const acctId = stx->getAccountID(sfAccount);
                    NodeID nodeId;
                    std::memcpy(nodeId.data(), acctId.data(), nodeId.size());

                    if (!isUNLReportMember(nodeId))
                    {
                        JLOG(j_.debug()) << "RNG: rejecting non-UNL entry from "
                                         << nodeId << " in acquired set";
                        continue;
                    }

                    // Verify proposal proof if present
                    if (stx->isFieldPresent(sfBlob))
                    {
                        auto proofBlob = stx->getFieldVL(sfBlob);
                        if (!verifyProof(
                                proofBlob, pubKey, digest, isCommitSet))
                        {
                            JLOG(j_.warn())
                                << "RNG: invalid proof from " << nodeId
                                << " in acquired set (diff)";
                            continue;
                        }
                    }

                    pendingData[nodeId] = digest;
                    nodeIdToKey_[nodeId] = pubKey;
                    ++merged;

                    JLOG(j_.trace())
                        << "RNG: merged " << (isCommitSet ? "commit" : "reveal")
                        << " from " << nodeId;
                }
                catch (std::exception const& ex)
                {
                    JLOG(j_.warn())
                        << "RNG: failed to parse entry from acquired set: "
                        << ex.what();
                }
            }
        }
    }
    else
    {
        // We don't have a local set yet — extract all entries
        map->visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                try
                {
                    SerialIter sit(item->slice());
                    auto stx = std::make_shared<STTx const>(std::ref(sit));

                    auto pk = stx->getFieldVL(sfSigningPubKey);
                    PublicKey pubKey(makeSlice(pk));
                    auto digest = stx->getFieldH256(sfDigest);

                    auto const acctId = stx->getAccountID(sfAccount);
                    NodeID nodeId;
                    std::memcpy(nodeId.data(), acctId.data(), nodeId.size());

                    if (!isUNLReportMember(nodeId))
                    {
                        JLOG(j_.debug()) << "RNG: rejecting non-UNL entry from "
                                         << nodeId << " in acquired set";
                        return;
                    }

                    // Verify proposal proof if present
                    if (stx->isFieldPresent(sfBlob))
                    {
                        auto proofBlob = stx->getFieldVL(sfBlob);
                        if (!verifyProof(
                                proofBlob, pubKey, digest, isCommitSet))
                        {
                            JLOG(j_.warn())
                                << "RNG: invalid proof from " << nodeId
                                << " in acquired set (visit)";
                            return;
                        }
                    }

                    pendingData[nodeId] = digest;
                    nodeIdToKey_[nodeId] = pubKey;
                    ++merged;
                }
                catch (std::exception const&)
                {
                    // Skip malformed entries
                }
            });
    }

    JLOG(j_.info()) << "RNG: merged " << merged << " entries from "
                    << (isCommitSet ? "commitSet" : "entropySet")
                    << " hash=" << hash;
}

void
RCLConsensus::Adaptor::fetchRngSetIfNeeded(std::optional<uint256> const& hash)
{
    if (!hash || *hash == uint256{})
        return;

    // Check if we already have this set
    if (commitSetMap_ && commitSetMap_->getHash().as_uint256() == *hash)
        return;
    if (entropySetMap_ && entropySetMap_->getHash().as_uint256() == *hash)
        return;

    // Check if already fetching
    if (pendingRngFetches_.count(*hash))
        return;

    // Check if InboundTransactions already has it
    if (auto existing = inboundTransactions_.getSet(*hash, false))
    {
        handleAcquiredRngSet(existing);
        return;
    }

    // Trigger network fetch
    JLOG(j_.debug()) << "RNG: triggering fetch for set " << *hash;
    pendingRngFetches_.insert(*hash);
    inboundTransactions_.getSet(*hash, true);
}

void
RCLConsensus::Adaptor::injectEntropyPseudoTx(
    CanonicalTXSet& retriableTxs,
    LedgerIndex seq)
{
    JLOG(j_.info()) << "RNG: injectEntropy seq=" << seq
                    << " commits=" << pendingCommits_.size()
                    << " reveals=" << pendingReveals_.size()
                    << " failed=" << entropyFailed_;

    uint256 finalEntropy;
    bool hasEntropy = false;

    // Calculate entropy from collected reveals
    if (entropyFailed_ || pendingReveals_.empty())
    {
        // Liveness fallback: inject zero entropy.
        // Hooks MUST check for zero to know entropy is unavailable.
        finalEntropy.zero();
        hasEntropy = true;
        JLOG(j_.warn()) << "RNG: Injecting ZERO entropy (fallback) for ledger "
                        << seq;
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

    // Synthesize and inject the pseudo-transaction
    if (hasEntropy)
    {
        // Account Zero convention for pseudo-transactions (same as ttFEE, etc)
        STTx tx(ttCONSENSUS_ENTROPY, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldH256(sfDigest, finalEntropy);
        });

        retriableTxs.insert(std::make_shared<STTx>(std::move(tx)));
    }

    // Reset RNG state for next round
    clearRngState();
}

void
RCLConsensus::Adaptor::harvestRngData(
    NodeID const& nodeId,
    PublicKey const& publicKey,
    ExtendedPosition const& position,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    uint256 const& prevLedger,
    Slice const& signature)
{
    JLOG(j_.debug()) << "RNG: harvestRngData from " << nodeId
                     << " commit=" << (position.myCommitment ? "yes" : "no")
                     << " reveal=" << (position.myReveal ? "yes" : "no");

    // Reject data from validators not in the active UNL
    if (!isUNLReportMember(nodeId))
    {
        JLOG(j_.debug()) << "RNG: rejecting data from non-UNL validator "
                         << nodeId;
        return;
    }

    // Store nodeId -> publicKey mapping for deterministic ordering
    nodeIdToKey_[nodeId] = publicKey;

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
        }
        else if (inserted)
        {
            JLOG(j_.trace()) << "Harvested commitment from " << nodeId << ": "
                             << *position.myCommitment;
        }
    }

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
        auto const prevLgr = ledgerMaster_.getLedgerByHash(prevLedger);
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
RCLConsensus::Adaptor::serializeProof(ProposalProof const& proof)
{
    Serializer s;
    s.add32(proof.proposeSeq);
    s.add32(proof.closeTime);
    s.addBitString(proof.prevLedger);
    s.addVL(proof.positionData.slice());
    s.addVL(Slice(proof.signature.data(), proof.signature.size()));
    return s.getData();
}

bool
RCLConsensus::Adaptor::verifyProof(
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
        auto position =
            ExtendedPosition::fromSerialIter(posIter, positionData.size());

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
RCLConsensus::startRound(
    NetClock::time_point const& now,
    RCLCxLedger::ID const& prevLgrId,
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowUntrusted,
    hash_set<NodeID> const& nowTrusted)
{
    std::lock_guard _{mutex_};
    consensus_.startRound(
        now,
        prevLgrId,
        prevLgr,
        nowUntrusted,
        adaptor_.preStartRound(prevLgr, nowTrusted));
}
}  // namespace ripple
