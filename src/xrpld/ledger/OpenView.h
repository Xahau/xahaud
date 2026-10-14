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

#ifndef RIPPLE_LEDGER_OPENVIEW_H_INCLUDED
#define RIPPLE_LEDGER_OPENVIEW_H_INCLUDED

#include <xrpld/ledger/RawView.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/ledger/detail/RawStateTable.h>
#include <xrpl/protocol/XRPAmount.h>

#include <boost/container/pmr/monotonic_buffer_resource.hpp>
#include <boost/container/pmr/polymorphic_allocator.hpp>

#include <functional>
#include <optional>
#include <utility>

namespace ripple {

/** Open ledger construction tag.

    Views constructed with this tag will have the
    rules of open ledgers applied during transaction
    processing.
*/
struct open_ledger_t
{
    explicit open_ledger_t() = default;
};

extern open_ledger_t const open_ledger;

/** Closed nested view construction tag.

    A view constructed with this tag sits on top of another OpenView
    and is always treated as a closed ledger, regardless of the base.
    It is used to apply one atomically emitted hook transaction
    (emit_atomic) on top of a subledger, so that the transaction can be
    checked before it is merged into the subledger or thrown away.
    See ApplyContext::applyToSubledger.
*/
struct closed_view_t
{
    explicit closed_view_t() = default;
};

extern closed_view_t const closed_view;

/** Subledger construction tag.

    A view constructed with this tag is the subledger of a transaction
    whose hooks emitted transactions atomically (emit_atomic). It sits on
    top of the view that transaction is being applied to and reports the
    same open() as that view. Its own transactions are numbered from zero
    and txExists() also consults the base. See ApplyContext.
*/
struct subledger_view_t
{
    explicit subledger_view_t() = default;
};

extern subledger_view_t const subledger_view;

/** RNG cursors of a transaction that has a subledger (featureAtomicEmit).

    While a transaction has a subledger, no Hook draws from the ledger's
    Random object directly. Every draw goes through one of two cursors:

    real    the RNG as it will be committed. Seeded from the Random object
            on first use; it carries on from any draw made before the
            subledger existed. The owning transaction's own Hooks, and the
            real application of every inner transaction, draw from it. It is
            written back to the Random object when the transaction is
            committed or discarded, so draws are never undone.

    shadow  forked off `real` (one-way) on the first draw made while an inner
            transaction is applied to the subledger the emitting Hooks can
            see. Outcomes drawn from it are never committed.

    Neither cursor is ever stored in a ledger entry, so no inner transaction
    modifies the Random object and none of its metadata can expose the state.
    See ApplyContext::applyToSubledger.
*/
struct SubledgerRng
{
    explicit SubledgerRng(uint256 const& otxnID_, bool open_)
        : otxnID(otxnID_), open(open_)
    {
    }

    // the transaction that owns the subledger (input to the shadow fork)
    uint256 const otxnID;
    // whether that transaction is applied to an open ledger: inner
    // transactions are applied to closed views, but their Hooks may draw on
    // the same seed as the owning transaction (see HookAPI::rng_next)
    bool const open;

    std::optional<uint256> real;
    std::optional<uint256> shadow;

    // something drew from `real` (it must be written back)
    bool realDrawn = false;
    // something drew from `shadow` since this was last cleared
    bool shadowDrawn = false;
};

//------------------------------------------------------------------------------

/** Writable ledger view that accumulates state and tx changes.

    @note Presented as ReadView to clients.
*/
class OpenView final : public ReadView, public TxsRawView
{
private:
    // Initial size for the monotonic_buffer_resource used for allocations
    // The size was chosen from the old `qalloc` code (which this replaces).
    // It is unclear how the size initially chosen in qalloc.
    static constexpr size_t initialBufferSize = kilobytes(256);

    class txs_iter_impl;

    struct txData
    {
        std::shared_ptr<Serializer const> txn;
        std::shared_ptr<Serializer const> meta;

        // Constructor needed for emplacement in std::map
        txData(
            std::shared_ptr<Serializer const> const& txn_,
            std::shared_ptr<Serializer const> const& meta_)
            : txn(txn_), meta(meta_)
        {
        }
    };

    // List of tx, key order
    // Use boost::pmr functionality instead of std::pmr
    // functions b/c clang does not support pmr yet (as-of 9/2020)
    using txs_map = std::map<
        key_type,
        txData,
        std::less<key_type>,
        boost::container::pmr::polymorphic_allocator<
            std::pair<key_type const, txData>>>;

    // monotonic_resource_ must outlive `items_`. Make a pointer so it may be
    // easily moved.
    std::unique_ptr<boost::container::pmr::monotonic_buffer_resource>
        monotonic_resource_;
    txs_map txs_;
    Rules rules_;
    LedgerInfo info_;
    ReadView const* base_;
    detail::RawStateTable items_;
    std::shared_ptr<void const> hold_;
    bool open_ = true;

    // closed_view: number of transactions already in the base chain (so
    // TransactionIndex continues from the base). closed_view and
    // subledger_view: the base OpenView for txExists() delegation.
    // Zero / nullptr for every other view.
    std::size_t baseTxCount_ = 0;
    OpenView const* baseTxs_ = nullptr;

