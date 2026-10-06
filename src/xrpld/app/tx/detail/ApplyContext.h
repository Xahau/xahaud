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

    /** Discard changes and start fresh. The subledger, if any, is
        discarded too. */
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
    */
    SubledgerResult
    applyToSubledger(std::shared_ptr<STTx const> const& stx);

    /** True iff this transaction has a subledger. */
    bool
    hasSubledger() const
    {
        return static_cast<bool>(subledger_);
    }

    /** The subledger's transactions, in application order. Each is a
        SubledgerTransaction { EmittedTxnID, EmittedTxn,
        TransactionMetaData }. */
    std::vector<std::shared_ptr<STObject const>> const&
    subledgerEntries() const
    {
        return subledgerEntries_;
    }

    /** Get the number of unapplied changes made by this transaction
        (changes made by its subledger's transactions are not counted). */
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

    OpenView& base_;
    ApplyFlags flags_;
    std::optional<ApplyViewImpl> view_;
    // see applyToSubledger()
    std::unique_ptr<OpenView> subledger_;
    std::vector<std::shared_ptr<STObject const>> subledgerEntries_;
};

}  // namespace ripple

#endif
