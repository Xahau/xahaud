//------------------------------------------------------------------------------
// CSF + PeerImp hybrid harness — Stage 0 acceptance (S0.6 / S0.ACCEPT).
//
// Stands up N real ApplicationImp in ONE process (between jtx::Env's single
// standalone app and csf::Sim's N toy ledgers) and drives them through real
// consensus. The bring-up/orchestration lives in the reusable MultiNode harness
// (test/jtx/MultiNode.h, extracted from the rungs below); this suite is the
// ladder that proves it, one rung per testcase, each guarded by the hang-safe
// runner (.ai-docs/impl/htest.py) so a wedged bring-up reports TIMEOUT:
//   A. one PEERED node comes up + tears down cleanly.
//   B. two coexist in one process.
//   C. two connect over the real loopback overlay (PeerImp handshake).
//   D. two validators converge on a shared validated ledger  <- the Stage 0 gate.
//   E. three validators converge (harness generalizes past N=2).
//   F–J. SimOverlay / SimTransport in-process bus rungs (Stage 1).
//   K. gating spike: two validators converge on a shared validated ledger driven
//      ENTIRELY by VIRTUAL ticks — manual consensus heartbeat + manual steady/
//      NetClock clocks, NO asio wall-clock heartbeat (Stage 2 driver mechanism).
//
// Spec: .ai-docs/specs/csf-peerimp-hybrid-overlay-harness.md (Stage 0/1/2).
//------------------------------------------------------------------------------
#include <test/jtx/MultiNode.h>
#include <test/jtx/SimOverlay.h>
#include <test/jtx/SimTransport.h>

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/overlay/Overlay.h>

#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/rdb/RelationalDatabase.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ripple::test {

class HarnessNet_test : public beast::unit_test::Suite
{
    void
    testSingleNodeBringUp()
    {
        testcase("A: single peered node — setup() returns, clean teardown");
        MultiNode net(*this);
        auto& n = net.add();
        BEAST_EXPECT(n.isUp());
        if (n.isUp())
            BEAST_EXPECT(!n.app().config().standalone());
        // net's dtor tears the node down and drops the shared debug sink.
    }

    void
    testTwoNodesCoexist()
    {
        testcase("B: two peered nodes coexist in one process");
        MultiNode net(*this);
        net.add();
        net.add();
        BEAST_EXPECT(net.size() == 2 && net.allUp());
        if (net.allUp())
            BEAST_EXPECT(&net[0].app() != &net[1].app());
    }

    void
    testTwoNodesPeer()
    {
        testcase("C: two peered nodes connect over the loopback overlay");
        using namespace std::chrono_literals;
        MultiNode net(*this);
        net.add();
        net.add();
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // Real PeerImp path: node[0] dials node[1]'s bound peer port; both must
        // reach an active (post-handshake) peer.
        net.connect(0, 1);
        BEAST_EXPECT(net.waitForPeers(1, 15s));
        log << "  node0 peers=" << net[0].app().overlay().size()
            << " node1 peers=" << net[1].app().overlay().size() << std::endl;
    }

    void
    testTwoNodesConverge()
    {
        testcase("D: two peered validators converge on a shared validated ledger");
        using namespace std::chrono_literals;
        MultiNode net(*this);

        // Two distinct validators; each node trusts BOTH (UNL size 2 → quorum 2:
        // both must validate, the intended safety property).
        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        net.add(TrustConfig{valA.seed, unl});
        net.add(TrustConfig{valB.seed, unl});
        if (!BEAST_EXPECT(net.allUp()))
            return;

        net.connect(0, 1);
        if (!BEAST_EXPECT(net.waitForPeers(1, 15s)))
            return;

        // Advance the virtual clocks in lockstep with wall time, else openTime
        // never reaches ledgerMinClose and no ledger ever closes.
        auto const pump = net.pumpClocks();

        bool const converged = net.waitForValidated(2, 90s);
        log << "  validated: node0="
            << net[0].app().getLedgerMaster().getValidLedgerIndex()
            << " node1="
            << net[1].app().getLedgerMaster().getValidLedgerIndex() << std::endl;
        BEAST_EXPECT(converged);

        // They must agree on history: same hash at a common validated seq.
        if (converged)
            BEAST_EXPECT(net.ledgersAgree(net.minValidated()));
    }

