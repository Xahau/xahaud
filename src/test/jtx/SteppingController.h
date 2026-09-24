#pragma once
//------------------------------------------------------------------------------
// SteppingController — the single-threaded executor of consensus-critical work
// for the stepping harness (Stage 3). Both consensus JOBS (claimed via per-node
// JobQueue dispatch hooks) and transport DELIVERIES (S3.4) are ENQUEUED onto
// one HarnessScheduler; stepping the scheduler is the ONLY place they run — on
// the stepping thread, clean stack, in deterministic (when, tier, nodeId, seq)
// order.
//
// Why enqueue (never run inline at the post site): a consensus job / delivered
// message run nested inside the caller's stack can re-enter a lock the caller
// still holds (the same-node ricochet: A's job → deliver to B → B replies →
// A's handler re-enters A's lock). Routing every cross-node effect and every
// consensus job through the scheduler lets each node's stack fully unwind
// before any reply/job re-enters it. (Cross-NODE nesting is itself lock-safe —
// distinct ApplicationImp instances have distinct mutexes — but the same-node
// ricochet is not, which is why delivery delay must be strictly positive; see
// scheduleDelivery.)
//
// Clock coherence: the scheduler's now() is the master virtual time. Every
// enqueued event first syncs the injected clocks through the callback MultiNode
// installs, so consensus reads a time consistent with the event order. Per-node
// K horizon mode additionally gives the event owner an observed time of
// global_now - lag(node); the callback receives both times so the shared steady
// clock can stay global while the owner's NetClock reads stale.
//------------------------------------------------------------------------------
#include <test/jtx/HarnessScheduler.h>

#include <xrpld/core/Job.h>        // JobType
#include <xrpld/core/JobQueue.h>   // JobQueue::DispatchHook / JobFunction
#include <xrpl/basics/contract.h>  // Throw
#include <xrpl/beast/core/CurrentThreadName.h>  // diagnostic: name the off-thread culprit

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ripple {
inline constexpr auto JtInvalid = jtINVALID;
inline constexpr auto JtPack = jtPACK;
inline constexpr auto JtPuboldledger = jtPUBOLDLEDGER;
inline constexpr auto JtClient = jtCLIENT;
inline constexpr auto JtClientSubscribe = jtCLIENT_SUBSCRIBE;
inline constexpr auto JtClientFeeChange = jtCLIENT_FEE_CHANGE;
inline constexpr auto JtClientConsensus = jtCLIENT_CONSENSUS;
inline constexpr auto JtClientAcctHist = jtCLIENT_ACCT_HIST;
inline constexpr auto JtClientRpc = jtCLIENT_RPC;
inline constexpr auto JtClientWebsocket = jtCLIENT_WEBSOCKET;
inline constexpr auto JtRpc = jtRPC;
inline constexpr auto JtSweep = jtSWEEP;
inline constexpr auto JtValidationUt = jtVALIDATION_ut;
inline constexpr auto JtManifest = jtMANIFEST;
inline constexpr auto JtUpdatePf = jtUPDATE_PF;
inline constexpr auto JtTransactionL = jtTRANSACTION_l;
inline constexpr auto JtReplayReq = jtREPLAY_REQ;
inline constexpr auto JtLedgerReq = jtLEDGER_REQ;
inline constexpr auto JtProposalUt = jtPROPOSAL_ut;
inline constexpr auto JtReplayTask = jtREPLAY_TASK;
inline constexpr auto JtTransaction = jtTRANSACTION;
inline constexpr auto JtMissingTxn = jtMISSING_TXN;
inline constexpr auto JtRequestedTxn = jtREQUESTED_TXN;
inline constexpr auto JtBatch = jtBATCH;
inline constexpr auto JtLedgerData = jtLEDGER_DATA;
inline constexpr auto JtAdvance = jtADVANCE;
inline constexpr auto JtPubledger = jtPUBLEDGER;
inline constexpr auto JtTxnData = jtTXN_DATA;
inline constexpr auto JtWal = jtWAL;
inline constexpr auto JtValidationT = jtVALIDATION_t;
inline constexpr auto JtWrite = jtWRITE;
inline constexpr auto JtAccept = jtACCEPT;
inline constexpr auto JtProposalT = jtPROPOSAL_t;
inline constexpr auto JtNetopCluster = jtNETOP_CLUSTER;
inline constexpr auto JtAdmin = jtADMIN;
inline constexpr auto JtPeer = jtPEER;
inline constexpr auto JtDisk = jtDISK;
inline constexpr auto JtTxnProc = jtTXN_PROC;
inline constexpr auto JtObSetup = jtOB_SETUP;
inline constexpr auto JtPathFind = jtPATH_FIND;
inline constexpr auto JtHoRead = jtHO_READ;
inline constexpr auto JtHoWrite = jtHO_WRITE;
inline constexpr auto JtGeneric = jtGENERIC;
inline constexpr auto JtNsSyncRead = jtNS_SYNC_READ;
inline constexpr auto JtNsAsyncRead = jtNS_ASYNC_READ;
inline constexpr auto JtNsWrite = jtNS_WRITE;
}  // namespace ripple

namespace ripple::test {

class SteppingController
{
public:
    using Tier = HarnessScheduler::Tier;
    using time_point = HarnessScheduler::time_point;
    using duration = HarnessScheduler::duration;
    using ProfiledPacer = HarnessScheduler::ProfiledPacer;
    using ProfiledStepStats = HarnessScheduler::ProfiledStepStats;

