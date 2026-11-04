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

#ifndef RIPPLE_APP_MISC_CANONICALTXSET_H_INCLUDED
#define RIPPLE_APP_MISC_CANONICALTXSET_H_INCLUDED

#include <xrpl/basics/CountedObject.h>
#include <xrpl/protocol/RippleLedgerHash.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SeqProxy.h>

#include <optional>

namespace ripple {

/** Holds transactions which were deferred to the next pass of consensus.

    "Canonical" refers to the order in which transactions are applied.

    - Puts transactions from the same account in SeqProxy order: Sequences,
      then Tickets
    - Then the account's time-sequenced transactions (featureJsonTx), oldest
      Time first

    Without a time-sequenced transaction in the set this is the SeqProxy
    order it always was. A Time on a transaction with a Sequence or a Ticket
    changes nothing: only time-sequenced transactions check or record
    sfLastTxnTime, so oldest first is all their order has to satisfy.

    They go after the Tickets for the reason Tickets go after Sequences. A
    time-sequenced transaction does not say which Sequence it followed, but
    what it does can depend on it - a TicketCreate numbers its Tickets from
    the account Sequence. A transaction with a Sequence or a Ticket that
    really came after it fails terPRE_SEQ or terPRE_TICKET here and is
    retried; one that came before it, applied second, could have its
    Sequence or Ticket taken and fail for good.

*/
// VFALCO TODO rename to SortedTxSet
class CanonicalTXSet : public CountedObject<CanonicalTXSet>
{
private:
    class Key
    {
    public:
        Key(uint256 const& account,
            SeqProxy seqProx,
            std::optional<std::uint64_t> time,
            uint256 const& id)
            : account_(account), txId_(id), seqProxy_(seqProx), time_(time)
        {
        }

        friend bool
        operator<(Key const& lhs, Key const& rhs);

        inline friend bool
        operator>(Key const& lhs, Key const& rhs)
        {
            return rhs < lhs;
        }

        inline friend bool
        operator<=(Key const& lhs, Key const& rhs)
        {
            return !(lhs > rhs);
        }

        inline friend bool
        operator>=(Key const& lhs, Key const& rhs)
        {
            return !(lhs < rhs);
        }

        inline friend bool
        operator==(Key const& lhs, Key const& rhs)
        {
            return lhs.txId_ == rhs.txId_;
        }

        inline friend bool
        operator!=(Key const& lhs, Key const& rhs)
        {
            return !(lhs == rhs);
        }

        uint256 const&
        getAccount() const
        {
            return account_;
        }

        uint256 const&
        getTXID() const
        {
            return txId_;
        }

    private:
        uint256 account_;
        uint256 txId_;
        SeqProxy seqProxy_;
        // sfTime of a time-sequenced transaction, and nothing for any other.
        // Compared before seqProxy_: an empty optional is less than every
        // Time, so time-sequenced transactions come after the rest of their
        // account's, and among themselves oldest first.
        std::optional<std::uint64_t> time_;
    };

    friend bool
    operator<(Key const& lhs, Key const& rhs);

    // Calculate the salted key for the given account
    uint256
    accountKey(AccountID const& account);

    // The Key for tx under the salted account key, id breaking ties
    static Key
    makeKey(uint256 const& account, STTx const& tx, uint256 const& id);

public:
    using const_iterator =
        std::map<Key, std::shared_ptr<STTx const>>::const_iterator;

public:
    explicit CanonicalTXSet(LedgerHash const& saltHash) : salt_(saltHash)
    {
    }

    void
    insert(std::shared_ptr<STTx const> const& txn);

    // Pops the next transaction on account that follows seqProx in the
    // sort order.  Normally called when a transaction is successfully
    // applied to the open ledger so the next transaction can be resubmitted
    // without waiting for ledger close.
    //
    // The return value is often null, when an account has no more
    // transactions.
    std::shared_ptr<STTx const>
    popAcctTransaction(std::shared_ptr<STTx const> const& tx);

    void
    reset(LedgerHash const& salt)
    {
        salt_ = salt;
        map_.clear();
    }

    const_iterator
    erase(const_iterator const& it)
    {
        return map_.erase(it);
    }

    const_iterator
    begin() const
    {
        return map_.begin();
    }

    const_iterator
    end() const
    {
        return map_.end();
    }

    size_t
    size() const
    {
        return map_.size();
    }
    bool
    empty() const
    {
        return map_.empty();
    }

    uint256 const&
    key() const
    {
        return salt_;
    }

private:
    std::map<Key, std::shared_ptr<STTx const>> map_;

    // Used to salt the accounts so people can't mine for low account numbers
    uint256 salt_;
};

}  // namespace ripple

#endif
