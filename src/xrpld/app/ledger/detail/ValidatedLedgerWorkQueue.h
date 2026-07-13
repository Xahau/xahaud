//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPLF

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_APP_LEDGER_DETAIL_VALIDATEDLEDGERWORKQUEUE_H_INCLUDED
#define RIPPLE_APP_LEDGER_DETAIL_VALIDATEDLEDGERWORKQUEUE_H_INCLUDED

#include <xrpl/basics/contract.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/RippleLedgerHash.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace ripple::detail {

struct ValidatedLedgerWork
{
    LedgerIndex seq;
    LedgerHash hash;

    friend bool
    operator==(ValidatedLedgerWork const&, ValidatedLedgerWork const&) =
        default;
};

/** A bounded, exact-key queue for work unlocked by ledger validation.

    The queue owns only scheduling identity. Consumers are responsible for
    looking up any ledger or feature-specific state after the item is drained.
*/
class ValidatedLedgerWorkQueue
{
public:
    struct EnqueueResult
    {
        bool inserted;
        bool needsDrain;
        std::optional<ValidatedLedgerWork> evicted;
    };

    explicit ValidatedLedgerWorkQueue(std::size_t capacity)
        : capacity_(capacity)
    {
        XRPL_ASSERT(
            capacity_ > 0,
            "ripple::detail::ValidatedLedgerWorkQueue : positive capacity");
    }

    EnqueueResult
    enqueue(ValidatedLedgerWork work)
    {
        std::lock_guard lock(mutex_);

        if ((active_ && *active_ == work) ||
            std::find(queue_.begin(), queue_.end(), work) != queue_.end())
            return {false, false, std::nullopt};

        std::optional<ValidatedLedgerWork> evicted;
        if (queue_.size() == capacity_)
        {
            evicted = std::move(queue_.front());
            queue_.pop_front();
        }

        queue_.push_back(std::move(work));
        bool const needsDrain = !drainScheduled_;
        drainScheduled_ = true;
        return {true, needsDrain, std::move(evicted)};
    }

    /** Drain queued items in FIFO order.

        The consumer runs without the queue mutex. It must not throw; future
        consumers should catch feature-specific failures within the callback.
    */
    template <class Consumer>
    std::size_t
    drain(Consumer&& consumer)
    {
        static_assert(
            std::is_nothrow_invocable_v<Consumer&, ValidatedLedgerWork const&>);

        std::size_t drained = 0;
        for (;;)
        {
            std::optional<ValidatedLedgerWork> work;
            {
                std::lock_guard lock(mutex_);
                active_.reset();
                if (queue_.empty())
                {
                    drainScheduled_ = false;
                    return drained;
                }

                work = std::move(queue_.front());
                queue_.pop_front();
                active_ = *work;
            }

            std::invoke(consumer, *work);
            ++drained;
        }
    }

    /** Re-arm scheduling if posting the drain job failed. */
    void
    cancelDrain()
    {
        std::lock_guard lock(mutex_);
        XRPL_ASSERT(
            !active_,
            "ripple::detail::ValidatedLedgerWorkQueue::cancelDrain : no "
            "active consumer");
        drainScheduled_ = false;
    }

    std::size_t
    size() const
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    bool
    drainScheduled() const
    {
        std::lock_guard lock(mutex_);
        return drainScheduled_;
    }

private:
    std::size_t const capacity_;
    mutable std::mutex mutex_;
    std::deque<ValidatedLedgerWork> queue_;
    std::optional<ValidatedLedgerWork> active_;
    bool drainScheduled_{false};
};

}  // namespace ripple::detail

#endif
