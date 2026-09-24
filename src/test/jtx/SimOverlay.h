#pragma once
//------------------------------------------------------------------------------
// SimOverlay — an in-process Overlay for the Stage 1 harness, injected via the
// S0.1 overlay-factory hook (make_Application's OverlayFactory) in place of the
// real loopback makeOverlay(). It is the REAL OverlayImpl, subclassed so a test
// can wire two nodes' PeerImps together over an in-process byte-pipe bus
// (SimTransport) with NO TCP/TLS — partition/eclipse become deterministic.
//
// Design (spec §5.1/§6/§7): rather than re-stub Overlay's 22 virtuals,
// SimOverlay
// `: public OverlayImpl` and inherits the entire production peer container,
// relay and metrics machinery. The ONLY deltas vs prod are:
//
//   1. A no-op Resolver (the sim has no DNS). OverlayImpl stores `Resolver&`,
//   so
//      it is supplied through the base-from-member idiom: a tiny base owns the
//      Resolver and is listed BEFORE OverlayImpl so it is fully constructed
//      when the OverlayImpl base ctor binds the reference.
//   2. start() keeps peerFinder setConfig+start (slots need it) but SKIPS the
//      bootstrap DNS resolve and the once-per-second maintenance Timer — the
//      sim autoconnects nothing and is driven deterministically by
//      simConnect().
//
// simConnect() (below) stands up a real handshaked PeerImp pair across two
// nodes' SimOverlays over a SimWire — this is the §7 crux that lets real
// PeerImps exchange protocol messages (proposals/validations) over the bus.
//
// Spec: .ai-docs/specs/csf-peerimp-hybrid-overlay-harness.md (Stage 1).
//------------------------------------------------------------------------------
#include <test/jtx/SimTransport.h>
#include <test/jtx/SteppingController.h>

#include <xrpld/app/main/CollectorManager.h>
#include <xrpld/overlay/detail/Handshake.h>
#include <xrpld/overlay/detail/OverlayImpl.h>
#include <xrpld/overlay/detail/PeerImp.h>
#include <xrpld/overlay/detail/ProtocolVersion.h>
#include <xrpld/overlay/make_Overlay.h>
#include <xrpld/peerfinder/PeerfinderManager.h>
#include <xrpld/rpc/ServerHandler.h>

#include <xrpl/basics/ResolverAsio.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/net/IPEndpoint.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/verb.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ripple::test {

// A Resolver that never resolves anything. The sim has no DNS and SimOverlay
// never issues a resolve, so every method is a safe no-op. (Resolver's pure
// virtual dtor is defined out-of-line in ResolverAsio.cpp, so deriving is
// cheap.)
class NoopResolver : public Resolver
{
public:
    void
    stop_async() override
    {
    }
    void
    stop() override
    {
    }
    void
    start() override
    {
    }
    void
    resolve(std::vector<std::string> const&, HandlerType const&) override
    {
    }
};

namespace detail {
// base-from-member: owns the Resolver so it outlives — and is constructed
// before — the OverlayImpl base that binds `Resolver&`.
struct SimResolverHolder
{
    NoopResolver resolver;
};
}  // namespace detail

// Reserve a range from the process-wide synthetic peer-ID sequence. Replay
// tests can perturb allocation history without creating another Application
// or resetting IDs that may still belong to live peers.
inline Peer::id_t
reserveSimPeerIds(Peer::id_t count = 1)
{
    static std::atomic<Peer::id_t> next{1};
    return next.fetch_add(count);
}

class SimOverlay : private detail::SimResolverHolder, public OverlayImpl
{
    Application& app_;

public:
    explicit SimOverlay(Application& app)
        : detail::SimResolverHolder{}
        , OverlayImpl(
              app,
              setup_Overlay(app.config()),
              app.getServerHandler(),
              app.getResourceManager(),
              SimResolverHolder::resolver,
              app.getIOService(),
              app.config(),
              app.getCollectorManager().collector())
        , app_(app)
    {
    }

