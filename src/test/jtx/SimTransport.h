#pragma once
//------------------------------------------------------------------------------
// SimTransport — the in-process sim implementation of ripple::Transport (the
// §5.1 seam). It replaces the production ssl_stream with a deterministic byte
// pipe so the REAL PeerImp can run with no TCP/TLS. Two endpoints share a
// SimWire: bytes written by one endpoint become readable by the other, and vice
// versa.
//
// Completions are ALWAYS posted on the supplied executor (never invoked
// inline), matching asio's contract, and the shared pipe state is mutex-guarded
// because the two endpoints live on different nodes' io_contexts (different
// threads).
//
// Stage 1 (spec §6/§7): paired with SimOverlay, this lets real PeerImps
// exchange protocol messages over the bus. makeSharedValue() returns a per-wire
// constant so both ends agree on the handshake shared value (prod derives it
// from the TLS session; buildHandshake/verifyHandshake are reused verbatim).
//------------------------------------------------------------------------------
#include <xrpld/overlay/detail/ProtocolMessage.h>
#include <xrpld/overlay/detail/Transport.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/contract.h>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ripple::test {

// ── §5.7 wire-altitude fault injection ────────────────────────────────────
// One message's fate, decided at WRITE time by a per-direction injector on
// the pipe. The seam lives at the write boundary because one
// Transport::async_write is exactly one framed protocol message on this base
// (PeerImp drains sendQueue_ one message per write, and simConnect peers
// never stream handshake bytes) — so drop/duplicate/delay need no byte
// parsing and the stream stays frame-valid under any combination (whole
// messages only). This models a byzantine NETWORK: it cannot forge
// signatures, which is the right default fidelity (design-notes §6). There
// is deliberately no `corrupt`: under TLS, corruption manifests as
// connection failure, and sever() already models that first-class.
// Default (no injector) → byte-identical behavior.
struct SimFault
{
    bool drop = false;   // don't deliver the original
    int duplicates = 0;  // extra copies delivered after the original
    // Extra in-flight latency for this message and its copies: the bytes
    // enter the pipe at now()+delay (scheduler-routed, so whole messages may
    // REORDER — that's the point), then deliver at +linkDelay as usual.
    // Stepping mode only (the delay rides the scheduler).
    std::chrono::steady_clock::duration delay{};
};

// Peek the 2-byte protocol message type from a framed message: bytes [4..5]
// in BOTH the compressed and uncompressed headers (ProtocolMessage.h).
[[nodiscard]] inline std::uint16_t
peekMessageType(std::vector<std::uint8_t> const& bytes)
{
    if (bytes.size() < 6)
        return 0;
    return static_cast<std::uint16_t>((bytes[4] << 8) | bytes[5]);
}

enum class SimTransportPostKind { read, write, shutdown };

struct SimTransportActivitySnapshot
{
    std::uint64_t inFlightPosts = 0;
    std::uint64_t epoch = 0;
    std::uint64_t readStarted = 0;
    std::uint64_t writeStarted = 0;
    std::uint64_t shutdownStarted = 0;
    std::uint64_t readFinished = 0;
    std::uint64_t writeFinished = 0;
    std::uint64_t shutdownFinished = 0;
    std::uint64_t maxInFlightPosts = 0;
    std::uint64_t bufferedEvents = 0;
    std::uint64_t maxBufferedBytes = 0;
    std::uint64_t readCapacityEvents = 0;
    std::uint64_t maxReadCapacityBytes = 0;
};

// Threaded K=0 quiescence tracker. It is inert unless a harness explicitly
// installs a shared instance on a SimWire, so the existing stepping and hybrid
// paths keep their post/router behavior.
class SimTransportActivity
{
    std::atomic<std::uint64_t> inFlightPosts_{0};
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<std::uint64_t> readStarted_{0};
    std::atomic<std::uint64_t> writeStarted_{0};
    std::atomic<std::uint64_t> shutdownStarted_{0};
    std::atomic<std::uint64_t> readFinished_{0};
    std::atomic<std::uint64_t> writeFinished_{0};
    std::atomic<std::uint64_t> shutdownFinished_{0};
    std::atomic<std::uint64_t> maxInFlightPosts_{0};
    std::atomic<std::uint64_t> bufferedEvents_{0};
    std::atomic<std::uint64_t> maxBufferedBytes_{0};
    std::atomic<std::uint64_t> readCapacityEvents_{0};
    std::atomic<std::uint64_t> maxReadCapacityBytes_{0};