    void
    testThreeNodesConverge()
    {
        testcase("E: three validators converge (harness generalizes past N=2)");
        using namespace std::chrono_literals;
        MultiNode net(*this);

        // 3 validators; every node trusts all 3 (UNL size 3 → quorum 3, so all
        // must validate). Proves the extracted harness isn't hard-wired to 2.
        std::vector<ValidatorKey> keys;
        std::vector<std::string> unl;
        for (auto const* name : {"node-0", "node-1", "node-2"})
        {
            keys.push_back(ValidatorKey::fromPassphrase(name));
            unl.push_back(keys.back().pubKey);
        }
        for (auto const& k : keys)
            net.add(TrustConfig{k.seed, unl});
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // Fully connect the three; each ends with 2 active peers.
        net.connect(0, 1);
        net.connect(0, 2);
        net.connect(1, 2);
        if (!BEAST_EXPECT(net.waitForPeers(2, 20s)))
            return;

        auto const pump = net.pumpClocks();
        bool const converged = net.waitForValidated(2, 120s);
        log << "  validated: n0=" << net[0].app().getLedgerMaster().getValidLedgerIndex()
            << " n1=" << net[1].app().getLedgerMaster().getValidLedgerIndex()
            << " n2=" << net[2].app().getLedgerMaster().getValidLedgerIndex()
            << std::endl;
        BEAST_EXPECT(converged);
        if (converged)
            BEAST_EXPECT(net.ledgersAgree(net.minValidated()));
    }

    void
    testSimOverlayStandup()
    {
        testcase("F: node stands up with an injected SimOverlay (Stage 1 scaffold)");
        MultiNode net(*this);
        // Inject the in-process SimOverlay via the S0.1 factory instead of the
        // real loopback overlay. Stage 1 increment 1: prove a node stands up with
        // a valid-but-EMPTY overlay (two-phase standup — beginConsensus fires
        // right after overlay construction, so size()/getActivePeers() must be
        // valid at t=0). No transport yet, so no peers.
        auto& n = net.add(std::nullopt, [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        });
        if (!BEAST_EXPECT(n.isUp()))
            return;
        BEAST_EXPECT(n.app().overlay().size() == 0);
        BEAST_EXPECT(n.app().overlay().getActivePeers().empty());
    }

    void
    testSimTransportBus()
    {
        testcase("G: SimTransport delivers bytes end-to-end over the in-process bus");
        // The bus primitive in isolation (no Application): B posts a read, A
        // writes, B's read completes with the bytes on its executor.
        boost::asio::io_context ioc;
        auto exec = Transport::executor_type(ioc.get_executor());
        SimWire wire;
        auto [a, b] = wire.endpoints(exec, exec, uint256{});

        std::string const msg = "hello-peer";
        std::array<std::uint8_t, 64> rbuf{};
        std::size_t readN = 0;
        bool readDone = false, writeDone = false;

        b->async_read_some(
            {boost::asio::buffer(rbuf)},
            exec,
            [&](Transport::error_code ec, std::size_t n) {
                BEAST_EXPECT(!ec);
                readN = n;
                readDone = true;
            });
        a->async_write(
            {boost::asio::buffer(msg)},
            exec,
            [&](Transport::error_code ec, std::size_t n) {
                BEAST_EXPECT(!ec && n == msg.size());
                writeDone = true;
            });

        ioc.run();
        BEAST_EXPECT(writeDone && readDone);
        BEAST_EXPECT(readN == msg.size());
        BEAST_EXPECT(
            std::string(rbuf.begin(), rbuf.begin() + readN) == msg);
    }