    // The JobQueue hook can run in two modes:
    //   strictKnownOnly  — discovery/regression mode; only the enumerated
    //                      closed-world jobs run, and any other job fails loud.
    //   canonicalAllJobs — S3.7 experiment mode; explicit drops/denies still
    //                      apply, but otherwise the REAL JobQueue closure runs
    //                      as a scheduler event in deterministic order.
    enum class JobPolicy { strictKnownOnly, canonicalAllJobs };

private:
    HarnessScheduler scheduler_;
    JobPolicy jobPolicy_ = JobPolicy::strictKnownOnly;
    // The thread that constructs + steps the controller is the one legal place
    // for consensus-critical work. A hooked job arriving on any other thread is
    // a bug (it would race the single-threaded scheduler); we record it and
    // fall through to workers rather than corrupt the queue.
    std::thread::id const steppingThread_{std::this_thread::get_id()};
    std::atomic<std::size_t> offThreadJobs_{0};
    // Set at teardown (beginDraining): once true, the harness is shutting down
    // and the scheduler has been dropped. Delivery routers consult draining()
    // and DROP residual cross-node completions rather than scheduling them —
    // during the poll-pumped app->run() shutdown a SimPipe::close()/EOF can
    // fire on the io pump thread (off the stepping thread), and the message is
    // irrelevant to a node that is going away. Avoids both the off-thread
    // hard-fail and a pump-vs- shutdown race.
    std::atomic<bool> draining_{false};
    // Nodes that are temporarily stopped during a lifecycle test. Their
    // residual jobs/deliveries are dropped, while the rest of the stepping
    // network keeps running. Access is mutex-protected because shutdown
    // poll-pumps can trigger transport completions off the stepping thread.
    mutable std::mutex inactiveMutex_;
    std::unordered_set<std::uint32_t> inactiveNodes_;
    // Installed by MultiNode: advance injected clocks for the event about to
    // run. `globalNow` is the scheduler's monotonic time; `observedNow` is what
    // the event owner should read. They differ only in per-node K horizon mode.
    // `ownerOnly` tells MultiNode not to refresh other nodes' NetClocks from an
    // event they do not own.
    std::function<void(std::uint32_t, time_point, time_point, bool)> syncClock_;
    struct ProfiledClockSync
    {
        std::uint32_t nodeId = 0;
        time_point observed{};
        bool ownerOnly = false;
    };
    std::optional<ProfiledClockSync> profiledClockSync_;
    mutable std::mutex diagnosticsMutex_;
    std::map<std::string, std::size_t> jobCounts_;
    std::map<std::string, std::size_t> laggedPendingJobs_;
    std::vector<std::string> recentJobs_;
    std::vector<std::string> failedJobs_;
    static constexpr std::size_t maxRecentJobs_ = 40;
    std::map<std::pair<std::uint32_t, int>, duration> jobLags_;
    std::map<std::tuple<std::uint32_t, JobType, std::string>, duration>
        namedJobLags_;
    std::function<void(std::uint32_t, JobType, std::string const&)> beforeJob_;
    // Survives observeJobs replacement. Scenario-wide invariants use this.
    std::function<void(std::uint32_t, JobType, std::string const&)>
        alwaysBeforeJob_;

    [[nodiscard]] static char const*
    jobTypeName(JobType t)
    {
        switch (t)
        {
            case JtInvalid:
                return "JtInvalid";
            case JtPack:
                return "JtPack";
            case JtPuboldledger:
                return "JtPuboldledger";
            case JtClient:
                return "JtClient";
            case JtClientSubscribe:
                return "JtClientSubscribe";
            case JtClientFeeChange:
                return "JtClientFeeChange";
            case JtClientConsensus:
                return "JtClientConsensus";
            case JtClientAcctHist:
                return "JtClientAcctHist";
            case JtClientRpc:
                return "JtClientRpc";
            case JtClientWebsocket:
                return "JtClientWebsocket";
            case JtRpc:
                return "JtRpc";
            case JtSweep:
                return "JtSweep";
            case JtValidationUt:
                return "JtValidationUt";
            case JtManifest:
                return "JtManifest";
            case JtUpdatePf:
                return "JtUpdatePf";
            case JtTransactionL:
                return "JtTransactionL";
            case JtReplayReq:
                return "JtReplayReq";
            case JtLedgerReq:
                return "JtLedgerReq";
            case JtProposalUt:
                return "JtProposalUt";
            case JtReplayTask:
                return "JtReplayTask";
            case JtTransaction:
                return "JtTransaction";
            case JtMissingTxn:
                return "JtMissingTxn";
            case JtRequestedTxn:
                return "JtRequestedTxn";
            case JtBatch:
                return "JtBatch";
            case JtLedgerData:
                return "JtLedgerData";
            case JtAdvance:
                return "JtAdvance";
            case JtPubledger:
                return "JtPubledger";
            case JtTxnData:
                return "JtTxnData";
            case JtWal:
                return "JtWal";
            case JtValidationT:
                return "JtValidationT";
            case JtWrite:
                return "JtWrite";
            case JtAccept:
                return "JtAccept";
            case JtProposalT:
                return "JtProposalT";
            case JtNetopCluster:
                return "JtNetopCluster";
            case jtNETOP_TIMER:
                return "jtNETOP_TIMER";
            case JtAdmin:
                return "JtAdmin";
            case JtPeer:
                return "JtPeer";
            case JtDisk:
                return "JtDisk";
            case JtTxnProc:
                return "JtTxnProc";
            case JtObSetup:
                return "JtObSetup";
            case JtPathFind:
                return "JtPathFind";
            case JtHoRead:
                return "JtHoRead";
            case JtHoWrite:
                return "JtHoWrite";
            case JtGeneric:
                return "JtGeneric";
            case JtNsSyncRead:
                return "JtNsSyncRead";
            case JtNsAsyncRead:
                return "JtNsAsyncRead";
            case JtNsWrite:
                return "JtNsWrite";
            default:
                return "JtUnknown";
        }
    }

    [[nodiscard]] static std::string
    jobLabel(JobType t, std::string const& name)
    {
        return std::string(jobTypeName(t)) + "(jt#" +
            std::to_string(static_cast<int>(t)) + ")/'" + name + "'";
    }

    void
    recordJobLocked(
        std::uint32_t nodeId,
        JobType t,
        std::string const& name,
        char const* action)
    {
        auto const label = std::string(action) + " " + jobLabel(t, name);
        ++jobCounts_[label];
        recentJobs_.push_back("n" + std::to_string(nodeId) + " " + label);
        if (recentJobs_.size() > maxRecentJobs_)
            recentJobs_.erase(recentJobs_.begin());
    }

