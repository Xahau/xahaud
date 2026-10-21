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

#ifndef RIPPLE_TX_APPLYCONTEXT_H_INCLUDED
#define RIPPLE_TX_APPLYCONTEXT_H_INCLUDED

#include <xrpld/app/main/Application.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/XRPAmount.h>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ripple {

namespace subledger {

/** Three-way merge of one ledger entry, field by field (featureAtomicEmit).

    `before` is the entry as the transaction read it (from the subledger its
    Hooks see), `ours` is the transaction's version of it and `theirs` is the
    entry in the real subledger. Every field `ours` changed relative to
    `before` is taken from `ours`; every other field from `theirs`.

    Returns nullptr on a conflict: a field `ours` changed that `theirs` does
    not hold as `before` held it. All three must be the same entry (key and
    type).
*/
std::shared_ptr<SLE>
mergeEntry(SLE const& before, SLE const& ours, SLE const& theirs);

}  // namespace subledger

/** State information when applying a tx. */
class ApplyContext
{
public:
    explicit ApplyContext(
        Application& app,
        OpenView& base,
        STTx const& tx,
        TER preclaimResult,
        XRPAmount baseFee,
        ApplyFlags flags,
        beast::Journal = beast::Journal{beast::Journal::getNullSink()});

    Application& app;
    STTx const& tx;
    TER const preclaimResult;
    XRPAmount const baseFee;
    beast::Journal const journal;

    /** Number of emit_atomic attempts made so far for this transaction,
        across every hook execution. Lives here (not on the ApplyViewImpl)
        so it survives discard()/reset(); it is never decremented, even when
        the attempt fails or a hook later rolls back. */
    std::uint32_t atomicEmitCount = 0;

    ApplyView&
    view()
    {
        return *view_;
    }

    ApplyView const&
    view() const
    {
        return *view_;
    }

    // VFALCO Unfortunately this is necessary
    RawView&
    rawView()
    {
        return *view_;
    }

    ApplyFlags const&
    flags() const
    {
        return flags_;
    }

    /** Sets the DeliveredAmount field in the metadata */
    void
    deliver(STAmount const& amount)
    {
        view_->deliver(amount);
    }

    /** Discard changes and start fresh. The subledger(s), if any, are
        discarded too. RNG draws made through the subledger cursors are
        written to the Random object of the fresh view (never undone). */
    void
    discard();

    /** Apply the transaction result to the base.

        If the transaction has a subledger it is committed as part of the
        transaction: the metadata's AffectedNodes describe the net change
        (subledger and transaction together, every entry threaded to this
        transaction) and its Subledger array lists the subledger's
        transactions with their own metadata. Only this transaction is
        added to the base's transaction list.
    */
    std::optional<TxMeta> apply(TER);

    /** Outcome of applyToSubledger(). */
    struct SubledgerResult
    {
        // the inner transaction's result
        TER ter;
        // its SubledgerTransaction entry; set iff the inner transaction was
        // applied (tes or tec) and merged into the subledger
        std::shared_ptr<STObject const> entry;
    };

    /** Apply an atomically emitted (emit_atomic) transaction into this
        transaction's subledger, creating the subledger on first use.

        The subledger sits between the base and the working view: from then
        on view() reads through it, this transaction's own changes are made
        on top of it, apply() commits it with them and discard() drops it.
        Before each inner transaction the working view's pending changes are
        moved into the subledger, so that the subledger is never modified
        underneath them.

        The inner transaction is applied with tapATOMIC_EMIT to a closed
        child view of the subledger, where it runs its own preflight,
        preclaim, hooks and invariant checks. It is merged into the subledger
        only if it was applied (tes or tec) and this transaction can still
        be applied on top of it: its account must exist exactly when it did
        before, its sequence or ticket must still be valid and its balance
        must still cover its fee.

        RNG (featureRNG): there are two subledgers once any inner
        transaction's Hooks draw from the RNG. The visible subledger is the
        one the strong Hooks see (view() reads through it, the returned entry
        describes it); its inner transactions draw from a shadow RNG forked
        off the real one. The real subledger applies the same inner
        transactions with the real RNG, and is the one committed. Nothing
        about the real subledger reaches the caller: the result describes the
        visible application only. An inner transaction that drew, and every
        one applied after it, is flagged stfIndeterminate. Until the first
        draw both subledgers are the same, and only one exists.
    */
    SubledgerResult
    applyToSubledger(std::shared_ptr<STTx const> const& stx);

