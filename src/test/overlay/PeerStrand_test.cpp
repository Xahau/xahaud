//------------------------------------------------------------------------------
// PeerStrand / InlineExecutor unit suite (Stage 3, S3.2) — proves the inline
// strand behaves the way the stepping harness needs, in ISOLATION (a bare
// io_context, no PeerImp), addressing the review caution: dispatch(), post() and
// bind_executor() over the inline strand must run on the CALLER thread with NO
// io_context servicing (no second queue), while the production strand (default,
// inlineStrands == false) must keep deferring to the io_context exactly as before.
// Spec: csf-peerimp-hybrid-overlay-harness.md (Stage 3, S3.2).
//------------------------------------------------------------------------------
#include <xrpld/overlay/detail/PeerStrand.h>
#include <xrpld/overlay/detail/Transport.h>  // Transport::executor_type (the erased path)

#include <xrpl/beast/unit_test/suite.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <thread>
#include <vector>

namespace ripple {
namespace test {

class PeerStrand_test : public beast::unit_test::suite
{
    static Config
    cfg(bool inlineStrands)
    {
        Config c;
        c.inlineStrands = inlineStrands;
        return c;
    }

    void
    testInlineRunsOnCaller()
    {
        testcase("inlineStrands: dispatch/post/bind_executor run inline on caller");
        boost::asio::io_context io;
        auto strand = makePeerStrand(cfg(true), io.get_executor());
        auto const me = std::this_thread::get_id();
        std::vector<int> order;

        bool dispatched = false;
        boost::asio::dispatch(strand, [&]() {
            order.push_back(1);
            dispatched = true;
            BEAST_EXPECT(std::this_thread::get_id() == me);
        });
        BEAST_EXPECT(dispatched);  // ran INLINE — before dispatch() returned

        bool posted = false;
        boost::asio::post(strand, [&]() {
            order.push_back(2);
            posted = true;
            BEAST_EXPECT(std::this_thread::get_id() == me);
        });
        BEAST_EXPECT(posted);  // post() also ran inline (no second queue)

        bool bound = false;
        auto h = boost::asio::bind_executor(strand, [&]() {
            order.push_back(3);
            bound = true;
            BEAST_EXPECT(std::this_thread::get_id() == me);
        });
        h();  // invoking a strand-bound completion runs it inline
        BEAST_EXPECT(bound);

        // Nothing was queued on the io_context — it has no work to service.
        BEAST_EXPECT(io.poll() == 0);
        BEAST_EXPECT((order == std::vector<int>{1, 2, 3}));
    }

    void
    testNestedStaysOnThread()
    {
        testcase("inlineStrands: nested post from a handler stays on the thread, FIFO");
        boost::asio::io_context io;
        auto strand = makePeerStrand(cfg(true), io.get_executor());
        auto const me = std::this_thread::get_id();
        std::vector<int> order;

        boost::asio::dispatch(strand, [&]() {
            order.push_back(1);
            // A handler that schedules more strand work (as PeerImp does when a
            // delivered message dispatches a follow-up). It must NOT need an io
            // thread/poll to run — and the strand must keep FIFO.
            boost::asio::post(strand, [&]() {
                order.push_back(3);
                BEAST_EXPECT(std::this_thread::get_id() == me);
            });
            order.push_back(2);
        });

        BEAST_EXPECT(io.poll() == 0);  // never needed the io_context
        BEAST_EXPECT((order == std::vector<int>{1, 2, 3}));
    }

    void
    testErasedTransportPathRunsInline()
    {
        testcase("inlineStrands: post/bind_executor via erased Transport::executor_type run inline");
        boost::asio::io_context io;
        auto strand = makePeerStrand(cfg(true), io.get_executor());
        // The EXACT erasure the transport does: PeerImp hands strand_ to
        // transport_->async_read_some/async_write as a Transport::executor_type
        // (any_io_executor), and SimTransport/SslTransport post + bind_executor on
        // THAT. So the inline behavior must survive strand<executor> → any_io_executor.
        Transport::executor_type erased = strand;
        auto const me = std::this_thread::get_id();
        std::vector<int> order;

        bool posted = false;
        boost::asio::post(erased, [&]() {
            order.push_back(1);
            posted = true;
            BEAST_EXPECT(std::this_thread::get_id() == me);
        });
        BEAST_EXPECT(posted);  // inline through the erased executor (transport path)

        bool bound = false;
        auto h = boost::asio::bind_executor(erased, [&]() {
            order.push_back(2);
            bound = true;
            BEAST_EXPECT(std::this_thread::get_id() == me);
        });
        h();
        BEAST_EXPECT(bound);

        BEAST_EXPECT(io.poll() == 0);  // still no second queue after erasure
        BEAST_EXPECT((order == std::vector<int>{1, 2}));
    }

    void
    testProductionDefersToIo()
    {
        testcase("production strand (inlineStrands off) defers to the io_context");
        boost::asio::io_context io;
        auto strand = makePeerStrand(cfg(false), io.get_executor());

        bool ran = false;
        boost::asio::post(strand, [&]() { ran = true; });
        BEAST_EXPECT(!ran);  // NOT inline — queued on the io_context as before
        BEAST_EXPECT(io.poll() >= 1);
        BEAST_EXPECT(ran);  // ran only once the io_context was serviced
    }

public:
    void
    run() override
    {
        testInlineRunsOnCaller();
        testNestedStaysOnThread();
        testErasedTransportPathRunsInline();
        testProductionDefersToIo();
    }
};

BEAST_DEFINE_TESTSUITE(PeerStrand, overlay, ripple);

}  // namespace test
}  // namespace ripple
