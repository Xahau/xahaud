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

#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/ApplyContext.h>
#include <xrpld/app/tx/detail/InvariantCheck.h>
#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/detail/STVar.h>

namespace ripple {

namespace {

// Forwards to another RawView, rewinding the threading fields
// (PreviousTxnID / PreviousTxnLgrSeq) of every entry to the values the base
// holds for it, or to "never threaded" for an entry the base does not have.
//
// The subledger threads the entries it touches to its own transactions,
// which never enter the ledger's transaction list. Rewinding lets the owning
// transaction's metadata record each entry's real previous transaction and
// thread every touched entry to the owning transaction instead.
class Unthreader final : public RawView
{
public:
    Unthreader(RawView& to, ReadView const& base, bool forwardDestroyedXRP)
        : to_(to), base_(base), forwardDestroyedXRP_(forwardDestroyedXRP)
    {
    }

    void
    rawErase(std::shared_ptr<SLE> const& sle) override
    {
        to_.rawErase(rewind(sle));
    }

    void
    rawInsert(std::shared_ptr<SLE> const& sle) override
    {
        to_.rawInsert(rewind(sle));
    }

    void
    rawReplace(std::shared_ptr<SLE> const& sle) override
    {
        to_.rawReplace(rewind(sle));
    }

    void
    rawDestroyXRP(XRPAmount const& fee) override
    {
        // an open ledger never destroys fees (see Transactor::finishApply)
        if (forwardDestroyedXRP_)
            to_.rawDestroyXRP(fee);
    }

private:
    std::shared_ptr<SLE>
    rewind(std::shared_ptr<SLE> const& sle) const
    {
        if (!sle->isFieldPresent(sfPreviousTxnID) &&
            !sle->isFieldPresent(sfPreviousTxnLgrSeq))
            return sle;

        auto copy = std::make_shared<SLE>(*sle);
        auto const orig = base_.read(keylet::unchecked(sle->key()));
        if (!orig)
        {
            // created by the subledger: thread it from scratch
            if (copy->isFieldPresent(sfPreviousTxnID))
                copy->setFieldH256(sfPreviousTxnID, uint256{});
            if (copy->isFieldPresent(sfPreviousTxnLgrSeq))
                copy->setFieldU32(sfPreviousTxnLgrSeq, 0);
            return copy;
        }

        if (orig->isFieldPresent(sfPreviousTxnID))
            copy->setFieldH256(
                sfPreviousTxnID, orig->getFieldH256(sfPreviousTxnID));
        else if (copy->isFieldPresent(sfPreviousTxnID))
            copy->makeFieldAbsent(sfPreviousTxnID);

        if (orig->isFieldPresent(sfPreviousTxnLgrSeq))
            copy->setFieldU32(
                sfPreviousTxnLgrSeq, orig->getFieldU32(sfPreviousTxnLgrSeq));
        else if (copy->isFieldPresent(sfPreviousTxnLgrSeq))
            copy->makeFieldAbsent(sfPreviousTxnLgrSeq);
        return copy;
    }

