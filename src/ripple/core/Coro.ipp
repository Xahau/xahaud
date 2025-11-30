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

#ifndef RIPPLE_CORE_COROINL_H_INCLUDED
#define RIPPLE_CORE_COROINL_H_INCLUDED

#include <ripple/basics/ByteUtilities.h>
#include <thread>

namespace ripple {

template <class F>
JobQueue::Coro::Coro(
    Coro_create_t,
    JobQueue& jq,
    JobType type,
    std::string const& name,
    F&& f)
    : jq_(jq)
    , type_(type)
    , name_(name)
    , running_(false)
    , coro_(
          [this, fn = std::forward<F>(f)](
              boost::coroutines::asymmetric_coroutine<void>::push_type&
                  do_yield) {
              yield_ = &do_yield;
              yield();
              fn(shared_from_this());
#ifndef NDEBUG
              finished_ = true;
#endif
          },
          boost::coroutines::attributes(megabytes(1)))
{
    lvs_.coroPtr = this;
}

inline JobQueue::Coro::~Coro()
{
#ifndef NDEBUG
    assert(finished_);
#endif
}

//@@start coro-yield
inline void
JobQueue::Coro::yield() const
{
    {
        std::lock_guard lock(jq_.m_mutex);
        ++jq_.nSuspend_;
    }
    (*yield_)();
}
//@@end coro-yield

inline bool
JobQueue::Coro::post()
{
    {
        std::lock_guard lk(mutex_run_);
        running_ = true;
    }

    // sp keeps 'this' alive
    if (jq_.addJob(
            type_, name_, [this, sp = shared_from_this()]() { resume(); }))
    {
        return true;
    }

    // The coroutine will not run.  Clean up running_.
    std::lock_guard lk(mutex_run_);
    running_ = false;
    cv_.notify_all();
    return false;
}

//@@start coro-resume
inline void
JobQueue::Coro::resume()
{
    {
        std::lock_guard lk(mutex_run_);
        running_ = true;
    }
    {
        std::lock_guard lock(jq_.m_mutex);
        --jq_.nSuspend_;
    }
    auto saved = detail::getLocalValues().release();
    detail::getLocalValues().reset(&lvs_);
    std::lock_guard lock(mutex_);
    assert(coro_);
    coro_();
    detail::getLocalValues().release();
    detail::getLocalValues().reset(saved);
    std::lock_guard lk(mutex_run_);
    running_ = false;
    cv_.notify_all();
}
//@@end coro-resume

inline bool
JobQueue::Coro::runnable() const
{
    return static_cast<bool>(coro_);
}

inline void
JobQueue::Coro::expectEarlyExit()
{
#ifndef NDEBUG
    if (!finished_)
#endif
    {
        // expectEarlyExit() must only ever be called from outside the
        // Coro's stack.  It you're inside the stack you can simply return
        // and be done.
        //
        // That said, since we're outside the Coro's stack, we need to
        // decrement the nSuspend that the Coro's call to yield caused.
        std::lock_guard lock(jq_.m_mutex);
        --jq_.nSuspend_;
#ifndef NDEBUG
        finished_ = true;
#endif
    }
}

inline void
JobQueue::Coro::join()
{
    std::unique_lock<std::mutex> lk(mutex_run_);
    cv_.wait(lk, [this]() { return running_ == false; });
}

inline bool
JobQueue::Coro::postAndYield()
{
    {
        std::lock_guard lk(mutex_run_);
        running_ = true;
    }

    // Flag starts false - will be set true right before yield()
    yielding_.store(false, std::memory_order_release);

    // Post a job that waits for yield to be ready, then resumes
    if (!jq_.addJob(
            type_, name_, [this, sp = shared_from_this()]() {
                // Spin-wait until yield() is about to happen
                // yielding_ is set true immediately before (*yield_)() is called
                while (!yielding_.load(std::memory_order_acquire))
                    std::this_thread::yield();
                resume();
            }))
    {
        std::lock_guard lk(mutex_run_);
        running_ = false;
        cv_.notify_all();
        return false;
    }

    // Signal that we're about to yield, then yield
    yielding_.store(true, std::memory_order_release);
    yield();

    // Clear flag after resuming
    yielding_.store(false, std::memory_order_release);
    return true;
}

}  // namespace ripple

#endif