    void
    recordJob(
        std::uint32_t nodeId,
        JobType t,
        std::string const& name,
        char const* action)
    {
        std::scoped_lock lock(diagnosticsMutex_);
        recordJobLocked(nodeId, t, name, action);
    }

    void
    recordFailedJob(
        std::uint32_t nodeId,
        JobType t,
        std::string const& name,
        char const* reason)
    {
        std::scoped_lock lock(diagnosticsMutex_);
        recordJobLocked(nodeId, t, name, reason);
        failedJobs_.push_back(
            "n" + std::to_string(nodeId) + " " + reason + " " +
            jobLabel(t, name));
    }

    void
    recordLaggedPending(
        std::uint32_t nodeId,
        Tier tier,
        JobType t,
        std::string const& name,
        duration lag)
    {
        std::scoped_lock lock(diagnosticsMutex_);
        auto const label =
            "n" + std::to_string(nodeId) +
            " tier=" + HarnessScheduler::tierName(static_cast<int>(tier)) +
            " +" +
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(lag)
                    .count()) +
            "ms " + jobLabel(t, name);
        ++laggedPendingJobs_[label];
    }

    void
    clearLaggedPending(
        std::uint32_t nodeId,
        Tier tier,
        JobType t,
        std::string const& name,
        duration lag)
    {
        std::scoped_lock lock(diagnosticsMutex_);
        auto const label =
            "n" + std::to_string(nodeId) +
            " tier=" + HarnessScheduler::tierName(static_cast<int>(tier)) +
            " +" +
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(lag)
                    .count()) +
            "ms " + jobLabel(t, name);
        auto it = laggedPendingJobs_.find(label);
        if (it == laggedPendingJobs_.end())
            return;
        if (--it->second == 0)
            laggedPendingJobs_.erase(it);
    }

    void
    clearAllLaggedPending()
    {
        std::scoped_lock lock(diagnosticsMutex_);
        laggedPendingJobs_.clear();
    }

    void
    clearLaggedPendingForNode(std::uint32_t nodeId)
    {
        std::scoped_lock lock(diagnosticsMutex_);
        auto const prefix = "n" + std::to_string(nodeId) + " ";
        for (auto it = laggedPendingJobs_.begin();
             it != laggedPendingJobs_.end();)
        {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
                it = laggedPendingJobs_.erase(it);
            else
                ++it;
        }
    }

    [[nodiscard]] std::string
    jobDiagnosticsLocked() const
    {
        std::ostringstream os;
        os << "jobCounts={";
        char const* sep = "";
        for (auto const& [label, count] : jobCounts_)
        {
            os << sep << label << ":" << count;
            sep = ", ";
        }
        os << "}";
        if (!failedJobs_.empty())
        {
            os << " failedJobs=[";
            sep = "";
            for (auto const& failure : failedJobs_)
            {
                os << sep << failure;
                sep = ", ";
            }
            os << "]";
        }
        if (!laggedPendingJobs_.empty())
        {
            os << " laggedPending=[";
            sep = "";
            for (auto const& [label, count] : laggedPendingJobs_)
            {
                os << sep << label << ":" << count;
                sep = ", ";
            }
            os << "]";
        }
        if (!recentJobs_.empty())
        {
            os << " recentJobs=[";
            sep = "";
            for (auto const& job : recentJobs_)
            {
                os << sep << job;
                sep = ", ";
            }
            os << "]";
        }
        return os.str();
    }

    [[nodiscard]] duration
    jobLag(
        std::uint32_t nodeId,
        Tier tier,
        JobType type,
        std::string const& name) const
    {
        if (auto const it = namedJobLags_.find({nodeId, type, name});
            it != namedJobLags_.end())
            return it->second;
        auto const it = jobLags_.find({nodeId, static_cast<int>(tier)});
        return it == jobLags_.end() ? duration::zero() : it->second;
    }

    void
    throwIfFailedJobs() const
    {
        std::string diagnostics;
        {
            std::scoped_lock lock(diagnosticsMutex_);
            if (failedJobs_.empty())
                return;
            diagnostics = jobDiagnosticsLocked();
        }
        Throw<std::logic_error>(
            "SteppingController: unmodeled job(s) were observed during "
            "stepping. A production catch may have swallowed the original "
            "exception; model/drop/gate the job before trusting this run. " +
            diagnostics);
    }

    // Wrap an enqueued handler so it first brings the clocks up to the event's
    // virtual time (scheduler_.now() is already set to the event's `when` when
    // it runs), then performs the work. Clock coherence is core to the model,
    // so a missing setSyncClock() is a HARD failure once events run (not
    // silently skipped) — consensus would otherwise read an inconsistent time.
    template <class Fn>
    auto
    withClockSync(std::uint32_t nodeId, Fn&& fn)
    {
        return [this, nodeId, fn = std::forward<Fn>(fn)]() {
            if (!syncClock_)
                Throw<std::logic_error>(
                    "SteppingController: setSyncClock() not installed before "
                    "events ran");
            auto const global = scheduler_.now();
            auto const useProfiled =
                profiledClockSync_ && profiledClockSync_->nodeId == nodeId;
            auto const observed =
                useProfiled ? profiledClockSync_->observed : global;
            auto const ownerOnly = useProfiled && profiledClockSync_->ownerOnly;
            syncClock_(nodeId, global, observed, ownerOnly);
            fn();
        };
    }

    // The single-threaded scheduler must only be mutated from the stepping
    // thread. Off-thread access is a bug (it would race the queue) — fail hard.
    void
    requireSteppingThread(char const* what) const
    {
        if (std::this_thread::get_id() != steppingThread_)
            Throw<std::logic_error>(
                std::string("SteppingController: ") + what +
                " off the stepping thread [thread='" +
                std::string(beast::getCurrentThreadName()) + "']");
    }

