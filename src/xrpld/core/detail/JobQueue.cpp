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

#include <xrpld/core/JobQueue.h>
#include <xrpld/perflog/PerfLog.h>
#include <xrpl/basics/contract.h>
#include <xrpl/beast/core/CurrentThreadName.h>

#include <mutex>
#include <utility>

namespace ripple {

JobQueue::JobQueue(
    int threadCount,
    beast::insight::Collector::ptr const& collector,
    beast::Journal journal,
    Logs& logs,
    perf::PerfLog& perfLog)
    : journal_(journal)
    , data_([&]<std::size_t... I>(std::index_sequence<I...>) {
        return std::array<Data, sizeof...(I)>{
            Data{jobTypes[I], collector, logs}...};
    }(std::make_index_sequence<jobTypes.size()>()))
    , workers_(*this, "JobQueue", threadCount)
    , perfLog_(perfLog)
    , collector_(collector)
{
    JLOG(journal_.info()) << "Using " << threadCount << " threads";

    // This is important to do before dispatching any jobs
    // so that the logger knows the number of threads.
    perfLog_.resizeJobs(threadCount);

    hook_ = collector_->make_hook([this] { collect(); });
    jobCountGauge_ = collector_->make_gauge("job_count");
}

JobQueue::~JobQueue()
{
    // Must unhook before destroying
    hook_ = beast::insight::Hook();
}

void
JobQueue::collect()
{
    std::lock_guard lock(mutex_);
    jobCountGauge_ = jobSet_.size();
    activeThreadsGauge_ = activeThreads_;
}

bool
JobQueue::addRefCountedJob(
    JobType type,
    std::string const& name,
    JobFunction func)
{
    JLOG(journal_.debug()) << "Adding job '" << name << "' (" << type << ")";

    // FIXME: Workaround incorrect client shutdown ordering
    // do not add jobs to a queue with no threads
    XRPL_ASSERT(
        (type >= jtCLIENT && type <= jtCLIENT_WEBSOCKET) ||
            workers_.count() > 0,
        "ripple::JobQueue::addRefCountedJob : threads available or job "
        "requires no threads");

    std::lock_guard lock(mutex_);

    auto& d = data_[type];

    auto r = jobSet_.emplace(
        type, name, ++nextId_, d.load.sample(), std::move(func));
    assert(r.second && jobSet_.find(*r.first) != jobSet_.end());

    perfLog_.jobQueue(type);

    if (d.waiting + d.running < jobTypes[type].limit)
        workers_.addTask();
    else
        ++d.deferred;

    ++d.waiting;

    return true;
}

int
JobQueue::getJobCount(JobType t) const
{
    std::lock_guard lock(mutex_);
    return data_[t].waiting;
}

int
JobQueue::getJobCountTotal(JobType t) const
{
    std::lock_guard lock(mutex_);
    auto const& d = data_[t];
    return d.waiting + d.running;
}

int
JobQueue::getJobCountGE(JobType t) const
{
    int ret = 0;

    std::lock_guard lock(mutex_);

    for (std::size_t i = 0; i < data_.size(); ++i)
    {
        if (static_cast<JobType>(i) >= t)
            ret += data_[i].waiting;
    }

    return ret;
}

LoadEvent
JobQueue::createLoadEvent(JobType t, std::string name)
{
    return {data_[t].load.sample(), std::move(name), true};
}

void
JobQueue::addLoadEvents(JobType t, int count, std::chrono::milliseconds elapsed)
{
    if (!isStopped()) [[likely]]
        data_[t].load.addSamples(count, elapsed);
}

bool
JobQueue::isOverloaded()
{
    return std::any_of(
        data_.begin(), data_.end(), [](auto& d) { return d.load.isOver(); });
}

Json::Value
JobQueue::getJson(int)
{
    using namespace std::chrono_literals;
    Json::Value ret(Json::objectValue);

    ret["threads"] = workers_.count();
    ret["coro.suspended"] = suspendedCoroutines_.load();
    ret["coro.total"] = totalCoroutines_.load();

    Json::Value priorities = Json::arrayValue;

    std::lock_guard lock(mutex_);

    for (std::size_t i = 0; i < data_.size(); ++i)
    {
        auto const type = static_cast<JobType>(i);

        if (type == jtGENERIC)
            continue;

        auto& d = data_[i];
        auto const s = d.load.getStats();

        if (s.count || d.waiting || d.running || (s.peakLatency != 0ms))
        {
            Json::Value& pri = priorities.append(Json::objectValue);

            pri["job_type"] = std::string(jobTypes[i].name);

            if (s.isOverloaded)
                pri["over_target"] = true;

            if (d.waiting != 0)
                pri["waiting"] = d.waiting;

            if (s.count != 0)
                pri["per_second"] = static_cast<int>(s.count);

            if (s.peakLatency != 0ms)
                pri["peak_time"] = static_cast<int>(s.peakLatency.count());

            if (s.averageLatency != 0ms)
                pri["avg_time"] = static_cast<int>(s.averageLatency.count());

            if (d.running != 0)
                pri["in_progress"] = d.running;
        }
    }

    ret["job_types"] = priorities;

    return ret;
}

void
JobQueue::rendezvous()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return activeThreads_ == 0 && jobSet_.empty(); });
}