    // closed_view only: the RNG cursors Hooks of the transaction applied to
    // this view draw from, and which one (see SubledgerRng)
    SubledgerRng* rng_ = nullptr;
    bool rngShadow_ = false;

public:
    OpenView() = delete;
    OpenView&
    operator=(OpenView&&) = delete;
    OpenView&
    operator=(OpenView const&) = delete;

    OpenView(OpenView&&) = default;

    /** Construct a shallow copy.

        Effects:

            Creates a new object with a copy of
            the modification state table.

        The objects managed by shared pointers are
        not duplicated but shared between instances.
        Since the SLEs are immutable, calls on the
        RawView interface cannot break invariants.
    */
    OpenView(OpenView const&);

    /** Construct an open ledger view.

        Effects:

            The sequence number is set to the
            sequence number of parent plus one.

            The parentCloseTime is set to the
            closeTime of parent.

            If `hold` is not nullptr, retains
            ownership of a copy of `hold` until
            the MetaView is destroyed.

            Calls to rules() will return the
            rules provided on construction.

        The tx list starts empty and will contain
        all newly inserted tx.
    */
    /** @{ */
    OpenView(
        open_ledger_t,
        ReadView const* base,
        Rules const& rules,
        std::shared_ptr<void const> hold = nullptr);

    OpenView(
        open_ledger_t,
        Rules const& rules,
        std::shared_ptr<ReadView const> const& base)
        : OpenView(open_ledger, &*base, rules, base)
    {
    }
    /** @} */

    /** Construct a new last closed ledger.

        Effects:

            The LedgerInfo is copied from the base.

            The rules are inherited from the base.

        The tx list starts empty and will contain
        all newly inserted tx.
    */
    OpenView(ReadView const* base, std::shared_ptr<void const> hold = nullptr);

    /** Construct a sandbox view on top of another OpenView.

        Effects:

            The LedgerInfo and rules are copied from the base
            (the sequence is NOT incremented).

            The view always reports closed (open() == false), even
            when the base is an open ledger, so that transactions
            applied into it see consensus-side semantics.

            txCount() continues from the base so that metadata
            TransactionIndex values stay contiguous, and txExists()
            also consults the base.

            When `rng` is given, Hooks executed by the transaction applied to
            this view draw from its shadow cursor if `shadow` is true and
            from its real cursor otherwise, never from the Random object.
    */
    OpenView(
        closed_view_t,
        OpenView const& base,
        SubledgerRng* rng = nullptr,
        bool shadow = false);

    /** The RNG cursors of a closed_view child (see SubledgerRng), or nullptr.
     */
    SubledgerRng*
    subledgerRng() const
    {
        return rng_;
    }

    /** True iff draws in this view use the shadow cursor. */
    bool
    subledgerRngShadow() const
    {
        return rngShadow_;
    }

    /** Construct a subledger on top of another OpenView.

        Effects:

            The LedgerInfo, rules and open() are copied from the base
            (the sequence is NOT incremented).

            txCount() counts only this view's own transactions, so the
            transactions applied into it are numbered from zero.

            txExists() also consults the base.
    */
    OpenView(subledger_view_t, OpenView const& base);

    /** Returns true if this reflects an open ledger. */
    bool
    open() const override
    {
        return open_;
    }

    /** Return the number of tx inserted since creation.

        This is used to set the "apply ordinal"
        when calculating transaction metadata.
    */
    std::size_t
    txCount() const;

    /** Apply changes. */
    void
    apply(TxsRawView& to) const;

    /** Apply state changes only (no transactions).

        Also forwards the XRP destroyed in this view
        (RawStateTable::apply calls to.rawDestroyXRP).
        Used to fold a subledger into the transaction that owns
        it: the subledger's transactions are recorded in that
        transaction's metadata, not in the ledger's tx list.
    */
    void
    applyState(RawView& to) const;

    /** The number of state entries this view changes in its base. */
    std::size_t
    stateSize() const
    {
        return items_.size();
    }

    // ReadView

    LedgerInfo const&
    info() const override;

    Fees const&
    fees() const override;

    Rules const&
    rules() const override;

    bool
    exists(Keylet const& k) const override;

    std::optional<key_type>
    succ(
        key_type const& key,
        std::optional<key_type> const& last = std::nullopt) const override;

    std::shared_ptr<SLE const>
    read(Keylet const& k) const override;

    std::unique_ptr<sles_type::iter_base>
    slesBegin() const override;

    std::unique_ptr<sles_type::iter_base>
    slesEnd() const override;

    std::unique_ptr<sles_type::iter_base>
    slesUpperBound(uint256 const& key) const override;

    std::unique_ptr<txs_type::iter_base>
    txsBegin() const override;

    std::unique_ptr<txs_type::iter_base>
    txsEnd() const override;

    bool
    txExists(key_type const& key) const override;

    tx_type
    txRead(key_type const& key) const override;

    // RawView

    void
    rawErase(std::shared_ptr<SLE> const& sle) override;

    void
    rawInsert(std::shared_ptr<SLE> const& sle) override;

    void
    rawReplace(std::shared_ptr<SLE> const& sle) override;

    void
    rawDestroyXRP(XRPAmount const& fee) override;

    // TxsRawView

    void
    rawTxInsert(
        key_type const& key,
        std::shared_ptr<Serializer const> const& txn,
        std::shared_ptr<Serializer const> const& metaData) override;
};

}  // namespace ripple

#endif