    /** End of the strong phase (featureAtomicEmit): continue on the real
        subledger.

        Called once every strong Hook chain has run, before their state is
        finalized: nothing that runs afterwards can roll this transaction
        back, so from here on the working view sits on the real subledger,
        and the visible one is dropped.

        The working view's pending changes, made on top of the visible
        subledger, are merged into the real one field by field (see
        subledger::mergeEntry). Returns false, leaving everything in place,
        if the subledgers cannot be reconciled: an inner transaction applied
        to the visible subledger was not applied to the real one, this
        transaction could not be applied after the real subledger, or a merge
        conflicted. The caller must then fail the transaction
        (tecSUBLEDGER_DIVERGED), which discards both.

        Returns true and does nothing when there is no real subledger (no
        inner transaction drew from the RNG, or there is no subledger).
    */
    bool
    concretizeSubledger();

    /** True iff this transaction has a subledger. */
    bool
    hasSubledger() const
    {
        return static_cast<bool>(subledger_);
    }

    /** The RNG cursors Hooks executing for this transaction draw from
        instead of the Random object, or nullptr if they draw from the Random
        object directly (see SubledgerRng). For an inner transaction
        (tapATOMIC_EMIT) these are its parent's, and `shadow` says which
        cursor; otherwise they exist while this transaction has a subledger
        and `shadow` is false. */
    SubledgerRng*
    subledgerRng(bool& shadow) const;

    /** True iff a Hook drew from this transaction's real RNG cursor. */
    bool
    subledgerRngDrawn() const
    {
        return rng_ && rng_->realDrawn;
    }

    /** The subledger's transactions, in application order. Each is a
        SubledgerTransaction { EmittedTxnID, EmittedTxn,
        TransactionMetaData }. */
    std::vector<std::shared_ptr<STObject const>> const&
    subledgerEntries() const
    {
        return subledgerEntries_;
    }

    /** Get the number of unapplied changes made by this transaction.

        With a subledger this also counts the entries the subledger changes,
        since apply() commits them as part of this transaction's metadata.
        An entry changed by both is counted twice, which is conservative
        (the count only feeds the oversizeMetaDataCap checks). */
    std::size_t
    size();

    /** Visit unapplied changes. With a subledger, these are this
        transaction's own changes and `before` is read from the subledger. */
    void
    visit(std::function<void(
              uint256 const& key,
              bool isDelete,
              std::shared_ptr<SLE const> const& before,
              std::shared_ptr<SLE const> const& after)> const& func);

    void
    destroyXRP(XRPAmount const& fee)
    {
        view_->rawDestroyXRP(fee);
    }

    TxMeta
    generateProvisionalMeta()
    {
        return view_->generateProvisionalMeta(viewBase(), tx, journal);
    }

    /** Applies all invariant checkers one by one.

        @param result the result generated by processing this transaction.
        @param fee the fee charged for this transaction
        @return the result code that should be returned for this transaction.
     */
    TER
    checkInvariants(TER const result, XRPAmount const fee);

    bool
    isEmittedTxn()
    {
        return tx.isFieldPresent(sfEmitDetails);
    }

    ApplyFlags const&
    flags()
    {
        return flags_;
    }

private:
    TER
    failInvariantCheck(TER const result);

    template <std::size_t... Is>
    TER
    checkInvariantsHelper(
        TER const result,
        XRPAmount const fee,
        std::index_sequence<Is...>);

    // the view the working view sits on: the subledger if there is one,
    // otherwise the base
    OpenView&
    viewBase()
    {
        return subledger_ ? *subledger_ : base_;
    }

    // true iff this transaction can still be applied on top of `after`
    bool
    stillApplies(ReadView const& after) const;

    std::optional<TxMeta>
    commitSubledger(TER ter);

    // copy subledgerEntries_ into the working view's Subledger metadata
    void
    syncSubledgerMetaData();

    // merge the working view's pending changes, made on top of the visible
    // subledger, into the real subledger; false on a conflict
    bool
    mergeInto(OpenView& real);

    // write the real RNG cursor to the working view's Random object
    void
    writeBackRng();

    std::shared_ptr<STObject const>
    makeSubledgerEntry(STTx const& stx, TxMeta const& meta, bool indeterminate)
        const;

    OpenView& base_;
    ApplyFlags flags_;
    std::optional<ApplyViewImpl> view_;
    // see applyToSubledger(). subledger_ is the subledger view() reads
    // through: until concretizeSubledger() the visible one, afterwards the
    // real one.
    std::unique_ptr<OpenView> subledger_;
    std::vector<std::shared_ptr<STObject const>> subledgerEntries_;
    // the real subledger, once it differs from the visible one (an inner
    // transaction drew from the RNG); its entries
    std::unique_ptr<OpenView> realSubledger_;
    std::vector<std::shared_ptr<STObject const>> realEntries_;
    // an inner transaction drew from the RNG: it and everything applied
    // after it are indeterminate
    bool indeterminate_ = false;
    // the real subledger could not follow the visible one
    bool diverged_ = false;
    // see SubledgerRng; exists while subledger_ does
    std::unique_ptr<SubledgerRng> rng_;
};

}  // namespace ripple

#endif
