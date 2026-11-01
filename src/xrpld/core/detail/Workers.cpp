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

#include <xrpld/core/detail/Workers.h>
#include <xrpl/beast/core/CurrentThreadName.h>
#include <xrpl/beast/utility/instrumentation.h>

#include <chrono>
#include <thread>

namespace ripple {

Workers::Workers(
    Callback& callback,
    std::string const& name,
    unsigned int count)
    : callback_(callback)
{
    assert(count != 0);

    // It is important that head and tail are equal but non-zero on
    // startup: we use zero as a special value in head_ to indicate that
    // the threads must terminate.
    assert(head_ == tail_ && head_ != 0);

    while (count--)
    {
        // We need to increment this outside of the thread, to avoid a
        // subtle (if unlikely) race condition during shutdown.
        ++threads_;

        std::thread th(
            [this, name](unsigned int instance) {
                auto const n = name + ":" + std::to_string(instance);

                while (head_)
                {
                    auto t = tail_.load();
                    auto h = head_.load();

                    assert((h == 0) || (h >= t));

                    if (h == t)
                    {
                        // Only pay for the thread rename when we are
                        // actually about to go to sleep.
                        beast::setCurrentThreadName(n + " [zZz]");

                        // Announce that we're about to sleep *before*
                        // re-checking the counters. Both this increment and
                        // the head_ CAS in addTask are seq_cst, so either we
                        // observe the new task here, or addTask observes
                        // sleepers_ != 0 and wakes us. No lost wakeups.
                        std::unique_lock lock(sleepMutex_);
                        ++sleepers_;
                        sleepCond_.wait(lock, [this, &t, &h] {
                            t = tail_.load();
                            h = head_.load();
                            return h != t;
                        });
                        --sleepers_;
                    }

                    // As long as we aren't stopping and there is a task
                    // that's waiting, we try to do work.
                    if (h > t && tail_.compare_exchange_strong(t, t + 1))
                    {
                        // Restore the name, in case the previous callback
                        // had changed it.
                        beast::setCurrentThreadName(n);

                        // Exceptions should never bubble up to here but
                        // just in case, if one does catch it.
                        try
                        {
                            callback_.processTask(instance);
                        }
                        catch (...)
                        {
                            callback_.uncaughtException(
                                instance, std::current_exception());
                        }
                    }
                }

                // Track number of threads
                --threads_;
            },
            count);

        // We detach from this thread; it will continue to run in the
        // background until we instruct it to stop.
        th.detach();
    }
}

Workers::~Workers()
{
    stop();

    assert(head_.load() == 0 && threads_.load() == 0);
}

void
Workers::stop()
{
    head_.store(0);

    // Taking the lock orders the store above against any thread that is
    // between checking its predicate and blocking.
    {
        std::lock_guard lock(sleepMutex_);
    }
    sleepCond_.notify_all();

    while (threads_.load() != 0)
        std::this_thread::yield();
}

void
Workers::addTask()
{
    auto h = head_.load();

    while (h != 0)
    {
        if (head_.compare_exchange_weak(h, h + 1))
        {
            // Fast path: if nobody is parked, someone is already awake and
            // will pick this task up when it next checks the counters.
            if (sleepers_.load() != 0)
            {
                {
                    std::lock_guard lock(sleepMutex_);
                }
                sleepCond_.notify_one();
            }
            return;
        }
    }
}

}  // namespace ripple