public:
    SteppingController() = default;
    SteppingController(SteppingController const&) = delete;
    SteppingController&
    operator=(SteppingController const&) = delete;

    [[nodiscard]] HarnessScheduler&
    scheduler()
    {
        return scheduler_;
    }

    [[nodiscard]] time_point
    now() const
    {
        return scheduler_.now();
    }

    [[nodiscard]] bool
    empty() const
    {
        return scheduler_.empty();
    }

    [[nodiscard]] JobPolicy
    jobPolicy() const
    {
        return jobPolicy_;
    }

    void
    setJobPolicy(JobPolicy policy)
    {
        requireSteppingThread("setJobPolicy");
        jobPolicy_ = policy;
    }

    void
    setJobLag(std::uint32_t nodeId, Tier tier, duration lag)
    {
        requireSteppingThread("setJobLag");
        if (lag < duration::zero())
            Throw<std::logic_error>(
                "SteppingController::setJobLag: lag must be non-negative");
        auto const key = std::make_pair(nodeId, static_cast<int>(tier));
        if (lag == duration::zero())
            jobLags_.erase(key);
        else
            jobLags_[key] = lag;
    }

    void
    setJobLag(
        std::uint32_t nodeId,
        JobType type,
        std::string name,
        duration lag)
    {
        requireSteppingThread("setJobLag");
        if (lag < duration::zero())
            Throw<std::logic_error>(
                "SteppingController::setJobLag: lag must be non-negative");
        auto const key = std::make_tuple(nodeId, type, std::move(name));
        if (lag == duration::zero())
            namedJobLags_.erase(key);
        else
            namedJobLags_[key] = lag;
    }

    // Passive scenario inspection after clock sync, immediately before the real
    // job body. This does not enqueue, suppress or replace that body.
    void
    observeJobs(std::function<void(std::uint32_t, JobType, std::string const&)>
                    observer)
    {
        requireSteppingThread("observeJobs");
        beforeJob_ = std::move(observer);
    }

    // Survives observeJobs replacement. Scenario-wide invariants use this.
    void
    setAlwaysBeforeJob(
        std::function<void(std::uint32_t, JobType, std::string const&)>
            observer)
    {
        requireSteppingThread("setAlwaysBeforeJob");
        alwaysBeforeJob_ = std::move(observer);
    }

    void
    clearJobLag(std::uint32_t nodeId)
    {
        requireSteppingThread("clearJobLag");
        for (auto it = jobLags_.begin(); it != jobLags_.end();)
        {
            if (it->first.first == nodeId)
                it = jobLags_.erase(it);
            else
                ++it;
        }
        for (auto it = namedJobLags_.begin(); it != namedJobLags_.end();)
            if (std::get<0>(it->first) == nodeId)
                it = namedJobLags_.erase(it);
            else
                ++it;
    }

    [[nodiscard]] std::size_t
    offThreadJobs() const
    {
        return offThreadJobs_.load(std::memory_order_relaxed);
    }

    // Executed-order fingerprint (see HarnessScheduler::traceFingerprint):
    // equal across two runs ⇔ same events, same total order, same instants.
    [[nodiscard]] std::uint64_t
    traceFingerprint() const
    {
        return scheduler_.traceFingerprint();
    }

    [[nodiscard]] std::uint64_t
    traceCount() const
    {
        return scheduler_.traceCount();
    }

    // Total events ever scheduled (executed or not) — the issue-005 pumpIo
    // guard reads this before/after an io poll to prove the poll enqueued
    // nothing into the deterministic timeline.
    [[nodiscard]] std::uint64_t
    insertionCount() const
    {
        return scheduler_.insertionCount();
    }

    [[nodiscard]] std::size_t
    failedJobs() const
    {
        std::scoped_lock lock(diagnosticsMutex_);
        return failedJobs_.size();
    }

    // Read-only view of currently delayed jobs for one node/type/name.
    // Does not include completed historical jobCounts.
    [[nodiscard]] std::size_t
    laggedPendingJobCount(
        std::uint32_t nodeId,
        JobType type,
        std::string const& name) const
    {
        std::scoped_lock lock(diagnosticsMutex_);
        auto const prefix = "n" + std::to_string(nodeId) + " ";
        auto const needle = jobLabel(type, name);
        std::size_t n = 0;
        for (auto const& [label, count] : laggedPendingJobs_)
        {
            if (label.compare(0, prefix.size(), prefix) == 0 &&
                label.find(needle) != std::string::npos)
                n += count;
        }
        return n;
    }

    [[nodiscard]] std::string
    jobDiagnostics() const
    {
        std::scoped_lock lock(diagnosticsMutex_);
        return jobDiagnosticsLocked();
    }

    void
    setSyncClock(std::function<void(time_point)> f)
    {
        syncClock_ = [f = std::move(f)](
                         std::uint32_t, time_point, time_point t, bool) {
            f(t);
        };
    }

    void
    setSyncClock(std::function<void(std::uint32_t, time_point)> f)
    {
        syncClock_ = [f = std::move(f)](
                         std::uint32_t nodeId, time_point, time_point t, bool) {
            f(nodeId, t);
        };
    }

    void
    setSyncClock(
        std::function<void(std::uint32_t, time_point, time_point, bool)> f)
    {
        syncClock_ = std::move(f);
    }

    // Enter teardown: delivery routers stop scheduling and drop residual
    // completions (see draining_). Idempotent; call before dropPending() + node
    // shutdown. Safe to call from any thread (the flag is atomic).
    void
    beginDraining()
    {
        draining_.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool
    draining() const
    {
        return draining_.load(std::memory_order_relaxed);
    }

    void
    deactivateNode(std::uint32_t nodeId)
    {
        std::scoped_lock lock(inactiveMutex_);
        inactiveNodes_.insert(nodeId);
    }

    void
    activateNode(std::uint32_t nodeId)
    {
        std::scoped_lock lock(inactiveMutex_);
        inactiveNodes_.erase(nodeId);
    }

    [[nodiscard]] bool
    nodeInactive(std::uint32_t nodeId) const
    {
        std::scoped_lock lock(inactiveMutex_);
        return inactiveNodes_.contains(nodeId);
    }

    // How the closed world treats a job: enqueue it onto the scheduler in
    // `tier`, drop it (a harness no-op), or fail (unmodeled → test failure).
    enum class Action { fail, drop, enqueue };
    struct Classification
    {
        Action action = Action::fail;
        Tier tier = Tier::process;  // meaningful only when action == enqueue
    };

    // Classify a job by (type, NAME) — name matters because one JobType can
    // serve several call sites. Enumerated from the S3.3b discovery inventory;
    // extend as the strict run surfaces more.
    [[nodiscard]] static Classification
    classify(JobType t, std::string const& name)
    {
        switch (t)
        {
            case jtNETOP_TIMER:  // "NetOPs.heartbeat" — the consensus heartbeat
                return {Action::enqueue, Tier::heartbeat};
            case JtProposalT:  // "checkPropose"
            case JtProposalUt:
            case JtValidationT:  // "ChkTrust"
            case JtValidationUt:
                return {Action::enqueue, Tier::process};
            case JtManifest:
                return name == "receiveManifests"
                    ? Classification{Action::enqueue, Tier::process}
                    : Classification{Action::fail};
            case JtAccept:  // "AcceptLedger" — deferred ledger build
                return {Action::enqueue, Tier::accept};
            case JtTransaction:
                // "SubmitTxn" (local submission entry) and "RcvCheckTx" (peer
                // receive -> checkTransaction -> processTransaction). Both are
                // per-tx consensus-input work; run in submission order.
                return {Action::enqueue, Tier::process};
            case JtBatch:
                // "TxBatchAsync"/"TxBatchSync" — NetworkOPs::transactionBatch,
                // the open-ledger apply of queued transactions_ (TxQ::apply via
                // OpenLedger::modify). Posted by doTransactionAsync on a
                // receiving node (and by doTransactionSyncBatch when more txs
                // queue mid-apply).
                return {Action::enqueue, Tier::process};
            case JtAdvance:
                // JtAdvance serves several call sites; classify by NAME:
                //   "advanceLedger" — the modeled validated-ledger advance;
                //   "getConsensusLedger1"/"getConsensusLedger2" — the
                //   ledger-ACQUIRE kickoffs
                //     (RCLConsensus::acquireLedger / RCLValidations) — modeled
                //     since the 5.6b late-joiner increment: the job body calls
                //     InboundLedgers::acquireAsync, whose request/response flow
                //     is real peer traffic through the scheduler;
                //   "tryFill" — LedgerMaster history backfill after catch-up.
                // Anything else surfaces as unmodeled (fail).
                if (name == "advanceLedger")
                    return {Action::enqueue, Tier::advance};
                if (name == "getConsensusLedger1" ||
                    name == "getConsensusLedger2" || name == "tryFill")
                    return {Action::enqueue, Tier::process};
                return {Action::fail};
            case JtLedgerReq:
                // Peer-serving reads: "RcvGetLedger" / "RcvGetObjByHash" — a
                // peer asked us for ledger data; we answer from local state.
                return {Action::enqueue, Tier::process};
            case JtLedgerData:
                // The acquire pipeline (5.6b): "ProcessLData" (apply received
                // TMLedgerData), "GotStaleData", "AcqDone" (completion) are
                // data-driven — run in arrival order. "InboundLedger" is the
                // TimeoutCounter RETRY (posted by a virtual Tier::timer
                // expiry via the injected timer factory — the 005 seam;
                // a wall asio timer in production): a timer, not data.
                return name == "InboundLedger"
                    ? Classification{Action::enqueue, Tier::timer}
                    : Classification{Action::enqueue, Tier::process};
            case JtTxnData:
                // The TX-SET acquire pipeline — the tx-set sibling of
                // JtLedgerData's closure graph (§5.1's forecast discovery,
                // flushed by SteppingTxStress's submit-near-close sweep:
                // positions diverge at a close boundary and the receiver
                // acquires the disputed set for real). Classify by NAME —
                // three distinct call sites share the type:
                //   "recvPeerData"  — apply a received liTS_CANDIDATE
                //     TMLedgerData fragment (PeerImp →
                //     InboundTransactions::gotData): data, arrival order;
                //   "completeAcquire" — acquire completion
                //     (TransactionAcquire::done → giveSet feeds consensus):
                //     data, arrival order;
                //   "TransactionAcquire"        — the TransactionAcquire
                //   TimeoutCounter
                //     RETRY (virtual Tier::timer expiry via the injected
                //     timer factory; wall asio in production): a timer,
                //     like "InboundLedger".
                if (name == "recvPeerData" || name == "completeAcquire")
                    return {Action::enqueue, Tier::process};
                if (name == "TransactionAcquire")
                    return {Action::enqueue, Tier::timer};
                return {Action::fail};
            case JtPubledger:
            case JtPuboldledger:
                // "<seq>" (dynamic name — the hygiene-#6 prefix rule):
                // pendSaveValidated's ASYNC save of a validated ledger into
                // the relational DB. The happy path saves synchronously
                // (checkAccept passes isSynchronous=true) — which is why
                // §5.1 never saw this; the history-backfill path posts it
                // async. Must RUN, not drop: tryFill and getHashByIndex
                // read the tables it writes. Reachable only since the
                // backfill subsystem lit up (earliest_seq + the doAdvance
                // progress fix — see issues/open/).
                // xahaud names the save job with the decimal sequence alone.
                if (!name.empty() &&
                    name.find_first_not_of("0123456789") == std::string::npos)
                    return {Action::enqueue, Tier::process};
                return {Action::fail};
            case JtPack:
                // "MakeFetchPack" — a peer asked us for a fetch pack (the
                // history-backfill accelerant); build and reply from local
                // state. Peer-serving read, sibling of "RcvGetLedger".
                // Reachable only since earliest_seq=1 lit up the backfill
                // subsystem
                // (issues/open/003-history-backfill-dark-genesis-worlds.md).
                if (name == "MakeFetchPack")
                    return {Action::enqueue, Tier::process};
                return {Action::fail};
            case JtWal:
                // Longer histories trigger SQLite's passive WAL checkpoint.
                // Run its real deferred closure, including the running_ reset;
                // dropping it would change persistence/teardown behaviour.
                return name == "WAL"
                    ? Classification{Action::enqueue, Tier::process}
                    : Classification{Action::fail};
            case JtClientFeeChange:  // "PubFee"  — fee-change sub notify (no
                                     // subs)
            case JtClientConsensus:  // "PubCons" — consensus-state sub notify
                                     // (no subs)
            case JtUpdatePf:  // "OB3"     — pathfinding update (none here)
                return {Action::drop};
            default:
                return {Action::fail};
        }
    }

    [[nodiscard]] static bool
    deniedInCanonicalMode(JobType t)
    {
        switch (t)
        {
            // Client/RPC/admin/background/special lanes are outside the
            // stepping model unless a focused test explicitly promotes one.
            // They are not part of the peer/consensus/acquire closure graph the
            // S3.7 probe is trying to run.
            case JtClient:
            case JtClientSubscribe:
            case JtClientAcctHist:
            case JtClientRpc:
            case JtClientWebsocket:
            case JtRpc:
            case JtSweep:
            case JtNetopCluster:
            case JtAdmin:
            case JtPeer:
            case JtDisk:
            case JtTxnProc:
            case JtObSetup:
            case JtPathFind:
            case JtHoRead:
            case JtHoWrite:
            case JtGeneric:
            case JtNsSyncRead:
            case JtNsAsyncRead:
            case JtNsWrite:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] Classification
    classifyForPolicy(JobType t, std::string const& name) const
    {
        auto const strict = classify(t, name);
        if (jobPolicy_ == JobPolicy::strictKnownOnly ||
            strict.action != Action::fail)
        {
            return strict;
        }
        if (deniedInCanonicalMode(t))
            return strict;

        // Canonical mode is the scheduler-backed alternate executor: unknown
        // but non-denied jobs run their REAL closures in deterministic order.
        return {Action::enqueue, Tier::process};
    }

    // A JobQueue dispatch hook for node `nodeId` (the alternate executor for
    // stepping mode). Every job is claimed: a scheduler-owned job is ENQUEUED
    // (never run inline, never left to workers); a known harness no-op is
    // DROPPED (reported as not-queued, so a caller that keys a counter off the
    // return value does not leak it); a denied/unmodeled job FAILS HARD. In
    // strictKnownOnly, every non-enumerated job fails. In canonicalAllJobs,
    // non-denied jobs run their real closures on the scheduler. With this
    // installed + zero workers, no app-visible work runs except as a scheduler
    // event.
    [[nodiscard]] JobQueue::DispatchHook
    makeJobHook(std::uint32_t nodeId)
    {
        return [this, nodeId](
                   JobType t,
                   std::string const& name,
                   JobQueue::JobFunction const& f) -> JobQueue::JobDisposition {
            // Teardown: once draining, claim+drop EVERY job. After
            // dropPending() the scheduler is empty and the apps are shutting
            // down; a job posted now (modeled or not, on- or off-thread) must
            // NOT enqueue a new counted closure (would re-arm jobCounter_ and
            // hang JobQueue::stop) and must NOT hard-fail. claimedDropped
            // releases the JobCounter token (addJob returns false). See
            // SteppingController::beginDraining.
            if (draining_.load(std::memory_order_relaxed))
            {
                recordJob(nodeId, t, name, "dropped:draining");
                return JobQueue::JobDisposition::claimedDropped;
            }
            if (nodeInactive(nodeId))
            {
                recordJob(nodeId, t, name, "dropped:inactive");
                return JobQueue::JobDisposition::claimedDropped;
            }
            auto const classification = classifyForPolicy(t, name);
            switch (classification.action)
            {
                case Action::enqueue: {
                    // A scheduler-owned job MUST arrive on the stepping thread;
                    // off thread it would race the scheduler. Record + fail
                    // HARD.
                    if (std::this_thread::get_id() != steppingThread_)
                    {
                        offThreadJobs_.fetch_add(1, std::memory_order_relaxed);
                        recordFailedJob(nodeId, t, name, "failed:offThread");
                        Throw<std::logic_error>(
                            "SteppingController: scheduler-owned job '" + name +
                            "' arrived off the stepping thread [thread='" +
                            std::string(beast::getCurrentThreadName()) + "']");
                    }
                    auto const lag =
                        jobLag(nodeId, classification.tier, t, name);
                    recordJob(
                        nodeId,
                        t,
                        name,
                        lag == duration::zero() ? "queued" : "queued:lagged");
                    if (lag != duration::zero())
                        recordLaggedPending(
                            nodeId, classification.tier, t, name, lag);
                    scheduler_.at(
                        scheduler_.now() + lag,
                        classification.tier,
                        nodeId,
                        withClockSync(
                            nodeId,
                            [this,
                             nodeId,
                             tier = classification.tier,
                             t,
                             name,
                             lag,
                             f]() {
                                if (lag != duration::zero())
                                    clearLaggedPending(
                                        nodeId, tier, t, name, lag);
                                recordJob(nodeId, t, name, "run");
                                if (alwaysBeforeJob_)
                                    alwaysBeforeJob_(nodeId, t, name);
                                if (beforeJob_)
                                    beforeJob_(nodeId, t, name);
                                f();
                            }),
                        HarnessScheduler::Kind::job,
                        // Full provenance (enum name + jt# + posted name) so
                        // the replay ladder names the job exactly; labels are
                        // print-only (never folded into the fingerprint).
                        jobLabel(t, name));
                    return JobQueue::JobDisposition::claimedQueued;
                }
                case Action::drop:
                    // Claimed but won't run; report not-queued (counter-safe).
                    recordJob(nodeId, t, name, "dropped");
                    return JobQueue::JobDisposition::claimedDropped;
                case Action::fail:
                default:
                    recordFailedJob(nodeId, t, name, "failed:unmodeled");
                    Throw<std::logic_error>(
                        "SteppingController: unmodeled job '" + name +
                        "' (jt#" + std::to_string(static_cast<int>(t)) +
                        ") in stepping mode — model it, drop it, or gate its "
                        "source. " +
                        jobDiagnostics());
            }
        };
    }

    // Schedule a transport delivery (S3.4). `delay` MUST be strictly positive
    // so a reply can never be scheduled at the same virtual instant as the
    // message that caused it (that would let the same-node ricochet re-enter
    // within one tick) — a non-positive delay is a bug, not clamped. Must be
    // called on the stepping thread (delivery is triggered from a job/onMessage
    // running there).
    void
    scheduleDelivery(
        std::uint32_t nodeId,
        duration delay,
        std::function<void()> fn,
        std::string label = {})
    {
        if (nodeInactive(nodeId))
            return;
        requireSteppingThread("scheduleDelivery");
        if (delay <= duration::zero())
            Throw<std::logic_error>(
                "SteppingController::scheduleDelivery: delay must be strictly "
                "positive (cross-node delivery cannot be same-instant)");
        scheduler_.in(
            delay,
            Tier::deliver,
            nodeId,
            withClockSync(
                nodeId,
                [this, nodeId, fn = std::move(fn)]() {
                    if (!nodeInactive(nodeId))
                        fn();
                }),
            HarnessScheduler::Kind::deliver,
            std::move(label));
    }

    // Schedule a TimeoutCounter retry expiry (the acquire timer seam, issue
    // 005 / codex round-6): a Tier::timer event owned by nodeId at virtual
    // now + delay. The delay is positive by TimeoutCounter's own interval
    // assert (10ms..30s); a stopped node's pending retries are dropped with
    // the rest of its events. Stepping-thread only — an arm from any other
    // thread is exactly the wall-time escape this seam removes, so the
    // guard IS the fail-loud.
    void
    scheduleTimer(
        std::uint32_t nodeId,
        duration delay,
        std::function<void()> fn,
        std::string label = {})
    {
        if (nodeInactive(nodeId))
            return;
        requireSteppingThread("scheduleTimer");
        scheduler_.in(
            delay,
            Tier::timer,
            nodeId,
            withClockSync(
                nodeId,
                [this, nodeId, fn = std::move(fn)]() {
                    if (!nodeInactive(nodeId))
                        fn();
                }),
            HarnessScheduler::Kind::timer,
            std::move(label));
    }

    // Schedule a HARNESS-DRIVEN event (e.g. a consensus heartbeat tick) at an
    // absolute virtual time, clock-synced like every other scheduler event — so
    // when it runs the nodes' clocks are advanced to `when` before the body.
    // This is how the stepping driver advances virtual time: a heartbeat
    // trigger at a FUTURE `when` moves now() forward when stepped, then runs
    // heartbeatTick() (which posts the heartbeat job the hook re-enqueues at
    // the same instant). Stepping-thread only. Use a strictly-future `when` to
    // advance time.
    void
    scheduleAt(
        time_point when,
        Tier tier,
        std::uint32_t nodeId,
        std::function<void()> fn,
        HarnessScheduler::Kind kind = HarnessScheduler::Kind::other)
    {
        if (nodeInactive(nodeId))
            return;
        requireSteppingThread("scheduleAt");
        scheduler_.at(
            when,
            tier,
            nodeId,
            withClockSync(
                nodeId,
                [this, nodeId, fn = std::move(fn)]() {
                    if (!nodeInactive(nodeId))
                        fn();
                }),
            kind);
    }

    // ── The ONE beat engine every driver builds on (plan §7.9a) ──────────────
    // A "beat" is the atom of driven network time: schedule the DRIVEN SET's
    // heartbeat triggers at `when` (node at position p fires at when + p·skew),
    // plant the time-ownership fence at the horizon `when + dt`, and step until
    // `stop()` holds, the region quiesces (queue empty or next event beyond the
    // horizon), or maxSteps. The fence makes over-reach a THROW, not a silent
    // drain: a beat can only ever execute the timeline it owns. Far-future
    // events (injections, later beats) wait for the beats that own them.
    //
    // `fire(nodeId)` posts one node's heartbeat (MultiNode passes
    // getOPs().heartbeatTick(); driver-laws tests pass plain counters —
    // the engine is deliberately Application-free). Inactive nodes are skipped
    // by scheduleAt as everywhere else. Stepping-thread only.
    struct BeatSpec
    {
        std::vector<std::uint32_t> nodeIds;  // the driven set
        duration dt{std::chrono::seconds{1}};
        duration skew{};  // per-position phase offset; keep p·skew < dt
        bool fireHeartbeats = true;
    };

    std::size_t
    beat(
        time_point when,
        BeatSpec const& spec,
        std::function<void(std::uint32_t)> const& fire,
        std::function<bool()> const& stop,
        std::size_t maxSteps)
    {
        requireSteppingThread("beat");
        if (spec.fireHeartbeats)
        {
            std::size_t pos = 0;
            for (auto const i : spec.nodeIds)
            {
                // Capture `fire` BY VALUE: an early stop() can leave this
                // beat's un-executed heartbeats queued for a later driver to
                // step, long after the caller's `fire` reference is gone.
                scheduleAt(
                    when + spec.skew * static_cast<std::int64_t>(pos++),
                    Tier::heartbeat,
                    i,
                    [fire, i]() { fire(i); },
                    HarnessScheduler::Kind::heartbeat);
            }
        }
        auto const horizon = when + spec.dt;
        // RAII: the fence must lift even if a step throws (failed job audit),
        // or the next driver would inherit a stale horizon.
        struct FenceGuard
        {
            HarnessScheduler& s;
            ~FenceGuard()
            {
                s.clearFence();
            }
        } guard{scheduler_};
        scheduler_.setFence(horizon);
        return stepUntil(
            [this, &stop, horizon]() {
                return (stop && stop()) || scheduler_.empty() ||
                    scheduler_.nextWhen() > horizon;
            },
            maxSteps);
    }

    // K-profiled variant of beat(): same absolute heartbeat grid and same
    // horizon fence as beat(), but each executed event is charged a uniform
    // virtual duration before its handler runs. This is coarse event-count
    // pressure, not a work-proportional timing model.
    std::size_t
    profiledBeat(
        time_point when,
        BeatSpec const& spec,
        ProfiledPacer const& pacer,
        ProfiledStepStats& stats,
        std::function<void(std::uint32_t)> const& fire,
        std::function<bool()> const& stop,
        std::size_t maxSteps)
    {
        requireSteppingThread("profiledBeat");
        if (!pacer.enabled())
            return beat(when, spec, fire, stop, maxSteps);

        if (spec.fireHeartbeats)
        {
            std::size_t pos = 0;
            for (auto const i : spec.nodeIds)
            {
                scheduleAt(
                    when + spec.skew * static_cast<std::int64_t>(pos++),
                    Tier::heartbeat,
                    i,
                    [fire, i]() { fire(i); },
                    HarnessScheduler::Kind::heartbeat);
            }
        }

        auto const horizon = when + spec.dt;
        struct FenceGuard
        {
            HarnessScheduler& s;
            ~FenceGuard()
            {
                s.clearFence();
            }
        } guard{scheduler_};
        scheduler_.setFence(horizon);

        std::size_t n = 0;
        auto const clampsBefore = stats.clampHits;
        auto const setProfiledClock = [this,
                                       ownerOnly = pacer.horizonMode ==
                                           ProfiledPacer::HorizonMode::perNode](
                                          std::uint32_t nodeId,
                                          time_point observed) {
            profiledClockSync_ = ProfiledClockSync{nodeId, observed, ownerOnly};
        };
        while (n < maxSteps)
        {
            if ((stop && stop()) || scheduler_.empty() ||
                scheduler_.nextWhen() > horizon)
                break;
            bool ran = false;
            try
            {
                ran = scheduler_.stepOneProfiled(
                    horizon, pacer, stats, setProfiledClock);
            }
            catch (...)
            {
                profiledClockSync_.reset();
                throw;
            }
            profiledClockSync_.reset();
            if (!ran)
                break;
            ++n;
            throwIfFailedJobs();
            if (pacer.horizonMode == ProfiledPacer::HorizonMode::global &&
                stats.clampHits != clampsBefore)
                break;
        }
        return n;
    }

    // Step every event with when <= `until` — a fenced, horizon-bounded settle
    // (quiet gaps, sever-cascade flushes). Never executes beyond `until`; far-
    // future events wait for their owners. Stepping-thread only.
    std::size_t
    stepUntilTime(time_point until, std::size_t maxSteps)
    {
        requireSteppingThread("stepUntilTime");
        struct FenceGuard
        {
            HarnessScheduler& s;
            ~FenceGuard()
            {
                s.clearFence();
            }
        } guard{scheduler_};
        scheduler_.setFence(until);
        return stepUntil(
            [this, until]() {
                return scheduler_.empty() || scheduler_.nextWhen() > until;
            },
            maxSteps);
    }

    /** Settle due work, then advance all clocks to a quiet-gap boundary.

        This is a controller-owned clock transition, independent of node
        liveness. A depleted step budget throws before skipping pending work.
    */
    void
    advanceTimeTo(time_point until, std::size_t maxSteps)
    {
        requireSteppingThread("advanceTimeTo");
        if (until < now())
            Throw<std::logic_error>(
                "SteppingController::advanceTimeTo: boundary is in the past");
        if (!syncClock_)
            Throw<std::logic_error>(
                "SteppingController::advanceTimeTo: setSyncClock() not "
                "installed");

        stepUntilTime(until, maxSteps);
        if (!scheduler_.empty() && scheduler_.nextWhen() <= until)
            Throw<std::logic_error>(
                "SteppingController::advanceTimeTo: step budget exhausted");

        // No due work remains. stepUntil now advances only the scheduler clock;
        // ownerOnly=false tells the callback to synchronize every live node.
        scheduler_.stepUntil(until);
        syncClock_(/*owner=*/0, until, until, /*ownerOnly=*/false);
    }

    // Run the next single event (advancing virtual time to it). False if empty.
    bool
    stepOne()
    {
        bool const ran = scheduler_.stepOne();
        if (ran)
            throwIfFailedJobs();
        return ran;
    }

    // Step until pred() holds or the queue empties or maxSteps is reached.
    // Returns the number of events run. Quiescence is implicit: an empty queue
    // means the message/job cascade has fully settled (no drain heuristic).
    template <class Pred>
    std::size_t
    stepUntil(Pred&& pred, std::size_t maxSteps)
    {
        std::size_t n = 0;
        while (n < maxSteps && !pred() && stepOne())
            ++n;
        return n;
    }

    // Drain the queue completely (or until maxSteps). Returns events run.
    std::size_t
    drain(std::size_t maxSteps)
    {
        std::size_t n = 0;
        while (n < maxSteps && stepOne())
            ++n;
        return n;
    }

    // Drop all pending events WITHOUT running them, destroying the closures
    // they hold. CRITICAL for teardown: a hook-claimed job's counted closure
    // lives in its scheduler event, and JobQueue::stop() blocks on
    // jobCounter_.join() until every such closure is destroyed — so the harness
    // MUST dropPending() before shutting the apps down, or shutdown hangs.
    // Stepping-thread only.
    void
    dropPending()
    {
        requireSteppingThread("dropPending");
        scheduler_.clear();
        clearAllLaggedPending();
    }

    // Drop pending events for one stopped node while preserving the rest of the
    // network's queued work. This is distinct from teardown's dropPending().
    std::size_t
    dropPendingForNode(std::uint32_t nodeId)
    {
        requireSteppingThread("dropPendingForNode");
        auto const removed = scheduler_.clearNode(nodeId);
        clearLaggedPendingForNode(nodeId);
        return removed;
    }
};

}  // namespace ripple::test