    // Like OverlayImpl::start() but deterministic: configure + start peerFinder
    // (slots/activate need it) yet SKIP the bootstrap DNS resolve and the
    // maintenance Timer (no autoconnect, no endpoint gossip). The work-guard
    // and teardown plumbing come from the OverlayImpl base unchanged.
    void
    start() override
    {
        PeerFinder::Config const config = PeerFinder::Config::makeConfig(
            app_.config(),
            app_.getServerHandler().setup().overlay.port(),
            app_.getValidationPublicKey().has_value(),
            OverlayImpl::setup().ipLimit);

        OverlayImpl::peerFinder().setConfig(config);
        OverlayImpl::peerFinder().start();
    }
};

// Optional deterministic-stepping wiring for simConnect (S3.4). When
// `controller` is non-null, each node's cross-node read completions are routed
// onto the shared SteppingController: a delivered message arrives as a
// scheduled event (+linkDelay) on the one stepping thread instead of an inline
// asio post. nodeIdA/nodeIdB are the two nodes' scheduler identities; linkDelay
// is the per-link latency and MUST be strictly positive (scheduleDelivery
// rejects a same-instant delivery). Default
// {} (controller == nullptr) → unchanged hybrid behaviour (asio post).
struct SimSteppingLink
{
    SteppingController* controller;
    std::uint32_t nodeIdA;
    std::uint32_t nodeIdB;
    HarnessScheduler::duration linkDelay;
    std::shared_ptr<SimTransportActivity> activity;

    SimSteppingLink(
        SteppingController* controller_ = nullptr,
        std::uint32_t nodeIdA_ = 0,
        std::uint32_t nodeIdB_ = 0,
        HarnessScheduler::duration linkDelay_ = {},
        std::shared_ptr<SimTransportActivity> activity_ = {})
        : controller(controller_)
        , nodeIdA(nodeIdA_)
        , nodeIdB(nodeIdB_)
        , linkDelay(linkDelay_)
        , activity(std::move(activity_))
    {
    }
};

