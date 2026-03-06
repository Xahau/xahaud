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

#include <xrpld/app/consensus/RCLConsensus.h>
#include <xrpld/app/consensus/RCLValidations.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/InboundLedgers.h>
#include <xrpld/app/ledger/InboundTransactions.h>
#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LocalTxs.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/misc/AmendmentTable.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
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
#include <xrpl/protocol/BuildInfo.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <random>

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

    XRPL_ASSERT(
        validatorKeys_.keys,
        "ripple::RCLConsensus::Adaptor::share : validator keys available");

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

    JLOG(j_.debug()) << "RNG: propose seq=" << proposal.proposeSeq()
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
        nodeIdToKey_.insert_or_assign(
            ownNodeId, validatorKeys_.keys->publicKey);
        JLOG(j_.trace()) << "RNG: self-seeded reveal for " << ownNodeId;
    }

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
        validatorKeys_.keys->publicKey,
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
            validatorKeys_.keys->publicKey,
            prevLedger->info().seq + 1);

        // Seed our own commitment into pendingCommits_ so we count
        // toward quorum (harvestRngData only sees peer proposals).
        auto const ownNodeId = validatorKeys_.nodeID;
        pendingCommits_[ownNodeId] = *pos.myCommitment;
        nodeIdToKey_.insert_or_assign(
            ownNodeId, validatorKeys_.keys->publicKey);

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
    Json::Value&& consensusJson,
    const bool validating)
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
                // Export signatures are now collected ephemerally via
                // validation messages (signPendingExports in validate()),
                // not via ttEXPORT_SIGN transactions. This eliminates the
                // O(n²) metadata bloat from accumulating signatures on-ledger.
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
    NetClock::time_point closeTime,
    bool closeTimeCorrect,
    NetClock::duration closeResolution,
    std::chrono::milliseconds roundTime,
    std::set<TxID>& failedTxs)
{
    std::shared_ptr<Ledger> built = [&]() {
        if (auto const replayData = ledgerMaster_.releaseReplay())
        {
            XRPL_ASSERT(
                replayData->parent()->info().hash == previousLedger.id(),
                "ripple::RCLConsensus::Adaptor::buildLCL : parent hash match");
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

    //@@start validate-sign-exports
    // Sign pending exports and collect signatures for ephemeral broadcasting
    auto exportSigs = signPendingExports(*ledger.ledger_, app_, j_);

    // Store our own signatures in memory
    auto const currentSeq = ledger.ledger_->info().seq;
    for (auto const& [txnHash, signer] : exportSigs)
    {
        if (auto const validationPublicKey = app_.getValidationPublicKey())
            app_.getExportSignatureCollector().addSignature(
                txnHash, *validationPublicKey, signer, currentSeq);
    }

    // Broadcast to all our peers:
    protocol::TMValidation val;
    val.set_validation(serialized.data(), serialized.size());

    // Add export signatures to the validation message
    for (auto const& [txnHash, signer] : exportSigs)
    {
        Serializer s;
        s.addBitString(txnHash);
        signer.add(s);
        val.add_exportsignatures(s.data(), s.size());
    }

    if (!exportSigs.empty())
    {
        JLOG(j_.debug()) << "Export: broadcasting " << exportSigs.size()
                         << " signatures with validation for seq="
                         << ledger.seq();
    }
    //@@end validate-sign-exports
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

ConsensusPhase
RCLConsensus::phase() const
{
    return consensus_->phase();
}

bool
RCLConsensus::inRngSubState() const
{
    return consensus_->inRngSubState();
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

bool
RCLConsensus::Adaptor::preStartRound(
    RCLCxLedger const& prevLgr,
    hash_set<NodeID> const& nowTrusted)
{
    rngEnabledThisRound_ =
        prevLgr.ledger_->rules().enabled(featureConsensusEntropy);

    JLOG(j_.trace()) << "RNGGATE: preStartRound prevSeq=" << prevLgr.seq()
                     << " rulesEnabled=" << rngEnabledThisRound_;

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
                        << " FULL->CONNECTED positions=" << positions;
        app_.getOPs().setMode(OperatingMode::CONNECTED);
    }
}

//------------------------------------------------------------------------------
// RNG Helper Methods

std::size_t
RCLConsensus::Adaptor::quorumThreshold() const
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
RCLConsensus::Adaptor::setExpectedProposers(hash_set<NodeID> proposers)
{
    bool const includeSelf = mode() == ConsensusMode::proposing &&
        validatorKeys_.keys && validatorKeys_.nodeID != beast::zero;

    if (!proposers.empty())
    {
        // Intersect recent proposers with the active UNL. This set is used as
        // a liveness hint only; commit quorum itself remains fixed to the
        // active UNL snapshot for the round.
        hash_set<NodeID> filtered;
        for (auto const& id : proposers)
        {
            if (!includeSelf && id == validatorKeys_.nodeID)
                continue;
            if (unlReportNodeIds_.count(id))
                filtered.insert(id);
        }
        if (includeSelf)
            filtered.insert(validatorKeys_.nodeID);
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
RCLConsensus::Adaptor::pendingCommitCount() const
{
    return pendingCommits_.size();
}

std::size_t
RCLConsensus::Adaptor::pendingRevealCount() const
{
    return pendingReveals_.size();
}

std::size_t
RCLConsensus::Adaptor::expectedProposerCount() const
{
    return likelyParticipants_.size();
}

bool
RCLConsensus::Adaptor::hasQuorumOfCommits() const
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
    JLOG(j_.trace()) << "RNG: hasMinimumReveals? " << pendingReveals_.size()
                     << "/" << expected << " -> " << (result ? "YES" : "no");
    return result;
}

bool
RCLConsensus::Adaptor::hasAnyReveals() const
{
    return !pendingReveals_.empty();
}

bool
RCLConsensus::Adaptor::rngEnabled() const
{
    return rngEnabledThisRound_;
}

bool
RCLConsensus::Adaptor::shouldSendExplicitFinalProposal() const
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
RCLConsensus::Adaptor::buildExplicitFinalProposalTxSet(
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

    // Keep this entropy-selection logic aligned with injectEntropyPseudoTx().
    // If these paths drift, different nodes can derive different synthetic
    // hashes for the same round, which is especially harmful because this
    // path mutates proposal tx-set identity late in establish.
    if (app_.config().standalone())
    {
        finalEntropy = sha512Half(std::string("standalone-entropy"), seq);
        hasEntropy = true;
    }
    else if (entropyFailed_ || pendingReveals_.empty())
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
        app_.config().standalone() ? 20
                                   : (entropyFailed_ || pendingReveals_.empty()
                                          ? 0
                                          : pendingReveals_.size()));

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
    inboundTransactions_.giveSet(hash, syntheticSet.map_, false);

    JLOG(j_.debug()) << "RNGFINAL: built synthetic txset"
                     << " hash=" << hash << " baseTxSet=" << txns.id()
                     << " txid=" << txID << " entropyCount=" << entropyCount;

    return syntheticSet;
}

uint256
RCLConsensus::Adaptor::buildCommitSet(LedgerIndex seq)
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

        // Encode the NodeID into sfAccount so handleAcquiredRngSet can
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
    inboundTransactions_.giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built commitSet SHAMap hash=" << hash
                     << " entries=" << pendingCommits_.size();
    return hash;
    //@@end rng-build-commit-set
}

uint256
RCLConsensus::Adaptor::buildEntropySet(LedgerIndex seq)
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
    inboundTransactions_.giveSet(hash, map, false);

    JLOG(j_.debug()) << "RNG: built entropySet SHAMap hash=" << hash
                     << " entries=" << pendingReveals_.size();
    return hash;
    //@@end rng-build-entropy-set
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
    return validatorKeys_.keys->publicKey;
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
    rngRoundSeq_.reset();
    pendingRngFetches_.clear();
    unlReportNodeIds_.clear();
    likelyParticipants_.clear();
    commitProofs_.clear();
    proposalProofs_.clear();
    // Keep the round-level enable latch intact here. Consensus::startRound()
    // calls preStartRound() first to snapshot whether RNG is enabled for the
    // upcoming round, then immediately clears per-round working state.
    // Resetting rngEnabledThisRound_ here would wipe that snapshot before
    // phaseEstablish() can consult it.
}

void
RCLConsensus::Adaptor::cacheUNLReport()
{
    unlReportNodeIds_.clear();
    bool const includeSelf = mode() == ConsensusMode::proposing &&
        validatorKeys_.keys && validatorKeys_.nodeID != beast::zero;

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

    // Only include ourselves when actively proposing. Observers/non-validators
    // do not emit commitments and must not be expected in commit quorum.
    if (includeSelf)
        unlReportNodeIds_.insert(validatorKeys_.nodeID);
    else
        unlReportNodeIds_.erase(validatorKeys_.nodeID);

    JLOG(j_.trace()) << "RNG: cacheUNLReport size=" << unlReportNodeIds_.size();
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

    JLOG(j_.debug()) << "RNGFETCH: handle acquired hash=" << hash
                     << " pending-after-erase=" << pendingRngFetches_.size();

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
                if (auto const closed = ledgerMaster_.getClosedLedger())
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

void
RCLConsensus::Adaptor::fetchRngSetIfNeeded(std::optional<uint256> const& hash)
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
        if (auto existing = inboundTransactions_.getSet(*hash, false))
        {
            JLOG(j_.debug())
                << "RNGFETCH: pending fetch completed, merging hash=" << *hash;
            handleAcquiredRngSet(existing);
        }
        else
        {
            JLOG(j_.debug()) << "RNGFETCH: still pending hash=" << *hash;
        }
        return;
    }

    // Check if InboundTransactions already has it
    if (auto existing = inboundTransactions_.getSet(*hash, false))
    {
        JLOG(j_.debug()) << "RNGFETCH: local cache hit, merging hash=" << *hash;
        handleAcquiredRngSet(existing);
        return;
    }

    // Trigger network fetch
    JLOG(j_.debug()) << "RNGFETCH: triggering network fetch hash=" << *hash;
    pendingRngFetches_.insert(*hash);
    if (auto immediate = inboundTransactions_.getSet(*hash, true))
    {
        JLOG(j_.debug()) << "RNGFETCH: immediate fetch hit, merging hash="
                         << *hash;
        handleAcquiredRngSet(immediate);
    }
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
    else if (entropyFailed_ || pendingReveals_.empty())
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

        // Account Zero convention for pseudo-transactions (same as ttFEE, etc)
        auto const entropyCount = static_cast<std::uint16_t>(
            app_.config().standalone()
                ? 20  // synthetic: high enough for Hook APIs (need >= 5)
                : (entropyFailed_ || pendingReveals_.empty()
                       ? 0
                       : pendingReveals_.size()));
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
    }
    //@@end rng-inject-pseudotx

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

std::optional<RCLConsensus::Adaptor::ProposalProof>
RCLConsensus::Adaptor::deserializeProof(Blob const& proofBlob)
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
            JLOG(j_.debug()) << header_ << line << ".";
    }
    JLOG(j_.debug()) << header_ << "Total duration: " << duration.count()
                     << "ms.";
}
}  // namespace ripple
