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

#include <xrpld/app/ledger/detail/TimeoutCounter.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/overlay/Overlay.h>
#include <boost/asio/error.hpp>

namespace ripple {

using namespace std::chrono_literals;

namespace {

class AsioTimeoutCounterTimer : public TimeoutCounterTimer
{
public:
    explicit AsioTimeoutCounterTimer(boost::asio::io_service& io) : timer_(io)
    {
    }

    void
    expiresAfter(
        std::chrono::milliseconds interval,
        std::function<void()> handler) override
    {
        timer_.expires_after(interval);
        timer_.async_wait(
            [h = std::move(handler)](boost::system::error_code const& ec) {
                if (ec == boost::asio::error::operation_aborted)
                    return;
                h();
            });
    }

    void
    cancel() override
    {
        try
        {
            timer_.cancel();
        }
        catch (boost::system::system_error const&)
        {
        }
    }

private:
    boost::asio::basic_waitable_timer<std::chrono::steady_clock> timer_;
};

}  // namespace

std::unique_ptr<TimeoutCounterTimer>
makeAsioTimeoutCounterTimer(boost::asio::io_service& io)
{
    return std::make_unique<AsioTimeoutCounterTimer>(io);
}

TimeoutCounter::TimeoutCounter(
    Application& app,
    uint256 const& hash,
    std::chrono::milliseconds interval,
    QueueJobParameter&& jobParameter,
    beast::Journal journal)
    : app_(app)
    , journal_(journal)
    , hash_(hash)
    , timeouts_(0)
    , complete_(false)
    , failed_(false)
    , progress_(false)
    , timerInterval_(interval)
    , queueJobParameter_(std::move(jobParameter))
    , timer_(app_.makeTimeoutCounterTimer())
{
    XRPL_ASSERT(
        (timerInterval_ > 10ms) && (timerInterval_ < 30s),
        "ripple::TimeoutCounter::TimeoutCounter : interval input inside range");
}

void
TimeoutCounter::setTimer(ScopedLockType& sl)
{
    if (isDone())
        return;
    timer_->expiresAfter(timerInterval_, [wptr = pmDowncast()]() {
        if (auto ptr = wptr.lock())
        {
            ScopedLockType sl(ptr->mtx_);
            ptr->queueJob(sl);
        }
    });
}

void
TimeoutCounter::queueJob(ScopedLockType& sl)
{
    if (isDone())
        return;
    if (queueJobParameter_.jobLimit &&
        app_.getJobQueue().getJobCountTotal(queueJobParameter_.jobType) >=
            queueJobParameter_.jobLimit)
    {
        JLOG(journal_.debug()) << "Deferring " << queueJobParameter_.jobName
                               << " timer due to load";
        setTimer(sl);
        return;
    }

    app_.getJobQueue().addJob(
        queueJobParameter_.jobType,
        queueJobParameter_.jobName,
        [wptr = pmDowncast()]() {
            if (auto sptr = wptr.lock(); sptr)
                sptr->invokeOnTimer();
        });
}

void
TimeoutCounter::invokeOnTimer()
{
    ScopedLockType sl(mtx_);

    if (isDone())
        return;

    if (!progress_)
    {
        ++timeouts_;
        JLOG(journal_.debug())
            << "Timeout(" << timeouts_ << ") " << " acquiring " << hash_;
        onTimer(false, sl);
    }
    else
    {
        progress_ = false;
        onTimer(true, sl);
    }

    if (!isDone())
        setTimer(sl);
}

void
TimeoutCounter::cancel()
{
    ScopedLockType sl(mtx_);
    if (!isDone())
    {
        failed_ = true;
        JLOG(journal_.info()) << "Cancel " << hash_;
    }
}

}  // namespace ripple