    RawView& to_;
    ReadView const& base_;
    bool const forwardDestroyedXRP_;
};

}  // namespace

namespace subledger {

std::shared_ptr<SLE>
mergeEntry(SLE const& before, SLE const& ours, SLE const& theirs)
{
    XRPL_ASSERT(
        before.key() == ours.key() && before.key() == theirs.key() &&
            before.getType() == ours.getType() &&
            before.getType() == theirs.getType(),
        "ripple::subledger::mergeEntry : same entry");

    auto merged = std::make_shared<SLE>(theirs);

    // Ledger entries are templated: all three hold the same fields (present
    // or not), so iterating `before` visits every one of them.
    for (auto const& field : before)
    {
        SField const& f = field.getFName();
        bool const inBefore = before.isFieldPresent(f);
        bool const inOurs = ours.isFieldPresent(f);

        bool const changed = inOurs != inBefore ||
            (inOurs && !(ours.peekAtField(f) == before.peekAtField(f)));
        if (!changed)
            continue;

        bool const inTheirs = theirs.isFieldPresent(f);
        if (inTheirs != inBefore ||
            (inTheirs && !(theirs.peekAtField(f) == before.peekAtField(f))))
            return nullptr;

        if (inOurs)
        {
            detail::STVar copy(ours.peekAtField(f));
            merged->set(std::move(copy.get()));
        }
        else
            merged->makeFieldAbsent(f);
    }

    return merged;
}

}  // namespace subledger

ApplyContext::ApplyContext(
    Application& app_,
    OpenView& base,
    STTx const& tx_,
    TER preclaimResult_,
    XRPAmount baseFee_,
    ApplyFlags flags,
    beast::Journal journal_)
    : app(app_)
    , tx(tx_)
    , preclaimResult(preclaimResult_)
    , baseFee(baseFee_)
    , journal(journal_)
    , base_(base)
    , flags_(flags)
{
    view_.emplace(&base_, flags_);
}

void
ApplyContext::discard()
{
    // featureRNG: Hook draws are never undone. If this transaction drew from
    // the RNG, its advanced state survives the discard (as does the record of
    // what its Hooks were given), so a rollback cannot preserve an RNG state
    // for whatever executes next. Read through the working view before the
    // subledger (if any) is dropped: a draw made before the first
    // emit_atomic was flushed into the subledger and is only visible
    // through it. Draws made while there is a subledger went to the real
    // cursor (writeBackRng).
    std::optional<uint256> const randomData = view_->getRandomData();
    std::optional<uint256> rngState;
    if (randomData)
    {
        if (auto const sle = view_->read(keylet::random()))
            rngState = sle->getFieldH256(sfRandomData);
    }

    // the working view may sit on a subledger: replace it before dropping
    // the subledgers
    view_.emplace(&base_, flags_);
    subledger_.reset();
    subledgerEntries_.clear();
    realSubledger_.reset();
    realEntries_.clear();
    indeterminate_ = false;
    diverged_ = false;

    if (randomData)
        view_->setRandomData(*randomData);

    if (rngState)
    {
        if (auto sle = view_->peek(keylet::random()))
        {
            sle->setFieldH256(sfRandomData, *rngState);
            view_->update(sle);
        }
    }

    writeBackRng();
    rng_.reset();
}

std::optional<TxMeta>
ApplyContext::apply(TER ter)
{
    if (subledger_)
        return commitSubledger(ter);
    return view_->apply(base_, tx, ter, flags_ & tapDRY_RUN, journal);
}

SubledgerRng*
ApplyContext::subledgerRng(bool& shadow) const
{
    if (flags_ & tapATOMIC_EMIT)
    {
        shadow = base_.subledgerRngShadow();
        return base_.subledgerRng();
    }

    shadow = false;
    return rng_.get();
}

void
ApplyContext::writeBackRng()
{
    if (!rng_ || !rng_->realDrawn || !rng_->real)
        return;

    if (auto sle = view_->peek(keylet::random()))
    {
        sle->setFieldH256(sfRandomData, *rng_->real);
        view_->update(sle);
    }
}

bool
ApplyContext::stillApplies(ReadView const& after) const
{
    auto const id = tx.getAccountID(sfAccount);
    auto const before = base_.read(keylet::account(id));
    auto const sle = after.read(keylet::account(id));

    // an account that did not exist (e.g. a first Import) must still not
    // exist, and one that did must not have been deleted
    if (!before || !sle)
        return !before && !sle;

    // e.g. an emitted TicketCreate moves the account's sequence: consuming
    // this transaction's sequence afterwards would move it back
    if (!isTesSuccess(Transactor::checkSeqProxy(after, tx, journal)))
        return false;

    // the fee is charged after the subledger (Transactor::apply)
    return sle->getFieldAmount(sfBalance).xrp() >=
        tx.getFieldAmount(sfFee).xrp();
}

std::shared_ptr<STObject const>
ApplyContext::makeSubledgerEntry(
    STTx const& stx,
    TxMeta const& meta,
    bool indeterminate) const
{
    auto entry = std::make_shared<STObject>(sfSubledgerTransaction);
    entry->setFieldH256(sfEmittedTxnID, stx.getTransactionID());
    if (indeterminate)
        entry->setFieldU32(sfFlags, stfIndeterminate);
    {
        Serializer s;
        stx.add(s);
        SerialIter sit(s.slice());
        entry->emplace_back(STObject(sit, sfEmittedTxn));
    }
    entry->emplace_back(meta.getAsObject());
    return entry;
}

ApplyContext::SubledgerResult
ApplyContext::applyToSubledger(std::shared_ptr<STTx const> const& stx)
{
    XRPL_ASSERT(
        !(flags_ & tapATOMIC_EMIT),
        "ripple::ApplyContext::applyToSubledger : not inside a subledger");

    if (!subledger_)
    {
        subledger_ = std::make_unique<OpenView>(subledger_view, base_);
        rng_ =
            std::make_unique<SubledgerRng>(tx.getTransactionID(), base_.open());
    }

    // Move the working view's pending changes (e.g. the TouchCount of the
    // TSH accounts touched so far) into the subledger(s) and continue on top
    // of the visible one. The working view buffers whole entries, so nothing
    // may change underneath it once it holds any. The real subledger gets
    // them merged field by field: they were made on top of the visible one.
    if (realSubledger_ && !diverged_ && !mergeInto(*realSubledger_))
    {
        JLOG(journal.debug()) << "Subledger[" << tx.getTransactionID()
                              << "]: pending changes conflict with the real "
                                 "subledger";
        diverged_ = true;
    }
    view_->flush(*subledger_);
    view_->rebase(subledger_.get());

    auto const id = stx->getTransactionID();

    // The visible application. No tapDRY_RUN: the inner must really apply
    // to the child (the whole subledger is discarded or committed with this
    // transaction, which honours tapDRY_RUN). No tapRETRY / tapFAIL_HARD: a
    // tec inner is final. Its Hooks draw from the shadow RNG.
    rng_->shadowDrawn = false;
    OpenView child(closed_view, *subledger_, rng_.get(), /*shadow=*/true);
    auto const result =
        ripple::apply(app, child, *stx, tapATOMIC_EMIT, journal);

    if (!result.applied)
    {
        JLOG(journal.debug())
            << "Subledger[" << tx.getTransactionID() << "]: " << id
            << " not applied: " << transToken(result.ter);
        return {result.ter, nullptr};
    }

    if (!stillApplies(child))
    {
        JLOG(journal.debug())
            << "Subledger[" << tx.getTransactionID() << "]: " << id
            << " refused: the transaction could not be applied after it";
        return {result.ter, nullptr};
    }

    // the child is closed, so metadata is always generated
    if (!result.metadata)
    {
        // LCOV_EXCL_START
        UNREACHABLE(
            "ripple::ApplyContext::applyToSubledger : missing metadata");
        return {tefINTERNAL, nullptr};
        // LCOV_EXCL_STOP
    }

    // Taint by order only: whether the real outcome turns out to differ must
    // never reach the Hooks.
    if (rng_->shadowDrawn)
        indeterminate_ = true;

    // The real application, needed once the subledgers can differ: this
    // inner drew (the subledgers fork here, from the subledger as it is
    // before this inner), or they already have.
    if (indeterminate_ && !diverged_)
    {
        OpenView& realBase = realSubledger_ ? *realSubledger_ : *subledger_;
        OpenView realChild(closed_view, realBase, rng_.get(), false);
        auto const real =
            ripple::apply(app, realChild, *stx, tapATOMIC_EMIT, journal);

        if (!real.applied || !real.metadata || !stillApplies(realChild))
        {
            JLOG(journal.debug())
                << "Subledger[" << tx.getTransactionID() << "]: " << id
                << " could not be applied to the real subledger: "
                << transToken(real.ter);
            diverged_ = true;
        }
        else
        {
            if (!realSubledger_)
            {
                // fork: the subledgers were the same up to this inner (the
                // copy shares the unchanged entries, which are never modified
                // in place)
                realSubledger_ = std::make_unique<OpenView>(*subledger_);
                realEntries_ = subledgerEntries_;
            }
            realChild.apply(*realSubledger_);
            realEntries_.push_back(
                makeSubledgerEntry(*stx, *real.metadata, true));
        }
    }

    child.apply(*subledger_);

    auto entry = makeSubledgerEntry(*stx, *result.metadata, indeterminate_);
    subledgerEntries_.push_back(entry);
    // keep the provisional metadata in step with what is visible
    syncSubledgerMetaData();

    JLOG(journal.trace()) << "Subledger[" << tx.getTransactionID()
                          << "]: " << id
                          << " applied: " << transToken(result.ter)
                          << (indeterminate_ ? " (indeterminate)" : "");
    return {result.ter, std::move(entry)};
}

bool
ApplyContext::mergeInto(OpenView& real)
{
    bool ok = true;
    view_->visit(
        *subledger_,
        [&](uint256 const& key,
            bool isDelete,
            std::shared_ptr<SLE const> const& before,
            std::shared_ptr<SLE const> const& after) {
            if (!ok)
                return;

            auto const theirs = real.read(keylet::unchecked(key));

            if (isDelete)
            {
                // the entry must be as this transaction saw it
                if (!before || !theirs || !(*theirs == *before))
                {
                    ok = false;
                    return;
                }
                real.rawErase(std::make_shared<SLE>(*theirs));
                return;
            }

            if (!before)
            {
                // created: the key must still be free
                if (theirs)
                {
                    ok = false;
                    return;
                }
                real.rawInsert(std::make_shared<SLE>(*after));
                return;
            }

            if (!theirs)
            {
                ok = false;
                return;
            }

            auto merged = subledger::mergeEntry(*before, *after, *theirs);
            if (!merged)
            {
                ok = false;
                return;
            }
            real.rawReplace(merged);
        });
    return ok;
}

bool
ApplyContext::concretizeSubledger()
{
    // also called for inner transactions, which never have a subledger
    if (!subledger_)
        return true;

    if (diverged_)
    {
        JLOG(journal.debug())
            << "Subledger[" << tx.getTransactionID() << "]: diverged";
        return false;
    }

    if (!realSubledger_)
        return true;

    // Merge into a copy, so that a failure leaves everything in place.
    auto real = std::make_unique<OpenView>(*realSubledger_);
    if (!mergeInto(*real) || !stillApplies(*real))
    {
        JLOG(journal.debug()) << "Subledger[" << tx.getTransactionID()
                              << "]: cannot reconcile the real subledger";
        return false;
    }

    // The working view's changes are in the real subledger now. Drop them
    // (into the visible subledger, which goes too) and continue on the real
    // one.
    view_->flush(*subledger_);
    view_->rebase(real.get());
    subledger_ = std::move(real);
    subledgerEntries_ = std::move(realEntries_);
    realSubledger_.reset();
    realEntries_.clear();
    syncSubledgerMetaData();

    JLOG(journal.trace()) << "Subledger[" << tx.getTransactionID()
                          << "]: continuing on the real subledger";
    return true;
}

std::optional<TxMeta>
ApplyContext::commitSubledger(TER ter)
{
    // Every path that turns a transaction with a subledger into a tec goes
    // through discard(), which drops it: reset() on a doApply tec, invariant
    // failure or oversize, and the dedicated weak-phase oversize block in
    // Transactor::finishApply.
    XRPL_ASSERT(
        isTesSuccess(ter),
        "ripple::ApplyContext::commitSubledger : transaction succeeded");
    // concretizeSubledger() ran at the end of the strong phase
    XRPL_ASSERT(
        !realSubledger_,
        "ripple::ApplyContext::commitSubledger : subledger is concrete");

    // Fold the working view into the subledger, then the whole subledger
    // back into the working view re-layered on the base. The working view
    // keeps its hook metadata, Subledger metadata (see syncSubledgerMetaData)
    // and delivered amount, and its AffectedNodes now describe the net change
    // against the base.
    view_->flush(*subledger_);
    view_->rebase(&base_);
    {
        Unthreader to(*view_, base_, !base_.open());
        subledger_->applyState(to);
    }
    writeBackRng();

    // the working view no longer refers to it
    subledger_.reset();
    subledgerEntries_.clear();
    rng_.reset();
    indeterminate_ = false;

    return view_->apply(base_, tx, ter, flags_ & tapDRY_RUN, journal);
}

void
ApplyContext::syncSubledgerMetaData()
{
    std::vector<STObject> subledger;
    subledger.reserve(subledgerEntries_.size());
    for (auto const& entry : subledgerEntries_)
        subledger.push_back(*entry);
    view_->setSubledgerMetaData(std::move(subledger));
}

std::size_t
ApplyContext::size()
{
    return view_->size() + (subledger_ ? subledger_->stateSize() : 0);
}

void
ApplyContext::visit(std::function<void(
                        uint256 const&,
                        bool,
                        std::shared_ptr<SLE const> const&,
                        std::shared_ptr<SLE const> const&)> const& func)
{
    view_->visit(viewBase(), func);
}

TER
ApplyContext::failInvariantCheck(TER const result)
{
    // If we already failed invariant checks before and we are now attempting to
    // only charge a fee, and even that fails the invariant checks something is
    // very wrong. We switch to tefINVARIANT_FAILED, which does NOT get included
    // in a ledger.

    return (result == tecINVARIANT_FAILED || result == tefINVARIANT_FAILED)
        ? TER{tefINVARIANT_FAILED}
        : TER{tecINVARIANT_FAILED};
}

template <std::size_t... Is>
TER
ApplyContext::checkInvariantsHelper(
    TER const result,
    XRPAmount const fee,
    std::index_sequence<Is...>)
{
    try
    {
        auto checkers = getInvariantChecks();

        // call each check's per-entry method
        visit([&checkers](
                  uint256 const& index,
                  bool isDelete,
                  std::shared_ptr<SLE const> const& before,
                  std::shared_ptr<SLE const> const& after) {
            (..., std::get<Is>(checkers).visitEntry(isDelete, before, after));
        });

        // Note: do not replace this logic with a `...&&` fold expression.
        // The fold expression will only run until the first check fails (it
        // short-circuits). While the logic is still correct, the log
        // message won't be. Every failed invariant should write to the log,
        // not just the first one.
        std::array<bool, sizeof...(Is)> finalizers{
            {std::get<Is>(checkers).finalize(
                tx, result, fee, *view_, journal)...}};

        // call each check's finalizer to see that it passes
        if (!std::all_of(
                finalizers.cbegin(), finalizers.cend(), [](auto const& b) {
                    return b;
                }))
        {
            JLOG(journal.fatal())
                << "Transaction has failed one or more invariants: "
                << to_string(tx.getJson(JsonOptions::none));

            return failInvariantCheck(result);
        }
    }
    catch (std::exception const& ex)
    {
        JLOG(journal.fatal())
            << "Transaction caused an exception in an invariant"
            << ", ex: " << ex.what()
            << ", tx: " << to_string(tx.getJson(JsonOptions::none));

        return failInvariantCheck(result);
    }

    return result;
}

TER
ApplyContext::checkInvariants(TER const result, XRPAmount const fee)
{
    XRPL_ASSERT(
        isTesSuccess(result) || isTecClaim(result),
        "ripple::ApplyContext::checkInvariants : is tesSUCCESS or tecCLAIM");

    return checkInvariantsHelper(
        result,
        fee,
        std::make_index_sequence<std::tuple_size<InvariantChecks>::value>{});
}

}  // namespace ripple