void
JobQueue::stop()
{
    if (state expected = state::running; !state_.compare_exchange_strong(
            expected, state::stopping, std::memory_order_acq_rel))
    {
        while (state_.load(std::memory_order_acquire) != state::stopped)
            std::this_thread::yield();
        return;
    }

    jobCounter_.join("JobQueue", std::chrono::seconds(1), journal_);

    // All jobs have finished executing (i.e. returned from `Job::doJob`) and
    // no more are being accepted, but there may still be some threads between
    // the return of `Job::doJob` and the return of `JobQueue::processTask`.
    // That is why we must wait on the condition variable to make these
    // assertions.
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return activeThreads_ == 0 && jobSet_.empty(); });

    XRPL_ASSERT(
        activeThreads_ == 0,
        "ripple::JobQueue::stop : all processes completed");
    XRPL_ASSERT(jobSet_.empty(), "ripple::JobQueue::stop : all jobs completed");
    XRPL_ASSERT(
        suspendedCoroutines_ == 0,
        "ripple::JobQueue::stop : no coros suspended");

    state_.store(state::stopped, std::memory_order_release);
}

void
JobQueue::uncaughtException(unsigned int instance, std::exception_ptr eptr)
{
    try
    {
        if (eptr)
            std::rethrow_exception(eptr);

        LogicError(
            beast::getCurrentThreadName() +
            ": Uncaught exception handler invoked with no exception_ptr");
    }
    catch (std::exception const& e)
    {
        LogicError(
            beast::getCurrentThreadName() +
            ": Exception caught during task processing: " + e.what());
    }
    catch (...)
    {
        LogicError(
            beast::getCurrentThreadName() +
            ": Unknown exception caught during task processing");
    }
}

void
JobQueue::processTask(unsigned int instance)
{
    JobType type;

    {
        Job job = [this] {
            std::lock_guard lock(mutex_);

            assert(!jobSet_.empty());

            for (auto iter = jobSet_.begin(); iter != jobSet_.end(); ++iter)
            {
                JobType const t = iter->getType();

                auto& d = data_[t];
                assert(d.running <= jobTypes[t].limit);

                if (d.running < jobTypes[t].limit)
                {
                    assert(d.waiting > 0);
                    --d.waiting;
                    ++d.running;
                    ++activeThreads_;
                    return std::move(jobSet_.extract(*iter).value());
                }
            }

            LogicError("Attempt to get a job when none are available");
        }();

        type = job.getType();

        using namespace std::chrono;

        auto const start_time = Job::clock_type::now();
        auto const q_time = ceil<microseconds>(start_time - job.queue_time());
        perfLog_.jobStart(type, q_time, start_time, instance);

        JLOG(journal_.trace())
            << "Starting: " << jobTypes[type].name
            << " (q_time=" << q_time.count() << " microseconds)";

        job.execute();

        auto const x_time =
            ceil<microseconds>(Job::clock_type::now() - start_time);

        JLOG(journal_.trace())
            << "Finished: " << jobTypes[type].name
            << " (x_time=" << x_time.count() << " microseconds)";

        if (x_time >= 10ms || q_time >= 10ms)
        {
            data_[type].dequeue.notify(q_time);
            data_[type].execute.notify(x_time);
        }

        perfLog_.jobFinish(type, x_time, instance);

        // Note that the Job object gets destroyed at this point.
    }

    std::lock_guard lock(mutex_);

    auto& d = data_[type];

    // Queue a deferred task if possible. This doesn't mean the task will
    // run next; only that one is now runnable.
    if (d.deferred > 0)
    {
        assert(d.running + d.waiting >= jobTypes[type].limit);
        --d.deferred;
        workers_.addTask();
    }

    --d.running;

    if (--activeThreads_ == 0 && jobSet_.empty())
        cv_.notify_all();
}

}  // namespace ripple