    static void
    noteMax(std::atomic<std::uint64_t>& maxValue, std::uint64_t value)
    {
        auto observed = maxValue.load(std::memory_order_relaxed);
        while (value > observed &&
               !maxValue.compare_exchange_weak(
                   observed,
                   value,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed))
        {
        }
    }

    static std::atomic<std::uint64_t>&
    startedCounter(SimTransportPostKind kind, SimTransportActivity& activity)
    {
        switch (kind)
        {
            case SimTransportPostKind::read:
                return activity.readStarted_;
            case SimTransportPostKind::write:
                return activity.writeStarted_;
            case SimTransportPostKind::shutdown:
                return activity.shutdownStarted_;
        }
        return activity.readStarted_;
    }

    static std::atomic<std::uint64_t>&
    finishedCounter(SimTransportPostKind kind, SimTransportActivity& activity)
    {
        switch (kind)
        {
            case SimTransportPostKind::read:
                return activity.readFinished_;
            case SimTransportPostKind::write:
                return activity.writeFinished_;
            case SimTransportPostKind::shutdown:
                return activity.shutdownFinished_;
        }
        return activity.readFinished_;
    }

public:
    void
    beginPost(SimTransportPostKind kind)
    {
        startedCounter(kind, *this).fetch_add(1, std::memory_order_acq_rel);
        auto const inFlight =
            inFlightPosts_.fetch_add(1, std::memory_order_acq_rel) + 1;
        noteMax(maxInFlightPosts_, inFlight);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    void
    finishPost(SimTransportPostKind kind)
    {
        finishedCounter(kind, *this).fetch_add(1, std::memory_order_acq_rel);
        inFlightPosts_.fetch_sub(1, std::memory_order_acq_rel);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    void
    noteBufferedBytes(std::size_t bytes)
    {
        if (bytes == 0)
            return;
        bufferedEvents_.fetch_add(1, std::memory_order_acq_rel);
        noteMax(maxBufferedBytes_, static_cast<std::uint64_t>(bytes));
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    void
    noteReadCapacity(std::size_t bytes)
    {
        readCapacityEvents_.fetch_add(1, std::memory_order_acq_rel);
        noteMax(maxReadCapacityBytes_, static_cast<std::uint64_t>(bytes));
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] SimTransportActivitySnapshot
    snapshot() const
    {
        return SimTransportActivitySnapshot{
            inFlightPosts_.load(std::memory_order_acquire),
            epoch_.load(std::memory_order_acquire),
            readStarted_.load(std::memory_order_acquire),
            writeStarted_.load(std::memory_order_acquire),
            shutdownStarted_.load(std::memory_order_acquire),
            readFinished_.load(std::memory_order_acquire),
            writeFinished_.load(std::memory_order_acquire),
            shutdownFinished_.load(std::memory_order_acquire),
            maxInFlightPosts_.load(std::memory_order_acquire),
            bufferedEvents_.load(std::memory_order_acquire),
            maxBufferedBytes_.load(std::memory_order_acquire),
            readCapacityEvents_.load(std::memory_order_acquire),
            maxReadCapacityBytes_.load(std::memory_order_acquire)};
    }
};

template <class Handler>
void
trackedPost(
    std::shared_ptr<SimTransportActivity> const& activity,
    Transport::executor_type const& strand,
    SimTransportPostKind kind,
    Handler&& handler)
{
    if (!activity)
    {
        boost::asio::post(strand, std::forward<Handler>(handler));
        return;
    }

    activity->beginPost(kind);
    boost::asio::post(
        strand, [activity, kind, h = std::forward<Handler>(handler)]() mutable {
            struct Finish
            {
                std::shared_ptr<SimTransportActivity> activity;
                SimTransportPostKind kind;

                ~Finish()
                {
                    activity->finishPost(kind);
                }
            } finish{activity, kind};
            h();
        });
}

// One direction of the wire: a byte stream with at most one pending async
// reader (PeerImp keeps a single async_read_some outstanding).
class SimPipe
{
    mutable std::mutex m_;
    std::deque<std::uint8_t> bytes_;
    bool closed_ = false;

    struct PendingRead
    {
        std::vector<boost::asio::mutable_buffer> buffers;
        Transport::executor_type strand;
        Transport::ReadHandler handler;
    };
    std::optional<PendingRead> reader_;

public:
    // S3.4 — the deterministic-delivery seam. When set (stepping mode), a READY
    // read completion is routed HERE instead of posted on the reader's strand:
    // the functor (built in simConnect) captures the shared SteppingController
    // + THIS reader's nodeId + the link delay and calls scheduleDelivery(), so
    // a delivered message arrives as a scheduled event (+delay) on the one
    // stepping thread — clean stack, deterministic order, and the
    // strictly-positive delay breaks the same-instant ricochet. Null
    // (prod/hybrid) → boost::asio::post, byte-identical. In stepping mode
    // SimPipe therefore NEVER posts a read completion inline. The trailing
    // string is DIAGNOSTIC PROVENANCE: the protocol message name(s) this
    // delivery carries (frame-tracked in writeRaw/drainInto). It becomes the
    // scheduler event's label — printed by the replay ladder, never folded into
    // the fingerprint.
    using DeliveryRouter = std::function<
        void(Transport::executor_type, std::function<void()>, std::string)>;

    // §5.7 — the fault seam (see SimFault above). Decides one message's fate
    // from its (type, size); installed at a stepping boundary, empty = off.
    using WriteFault =
        std::function<SimFault(std::uint16_t type, std::size_t size)>;
    // Optional content-aware selection of whole frames. Read-only and valid
    // only during the callback: selectors cannot rewrite signed messages.
    using Frame = std::span<std::uint8_t const>;
    using FrameFault = std::function<SimFault(std::uint16_t type, Frame bytes)>;
    // Installed by simConnect in stepping mode: schedule a raw write of
    // `bytes` into THIS pipe `delay` from now (rides scheduleDelivery, so it
    // inherits the draining/inactive-node guards). Fault delays require it.
    using DelayedWrite = std::function<
        void(std::chrono::steady_clock::duration, std::vector<std::uint8_t>)>;

private:
    DeliveryRouter router_;
    FrameFault writeFault_;
    DelayedWrite delayedWrite_;
    // (type, bytes remaining) of every framed message currently buffered, in
    // order — writeRaw pushes, drainInto consumes. Guarded by m_. Purely
    // diagnostic: names what a read completion carries for the trace label.
    std::deque<std::pair<std::uint16_t, std::size_t>> frames_;
    std::shared_ptr<SimTransportActivity> activity_;

    // Deliver a ready read completion: through the router (stepping) or by
    // posting on the reader's strand (prod/hybrid). MUST be called with m_
    // released — the router enters the scheduler, and post() runs the handler
    // on the strand.
    void
    deliver(
        Transport::executor_type const& strand,
        std::function<void()> completion,
        std::string label = {})
    {
        if (router_)
            router_(strand, std::move(completion), std::move(label));
        else
            trackedPost(
                activity_,
                strand,
                SimTransportPostKind::read,
                std::move(completion));
    }

    // Copy up to the buffer capacity out of bytes_ (caller holds m_).
    // Returns {bytes drained, label naming the frames those bytes belong to}
    // — e.g. "ping" / "proposal,validation" / "validation…" (… = the drain
    // ends mid-frame; the continuation names the same frame again).
    std::pair<std::size_t, std::string>
    drainInto(std::vector<boost::asio::mutable_buffer> const& buffers)
    {
        std::size_t total = 0;
        for (auto const& b : buffers)
        {
            auto* p = static_cast<std::uint8_t*>(b.data());
            std::size_t const n = std::min(b.size(), bytes_.size());
            for (std::size_t i = 0; i < n; ++i)
            {
                p[i] = bytes_.front();
                bytes_.pop_front();
            }
            total += n;
            if (bytes_.empty())
                break;
        }
        std::string label;
        for (std::size_t left = total; left > 0 && !frames_.empty();)
        {
            auto& [type, remaining] = frames_.front();
            auto const consumed = std::min(left, remaining);
            remaining -= consumed;
            left -= consumed;
            if (!label.empty())
                label += ',';
            label += protocolMessageName(static_cast<int>(type));
            if (remaining == 0)
                frames_.pop_front();
            else
                label += "\xe2\x80\xa6";  // U+2026: drain ends mid-frame
        }
        return {total, std::move(label)};
    }

public:
    // Install the deterministic-delivery router (stepping mode). Called once at
    // wiring time, BEFORE any traffic or any PeerImp reads this pipe; a null
    // router leaves prod/hybrid behaviour (asio post) untouched.
    void
    setDeliveryRouter(DeliveryRouter r)
    {
        router_ = std::move(r);
    }

    void
    setActivityTracker(std::shared_ptr<SimTransportActivity> activity)
    {
        activity_ = std::move(activity);
    }

    [[nodiscard]] std::size_t
    bufferedBytes() const
    {
        std::scoped_lock const lock(m_);
        return bytes_.size();
    }

    // Reader side (this endpoint's rx pipe).
    void
    read(
        std::vector<boost::asio::mutable_buffer> buffers,
        Transport::executor_type strand,
        Transport::ReadHandler handler)
    {
        std::unique_lock lock(m_);
        if (!bytes_.empty())
        {
            auto [n, label] = drainInto(buffers);
            lock.unlock();
            deliver(
                strand,
                [h = std::move(handler), n = n]() mutable { h({}, n); },
                std::move(label));
        }
        else if (closed_)
        {
            lock.unlock();
            deliver(strand, [h = std::move(handler)]() mutable {
                h(boost::asio::error::eof, 0);
            });
        }
        else
        {
            reader_ = PendingRead{
                std::move(buffers), std::move(strand), std::move(handler)};
        }
    }

    // Install the per-direction fault injector (empty clears). Set at a
    // stepping boundary; single-threaded stepping makes live install race-free.
    void
    setWriteFault(WriteFault f)
    {
        if (!f)
            writeFault_ = {};
        else
            writeFault_ = [f = std::move(f)](std::uint16_t type, Frame bytes) {
                return f(type, bytes.size());
            };
    }

    void
    setFrameFault(FrameFault f)
    {
        writeFault_ = std::move(f);
    }

    void
    setDelayedWriter(DelayedWrite w)
    {
        delayedWrite_ = std::move(w);
    }

    // Writer side (the PEER endpoint's tx pipe == this pipe). Returns whether
    // the write was accepted before the pipe closed. Intentional fault drops
    // still count as accepted: the sender wrote successfully and the network
    // discarded the message. One call = one framed message; the injector (if
    // any) decides drop/duplicate/delay per message.
    bool
    write(std::vector<std::uint8_t> data)
    {
        if (!writeFault_)
            return writeRaw(std::move(data));

        auto const fault = writeFault_(peekMessageType(data), Frame{data});
        auto const copies = (fault.drop ? 0 : 1) + fault.duplicates;
        if (copies == 0)
        {
            std::scoped_lock const lock(m_);
            return !closed_;
        }
        if (fault.delay > std::chrono::steady_clock::duration::zero())
        {
            if (!delayedWrite_)
                Throw<std::logic_error>(
                    "SimPipe: fault delay requires stepping mode "
                    "(no delayed writer installed)");
            {
                // Linearize acceptance against close(). A close after this
                // point represents an in-flight copy and writeRaw() rejects it
                // when the scheduled delivery eventually lands.
                std::scoped_lock const lock(m_);
                if (closed_)
                    return false;
            }
            for (int i = 0; i < copies; ++i)
                delayedWrite_(fault.delay, data);
            return true;
        }
        for (int i = 0; i < copies; ++i)
        {
            if (!writeRaw(data))
                return i != 0;
        }
        return true;
    }

    // The actual byte insertion — delayed fault copies land here directly so
    // a scheduled copy is never re-faulted (no regress). The closed check and
    // insertion share m_, so a copy cannot enter after close() linearizes.
    bool
    writeRaw(std::vector<std::uint8_t> data)
    {
        std::unique_lock lock(m_);
        if (closed_)
            return false;
        if (!data.empty())
            frames_.emplace_back(peekMessageType(data), data.size());
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        if (activity_)
            activity_->noteBufferedBytes(bytes_.size());
        if (!reader_)
            return true;
        // Fulfil the waiting reader.
        auto r = std::move(*reader_);
        reader_.reset();
        auto [n, label] = drainInto(r.buffers);
        lock.unlock();
        deliver(
            r.strand,
            [h = std::move(r.handler), n = n]() mutable { h({}, n); },
            std::move(label));
        return true;
    }

    void
    close()
    {
        std::unique_lock lock(m_);
        closed_ = true;
        if (!reader_)
            return;
        auto r = std::move(*reader_);
        reader_.reset();
        // Deliver any buffered bytes first, else EOF.
        auto [n, label] = drainInto(r.buffers);
        lock.unlock();
        deliver(
            r.strand,
            [h = std::move(r.handler), n = n]() mutable {
                h(n ? boost::system::error_code{} : boost::asio::error::eof, n);
            },
            n ? std::move(label) : std::string("eof"));
    }
};

class SimWire;

// One endpoint of a SimWire, presented to a PeerImp as its Transport.
class SimTransport : public Transport
{
    Transport::executor_type executor_;  // this node's io_context executor
    std::shared_ptr<SimPipe> rx_;        // this endpoint reads from here
    std::shared_ptr<SimPipe> tx_;  // this endpoint writes into here (peer's rx)
    std::shared_ptr<uint256 const> sharedValue_;
    std::shared_ptr<SimTransportActivity> activity_;
    bool open_ = true;

    friend class SimWire;
    SimTransport(
        Transport::executor_type executor,
        std::shared_ptr<SimPipe> rx,
        std::shared_ptr<SimPipe> tx,
        std::shared_ptr<uint256 const> sharedValue,
        std::shared_ptr<SimTransportActivity> activity = {})
        : executor_(std::move(executor))
        , rx_(std::move(rx))
        , tx_(std::move(tx))
        , sharedValue_(std::move(sharedValue))
        , activity_(std::move(activity))
    {
    }

public:
    [[nodiscard]] executor_type
    get_executor() override
    {
        return executor_;
    }

    [[nodiscard]] bool
    is_open() const override
    {
        return open_;
    }

    void
    async_read_some(
        std::vector<boost::asio::mutable_buffer> buffers,
        executor_type strand,
        ReadHandler handler) override
    {
        if (activity_)
        {
            std::size_t capacity = 0;
            for (auto const& buffer : buffers)
                capacity += buffer.size();
            activity_->noteReadCapacity(capacity);
        }
        rx_->read(std::move(buffers), std::move(strand), std::move(handler));
    }

    void
    async_write(
        std::vector<boost::asio::const_buffer> buffers,
        executor_type strand,
        WriteHandler handler) override
    {
        std::vector<std::uint8_t> bytes;
        std::size_t total = 0;
        for (auto const& b : buffers)
            total += b.size();
        bytes.reserve(total);
        for (auto const& b : buffers)
        {
            auto const* p = static_cast<std::uint8_t const*>(b.data());
            bytes.insert(bytes.end(), p, p + b.size());
        }
        auto const accepted = tx_->write(std::move(bytes));
        trackedPost(
            activity_,
            strand,
            SimTransportPostKind::write,
            [h = std::move(handler), accepted, total]() mutable {
                h(accepted ? Transport::error_code{}
                           : boost::asio::error::broken_pipe,
                  accepted ? total : 0);
            });
    }

    void
    async_shutdown(executor_type strand, ShutdownHandler handler) override
    {
        // No TLS in sim; graceful shutdown completes immediately.
        trackedPost(
            activity_,
            strand,
            SimTransportPostKind::shutdown,
            [h = std::move(handler)]() mutable { h({}); });
    }

    void
    close() override
    {
        open_ = false;
        tx_->close();
        rx_->close();
    }

    [[nodiscard]] std::optional<uint256>
    makeSharedValue(beast::Journal) override
    {
        return *sharedValue_;
    }
};

// Owns the two directional pipes + the per-wire handshake shared value, and
// hands out the two paired endpoints. A and B's executors are their respective
// nodes'.
//
// Stateful + SEVERABLE: the SimWire creates and RETAINS the two directional
// pipes in its ctor, then co-owns them with the SimTransports it hands out (the
// pipes are shared_ptr). So after the PeerImps are live, a test can call
// sever() to close the VERY pipe objects those live PeerImps read from —
// fulfilling each side's single pending async_read_some with EOF and driving a
// deterministic partition/eclipse.
class SimWire
{
    std::shared_ptr<SimPipe> a2b_;  // A writes, B reads
    std::shared_ptr<SimPipe> b2a_;  // B writes, A reads
    bool severed_ = false;

public:
    SimWire()
        : a2b_(std::make_shared<SimPipe>()), b2a_(std::make_shared<SimPipe>())
    {
    }

    std::pair<std::unique_ptr<SimTransport>, std::unique_ptr<SimTransport>>
    endpoints(
        Transport::executor_type execA,
        Transport::executor_type execB,
        uint256 const& sharedValue,
        SimPipe::DeliveryRouter routerForA = {},
        SimPipe::DeliveryRouter routerForB = {},
        std::shared_ptr<SimTransportActivity> activity = {})
    {
        // makeSharedValue()/verifyHandshake reject a zero shared value (a zero
        // means the two TLS finished-messages hashed identically — a MITM
        // tell). The sim has no TLS, so guarantee a deterministic NON-ZERO
        // value here.
        uint256 nonZero = sharedValue;
        if (nonZero == uint256{})
            nonZero.data()[0] = 1;
        auto sv = std::make_shared<uint256 const>(nonZero);
        // S3.4 deterministic delivery (stepping mode): A reads from b2a_, B
        // reads from a2b_, so each reader's completions route through ITS
        // node's router. Empty routers (prod/hybrid) leave the pipes on the
        // asio-post path.
        if (routerForA)
            b2a_->setDeliveryRouter(std::move(routerForA));
        if (routerForB)
            a2b_->setDeliveryRouter(std::move(routerForB));
        if (activity)
        {
            a2b_->setActivityTracker(activity);
            b2a_->setActivityTracker(activity);
        }
        // A: rx=b2a, tx=a2b ; B: rx=a2b, tx=b2a
        return {
            std::unique_ptr<SimTransport>(
                new SimTransport(std::move(execA), b2a_, a2b_, sv, activity)),
            std::unique_ptr<SimTransport>(
                new SimTransport(std::move(execB), a2b_, b2a_, sv, activity))};
    }

    void
    setActivityTracker(std::shared_ptr<SimTransportActivity> activity)
    {
        a2b_->setActivityTracker(activity);
        b2a_->setActivityTracker(std::move(activity));
    }

    [[nodiscard]] std::size_t
    bufferedBytes() const
    {
        return a2b_->bufferedBytes() + b2a_->bufferedBytes();
    }

    // §5.7: install a fault injector on ONE direction of the live wire
    // (aToB=true faults messages A wrote toward B). The wire retains the
    // pipes, so this works after the transports were handed to the PeerImps.
    // Empty clears. Install at a stepping boundary.
    void
    setFault(bool aToB, SimPipe::WriteFault f)
    {
        (aToB ? a2b_ : b2a_)->setWriteFault(std::move(f));
    }

    void
    setFrameFault(bool aToB, SimPipe::FrameFault f)
    {
        (aToB ? a2b_ : b2a_)->setFrameFault(std::move(f));
    }

    /** Inject one already-framed protocol message from endpoint A toward B. */
    void
    injectAToB(std::vector<std::uint8_t> bytes)
    {
        a2b_->writeRaw(std::move(bytes));
    }

    /** Inject one already-framed protocol message from endpoint B toward A. */
    void
    injectBToA(std::vector<std::uint8_t> bytes)
    {
        b2a_->writeRaw(std::move(bytes));
    }

    // §5.7: give both pipes a delayed-write path for SimFault::delay —
    // installed by simConnect in stepping mode alongside the delivery
    // routers. A delayed copy is scheduled on the RECEIVING node (it is
    // in-flight network latency) and lands via writeRaw (never re-faulted).
    template <class ScheduleDelayed>
    void
    configureDelayedWrites(ScheduleDelayed&& make)
    {
        // make(nodeSide, pipe) -> DelayedWrite; side A reads b2a_, B reads
        // a2b_.
        a2b_->setDelayedWriter(make(/*towardA=*/false, a2b_));
        b2a_->setDelayedWriter(make(/*towardA=*/true, b2a_));
    }

    // Deterministic partition: close BOTH directional pipes. Each close()
    // fulfils the peer's single pending async_read_some with EOF
    // (boost::asio::error::eof)
    // -> PeerImp fail()s -> OverlayImpl drops it -> size() decreases on BOTH
    // ends. Idempotent (SimPipe::close just re-sets closed_).
    void
    sever()
    {
        severed_ = true;
        a2b_->close();
        b2a_->close();
    }

    [[nodiscard]] bool
    severed() const
    {
        return severed_;
    }
};

}  // namespace ripple::test