    void
    testSimOverlayEstablish()
    {
        testcase("H: real PeerImps peer over the in-process bus (SimOverlay)");
        using namespace std::chrono_literals;
        MultiNode net(*this);

        // Two validators (UNL size 2 -> quorum 2), each wired with a SimOverlay
        // instead of the loopback overlay. Trust is static [validators], so no
        // validator-list exchange is needed for them to trust each other.
        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(TrustConfig{valA.seed, unl}, simFactory);
        net.add(TrustConfig{valB.seed, unl}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // Stand up a real handshaked PeerImp pair over a SimWire (no TCP/TLS).
        simConnect(net[0].app(), net[1].app());

        // Each SimOverlay must reach one active (post-handshake) peer — proof the
        // real PeerImps are alive and talking over the bus.
        BEAST_EXPECT(net.waitForPeers(1, 20s));
        log << "  node0 peers=" << net[0].app().overlay().size()
            << " node1 peers=" << net[1].app().overlay().size() << std::endl;
        BEAST_EXPECT(net[0].app().overlay().size() == 1);
        BEAST_EXPECT(net[1].app().overlay().size() == 1);
    }

    void
    testSimOverlayConverge()
    {
        testcase("I: two validators converge over the sim transport (stretch)");
        using namespace std::chrono_literals;
        MultiNode net(*this);

        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(TrustConfig{valA.seed, unl}, simFactory);
        net.add(TrustConfig{valB.seed, unl}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        simConnect(net[0].app(), net[1].app());
        if (!BEAST_EXPECT(net.waitForPeers(1, 20s)))
            return;

        // Drive consensus: lockstep clocks, then wait for a shared validated
        // ledger and a matching hash. Proposals/validations flow over the bus.
        auto const pump = net.pumpClocks();
        bool const converged = net.waitForValidated(2, 120s);
        log << "  validated: node0="
            << net[0].app().getLedgerMaster().getValidLedgerIndex()
            << " node1="
            << net[1].app().getLedgerMaster().getValidLedgerIndex() << std::endl;
        BEAST_EXPECT(converged);
        if (converged)
            BEAST_EXPECT(net.ledgersAgree(net.minValidated()));
    }

    void
    testSimPartition()
    {
        testcase("J: severing the sim bus deterministically drops the peer");
        using namespace std::chrono_literals;
        MultiNode net(*this);

        // Mirror rung H: two validators (UNL size 2), each on a SimOverlay.
        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(TrustConfig{valA.seed, unl}, simFactory);
        net.add(TrustConfig{valB.seed, unl}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // Stand up the handshaked PeerImp pair over a SimWire and keep the handle
        // so we can sever the bus on cue.
        auto wire = simConnect(net[0].app(), net[1].app());
        if (!BEAST_EXPECT(net.waitForPeers(1, 20s)))
            return;
        log << "  before sever: node0 peers="
            << net[0].app().overlay().size()
            << " node1 peers=" << net[1].app().overlay().size() << std::endl;

        // Sever BOTH directional pipes. Each PeerImp's pending async_read_some
        // completes with EOF -> fail() -> OverlayImpl drops it on BOTH ends.
        auto const t0 = std::chrono::steady_clock::now();
        wire->sever();

        bool const dropped = waitUntil(
            [&]() {
                return net[0].app().overlay().size() == 0 &&
                    net[1].app().overlay().size() == 0;
            },
            20s);
        auto const dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        log << "  after sever (" << dt.count() << "ms): node0 peers="
            << net[0].app().overlay().size()
            << " node1 peers=" << net[1].app().overlay().size() << std::endl;
        BEAST_EXPECT(dropped);
    }

    void
    testVirtualConverge()
    {
        testcase(
            "K: gating spike — two validators converge via VIRTUAL ticks "
            "(manual heartbeat + manual clocks, no asio wall-clock)");
        using namespace std::chrono_literals;

        // virtualClock=true: both nodes run on a shared manual STEADY clock with
        // the asio consensus heartbeat suppressed (Config::manualHeartbeat). This
        // mirrors rung I (two SimOverlay validators, UNL size 2 -> quorum 2) but
        // is driven by tick() — the Stage 2 mechanism — instead of pumpClocks()
        // + the ~1s asio heartbeat.
        MultiNode net(*this, /*virtualClock=*/true);

        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(TrustConfig{valA.seed, unl}, simFactory);
        net.add(TrustConfig{valB.seed, unl}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // The PeerImp handshake is synchronous (simConnect), so peers are active
        // immediately; this bounded wall poll just confirms it.
        simConnect(net[0].app(), net[1].app());
        if (!BEAST_EXPECT(net.waitForPeers(1, 20s)))
            return;

        // Drive consensus purely on virtual ticks: each tick advances the steady
        // AND NetClock clocks by 1s (the production heartbeat granularity) and
        // fires ONE manual heartbeat per node. No asio wall-clock heartbeat is
        // involved (manualHeartbeat=true). The tick count is bounded, so a stall
        // surfaces as a FAIL (not a hang) — runVirtual returns at the cap.
        constexpr std::size_t kMaxTicks = 200;
        auto const ticks = net.runVirtual(/*target=*/2, kMaxTicks, 1s);
        bool const converged = net.minValidated() >= 2;

        log << "  virtual ticks=" << ticks << " (cap " << kMaxTicks << ")"
            << " validated: node0="
            << net[0].app().getLedgerMaster().getValidLedgerIndex()
            << " node1="
            << net[1].app().getLedgerMaster().getValidLedgerIndex() << std::endl;

        BEAST_EXPECT(converged);
        BEAST_EXPECT(ticks < kMaxTicks);  // converged strictly within the bound
        if (converged)
            BEAST_EXPECT(net.ledgersAgree(net.minValidated()));
    }

    void
    testVirtualQuorumLoss()
    {
        testcase(
            "L: partition under virtual time deterministically loses quorum "
            "(validation stalls — the bless-the-tip precondition)");
        using namespace std::chrono_literals;

        // Rung K's 2-validator virtual-time setup, but PARTITIONED mid-run. With
        // UNL size 2 -> quorum 2, severing the only link makes quorum unreachable
        // for either node, so NO further ledger can fully-validate. Under virtual
        // time this is DETERMINISTIC (a fixed tick budget) — unlike the wall-clock
        // version that was deferred from S1.3c as inherently flaky.
        MultiNode net(*this, /*virtualClock=*/true);

        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(TrustConfig{valA.seed, unl}, simFactory);
        net.add(TrustConfig{valB.seed, unl}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        auto wire = simConnect(net[0].app(), net[1].app());
        if (!BEAST_EXPECT(net.waitForPeers(1, 20s)))
            return;

        // Phase 1 — quorum intact: converge.
        constexpr std::size_t kMaxTicks = 200;
        net.runVirtual(/*target=*/2, kMaxTicks, 1s);
        if (!BEAST_EXPECT(net.minValidated() >= 2))
            return;

        // Phase 2 — partition: sever the bus, let the peer drop settle.
        wire->sever();
        for (int i = 0; i < 5; ++i)
            net.tick(1s);
        BEAST_EXPECT(
            net[0].app().overlay().size() == 0 &&
            net[1].app().overlay().size() == 0);
        auto const partitionedIdx = net.minValidated();

        // Phase 3 — quorum lost: a generous virtual-time budget must NOT advance
        // any node's FULLY-VALIDATED ledger (no quorum -> no validation).
        constexpr std::size_t kStallTicks = 30;
        for (std::size_t i = 0; i < kStallTicks; ++i)
            net.tick(1s);

        auto const v0 = net[0].app().getLedgerMaster().getValidLedgerIndex();
        auto const v1 = net[1].app().getLedgerMaster().getValidLedgerIndex();
        auto const maxAfter = v0 > v1 ? v0 : v1;
        log << "  converged=" << partitionedIdx << ", after " << kStallTicks
            << " partitioned ticks: node0=" << v0 << " node1=" << v1 << std::endl;

        // The bless-the-tip precondition, deterministically: quorum loss stalls
        // full validation — neither node advances past the partition point.
        BEAST_EXPECT(maxAfter == partitionedIdx);
    }

    void
    testVirtualReproducible()
    {
        testcase(
            "M: validated-ledger hashes are reproducible run-to-run "
            "(deterministic under virtual time)");
        using namespace std::chrono_literals;

        // Run the SAME virtual-time 2-validator convergence twice (fresh nodes
        // each time) and compare the validated-ledger hash at a fixed seq. For
        // empty ledgers the hash depends only on prev-hash + (empty) tx tree +
        // (deterministic) state tree + closeTime; closeTime is driven by the
        // shared manual clock (deterministic), and cookies (PRNG) are validation
        // metadata NOT hashed into the ledger. So the hash should be IDENTICAL
        // across runs even though job/io threading is not single-threaded.
        auto runOnce = [this]() -> uint256 {
            MultiNode net(*this, /*virtualClock=*/true);
            auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
            auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
            std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
            auto simFactory = [](Application& app) {
                return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
            };
            net.add(TrustConfig{valA.seed, unl}, simFactory);
            net.add(TrustConfig{valB.seed, unl}, simFactory);
            if (!net.allUp())
                return uint256{};
            simConnect(net[0].app(), net[1].app());
            if (!net.waitForPeers(1, 20s))
                return uint256{};
            net.runVirtual(/*target=*/3, 200, 1s);
            auto const l = net[0].app().getLedgerMaster().getLedgerBySeq(2);
            return l ? l->header().hash : uint256{};
        };

        auto const h1 = runOnce();
        auto const h2 = runOnce();
        log << "  validated seq-2 hash run1==run2: "
            << ((h1 == h2 && h1 != uint256{}) ? "MATCH" : "DIFFER") << std::endl;
        BEAST_EXPECT(h1 != uint256{});  // both runs validated seq 2
        BEAST_EXPECT(h1 == h2);         // reproducible run-to-run
    }

    // A restarted node is rebuilt on its stable slot (same database directory,
    // same identity). The two restart entry points differ ONLY in the startup
    // policy handed to Application::setup: restartNode loads the latest saved
    // ledger (StartUpType::Load, "latest"); restartNodeFresh ignores the saved
    // ledgers and starts from genesis. Same database, opposite outcome — so a
    // harness that silently forwards the wrong policy fails the fresh leg.
    void
    testRestartLoadPolicy()
    {
        testcase(
            "M: restart policy — ordinary restart resumes at the latest saved "
            "ledger, fresh restart of the SAME database starts at genesis");
        using namespace std::chrono_literals;
        MultiNode net(*this, /*virtualClock=*/true);

        auto const valA = ValidatorKey::fromPassphrase("harness-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("harness-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(TrustConfig{valA.seed, unl}, simFactory);
        net.add(TrustConfig{valB.seed, unl}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // The genesis-mode closed seq, captured before any ledger closes: the
        // self-calibrating "fresh" oracle (genesis + its first successor).
        auto const genesisClosed = net.closedSeq(1);
        auto const dbPath = net.databasePath(1);

        auto wire = net.simConnect(0, 1);
        if (!BEAST_EXPECT(net.waitForPeers(1, 20s)))
            return;
        constexpr std::size_t kMaxTicks = 200;
        // Validated >= 4 keeps LoadLatest (closed >= 4) and Fresh (closed 2)
        // unambiguously apart; a lower target can make the oracle vacuous.
        net.runVirtual(/*target=*/4, kMaxTicks, 1s);
        if (!BEAST_EXPECT(net.minValidated() >= 4))
            return;
        auto const validatedBefore = net.validSeq(1);
        BEAST_EXPECT(validatedBefore > genesisClosed);

        // The validated ledger is saved to SQL by a posted job; loadOldLedger
        // ("latest") reads that table, so wait for the row before stopping.
        if (!BEAST_EXPECT(waitUntil(
                [&] {
                    auto const max = net[1].app().getRelationalDatabase().getMaxLedgerSeq();
                    return max && *max >= validatedBefore;
                },
                10s)))
            return;

        // Ordinary restart: latest saved ledger loaded → the closed ledger
        // resumes at or above the validated seq, before any reconnect or tick.
        wire->sever();
        net.stopNode(1);
        if (!BEAST_EXPECT(net.restartNode(1).isUp()))
            return;
        auto const resumed = net.closedSeq(1);
        log << "  genesisClosed=" << genesisClosed << " validatedBefore=" << validatedBefore
            << " resumed=" << resumed << std::endl;
        BEAST_EXPECT(resumed >= validatedBefore);
        BEAST_EXPECT(net.databasePath(1) == dbPath);

        // Fresh restart of the SAME database: saved ledgers ignored, closed
        // ledger is genesis again. This is the leg a dropped policy flag fails.
        net.stopNode(1);
        if (!BEAST_EXPECT(net.restartNodeFresh(1).isUp()))
            return;
        auto const fresh = net.closedSeq(1);
        log << "  fresh=" << fresh << std::endl;
        BEAST_EXPECT(fresh == genesisClosed);
        BEAST_EXPECT(fresh < validatedBefore);
        BEAST_EXPECT(net.databasePath(1) == dbPath);
    }

    // The intended restartNodeFresh use case: a node that never validated a
    // loadable ledger (no validator identity, no peers) must come back up on
    // its stable slot and sit at genesis.
    void
    testFreshRestartWithoutLoadableLedger()
    {
        testcase("N: fresh restart of a node that never validated a loadable ledger");
        MultiNode net(*this, /*virtualClock=*/true);
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(std::nullopt, simFactory);
        if (!BEAST_EXPECT(net[0].isUp()))
            return;
        auto const genesisClosed = net.closedSeq(0);
        auto const dbPath = net.databasePath(0);
        log << "  genesisClosed=" << genesisClosed << " validSeq=" << net.validSeq(0) << std::endl;

        net.stopNode(0);
        if (!BEAST_EXPECT(net.restartNodeFresh(0).isUp()))
            return;
        BEAST_EXPECT(net.closedSeq(0) == genesisClosed);
        BEAST_EXPECT(net.databasePath(0) == dbPath);
    }

public:
    void
    run() override
    {
        testSingleNodeBringUp();
        testTwoNodesCoexist();
        testTwoNodesPeer();
        testTwoNodesConverge();
        testThreeNodesConverge();
        testSimOverlayStandup();
        testSimTransportBus();
        testSimOverlayEstablish();
        testSimOverlayConverge();
        testSimPartition();
        testVirtualConverge();
        testVirtualQuorumLoss();
        testVirtualReproducible();
        testRestartLoadPolicy();
        testFreshRestartWithoutLoadableLedger();
    }
};

BEAST_DEFINE_TESTSUITE(HarnessNet, consensus, xrpl);

}  // namespace ripple::test