//------------------------------------------------------------------------------
// simConnect — establish a real, handshaked PeerImp pair between two nodes over
// an in-process SimWire.
//
// Faithfulness note: prod pairs one OUTBOUND PeerImp (ConnectAttempt, which has
// already consumed the 101 response off the wire) with one INBOUND PeerImp
// (onHandoff, whose doAccept() WRITES the 101 response onto the wire).
// Replaying that asymmetry here would require driving the HTTP handshake
// byte-by-byte over the bus (a beast http parser fed from the pipe), because a
// directly-constructed outbound peer would mis-read the inbound peer's
// doAccept() response as a protocol message and disconnect. Instead both peers
// are built via the OUTBOUND (already-handshaked) ctor: neither writes an HTTP
// response, the pipes stay clean, and both go straight to doProtocolStart() and
// exchange REAL protocol messages over the bus. The handshake itself is still
// EXERCISED for real — we build each side's response with the production
// makeResponse()/buildHandshake() (real Session-Signature over a shared value)
// and verify it with the production verifyHandshake() to derive the peer's
// public key — it just isn't byte-streamed over the bus. Each peer ends up
// active (in ids_), so size()==1 on both nodes.
//
// Returns the heap-allocated SimWire (it owns the two directional pipes,
// co-owned with the live PeerImps' SimTransports). The caller keeps the handle
// so it can later sever() the bus — closing the same pipe objects the PeerImps
// read from — for a deterministic partition. Ignore the return value to just
// connect.
//------------------------------------------------------------------------------
inline std::shared_ptr<SimWire>
simConnect(Application& a, Application& b, SimSteppingLink link = {})
{
    auto& ovA = dynamic_cast<SimOverlay&>(a.overlay());
    auto& ovB = dynamic_cast<SimOverlay&>(b.overlay());

    // Unique, never-self loopback endpoints for the two slots (the real bus is
    // the SimWire; these are only slot identities / remoteAddress_).
    static std::atomic<std::uint16_t> nextSimPort{40000};
    auto const loopback = boost::asio::ip::make_address("127.0.0.1");
    std::uint16_t const portA = nextSimPort++;
    std::uint16_t const portB = nextSimPort++;
    beast::IP::Endpoint const epA{loopback, portA};  // node a's address
    beast::IP::Endpoint const epB{loopback, portB};  // node b's address

    // A single deterministic NON-ZERO shared value for the wire/handshake (the
    // sim's stand-in for the TLS finished-message cookie). Both makeResponse
    // and verifyHandshake below use THIS value, so the Session-Signature
    // verifies.
    uint256 sharedValue;
    sharedValue.data()[0] = 0xA5;

    // S3.4: in stepping mode, build a per-node delivery router that hands each
    // ready read completion to the shared SteppingController (scheduled
    // +linkDelay on the stepping thread). The completion is dispatched THROUGH
    // the reader's (inline) strand so PeerImp's read handler runs in strand
    // context — the EOF path flows into fail(), which asserts
    // strand_.running_in_this_thread(). With a null controller both routers
    // stay empty → endpoints() leaves the asio path.
    SimPipe::DeliveryRouter routerForA, routerForB;
    if (link.controller)
    {
        auto makeRouter = [c = link.controller](
                              std::uint32_t nodeId,
                              HarnessScheduler::duration delay) {
            return [c, nodeId, delay](
                       Transport::executor_type strand,
                       std::function<void()> completion,
                       std::string label) {
                // Teardown: the scheduler is being dropped and this may run on
                // the io poll-pump thread (off the stepping thread) as peers
                // close during app->run() shutdown. Drop the residual
                // completion — the node is going away and determinism no longer
                // applies — rather than scheduling off-thread (which
                // hard-fails) or running it inline (which races the shutdown).
                if (c->draining())
                    return;
                c->scheduleDelivery(
                    nodeId,
                    delay,
                    [strand, completion = std::move(completion)]() mutable {
                        boost::asio::dispatch(strand, std::move(completion));
                    },
                    std::move(label));
            };
        };
        routerForA = makeRouter(link.nodeIdA, link.linkDelay);
        routerForB = makeRouter(link.nodeIdB, link.linkDelay);
    }

    auto simWire = std::make_shared<SimWire>();
    auto [transportA, transportB] = simWire->endpoints(
        a.getIOService().get_executor(),
        b.getIOService().get_executor(),
        sharedValue,
        std::move(routerForA),
        std::move(routerForB),
        link.activity);

    // §5.7: in stepping mode, give both pipes a delayed-write path so a
    // fault injector can add per-message latency. The delayed copy is
    // scheduled on the RECEIVING node's timeline (in-flight latency) via
    // scheduleDelivery, inheriting its draining/inactive guards, and lands
    // through writeRaw so it is never re-faulted.
    if (link.controller)
    {
        simWire->configureDelayedWrites(
            [c = link.controller, idA = link.nodeIdA, idB = link.nodeIdB](
                bool towardA, std::shared_ptr<SimPipe> const& pipe) {
                auto const nodeId = towardA ? idA : idB;
                return [c, nodeId, wp = std::weak_ptr<SimPipe>(pipe)](
                           std::chrono::steady_clock::duration delay,
                           std::vector<std::uint8_t> bytes) {
                    auto label = "delayed:" +
                        protocolMessageName(
                                     static_cast<int>(peekMessageType(bytes)));
                    c->scheduleDelivery(
                        nodeId,
                        delay,
                        [wp, bytes = std::move(bytes)]() {
                            if (auto const p = wp.lock())
                                p->writeRaw(bytes);
                        },
                        std::move(label));
                };
            });
    }

    beast::IP::Address const
        publicIp{};  // unspecified -> no Local-IP, IP checks skipped
    auto const version =
        negotiateProtocolVersion(supportedProtocolVersions()).value();

    // A minimal peer upgrade request; makeResponse only reads its version and
    // X-Protocol-Ctl (feature negotiation). Both nodes share one config.
    auto const makePeerRequest = [&](Application& app) {
        http_request_type req;
        req.method(boost::beast::http::verb::get);
        req.target("/");
        req.version(11);
        req.insert("Upgrade", supportedProtocolVersions());
        req.insert("Connection", "Upgrade");
        req.insert("Connect-As", "Peer");
        req.insert(
            "X-Protocol-Ctl",
            makeFeaturesRequestHeader(
                app.config().COMPRESSION,
                app.config().LEDGER_REPLAY,
                app.config().TX_REDUCE_RELAY_ENABLE,
                app.config().VP_REDUCE_RELAY_ENABLE));
        return req;
    };

    // The response each node would have sent the other (signed by its own
    // identity over sharedValue). remoteIp is the OTHER node's (loopback)
    // address, so buildHandshake adds no Remote-IP/Local-IP headers.
    auto respFromB = makeResponse(
        false,
        makePeerRequest(b),
        publicIp,
        epA.address(),
        sharedValue,
        ovB.OverlayImpl::setup().networkID,
        version,
        b);
    auto respFromA = makeResponse(
        false,
        makePeerRequest(a),
        publicIp,
        epB.address(),
        sharedValue,
        ovA.OverlayImpl::setup().networkID,
        version,
        a);

    // Each node verifies the other's response (real security checks) -> peer
    // key.
    PublicKey const pkB = verifyHandshake(
        respFromB,
        sharedValue,
        ovA.OverlayImpl::setup().networkID,
        publicIp,
        epB.address(),
        a);
    PublicKey const pkA = verifyHandshake(
        respFromA,
        sharedValue,
        ovB.OverlayImpl::setup().networkID,
        publicIp,
        epA.address(),
        b);

    // Build + activate one peer per node (mirrors
    // ConnectAttempt::processResponse: new_outbound_slot -> onConnected ->
    // activate -> make_shared<PeerImp> -> addActive).
    auto wire = [](Application& app,
                   SimOverlay& ov,
                   std::unique_ptr<SimTransport>&& transport,
                   beast::IP::Endpoint const& localEp,
                   beast::IP::Endpoint const& remoteEp,
                   http_response_type&& response,
                   PublicKey const& peerKey,
                   ProtocolVersion ver) {
        auto usage =
            ov.OverlayImpl::resourceManager().newUnlimitedEndpoint(remoteEp);
        auto slot = ov.OverlayImpl::peerFinder().new_outbound_slot(remoteEp);
        if (slot == nullptr)
            return false;
        if (!ov.OverlayImpl::peerFinder().onConnected(slot, localEp))
            return false;
        if (ov.OverlayImpl::peerFinder().activate(slot, peerKey, false) !=
            PeerFinder::Result::success)
            return false;

        auto const peer = std::make_shared<PeerImp>(
            app,
            std::unique_ptr<Transport>(std::move(transport)),
            boost::asio::const_buffer{},  // no leftover handshake bytes
            std::move(slot),
            std::move(response),
            usage,
            peerKey,
            ver,
            reserveSimPeerIds(),
            ov);
        ov.add_active(peer);
        return true;
    };

    bool const wiredA = wire(
        a,
        ovA,
        std::move(transportA),
        epA,
        epB,
        std::move(respFromB),
        pkB,
        version);
    bool const wiredB = wire(
        b,
        ovB,
        std::move(transportB),
        epB,
        epA,
        std::move(respFromA),
        pkA,
        version);
    if (!wiredA || !wiredB)
    {
        simWire->sever();
        return {};
    }

    return simWire;
}

}  // namespace ripple::test
