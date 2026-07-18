#ifndef RIPPLE_APP_PATHS_PAYGRAPH_H_INCLUDED
#define RIPPLE_APP_PATHS_PAYGRAPH_H_INCLUDED

//------------------------------------------------------------------------------
/*
    PayGraph — Persistent, incrementally-updated asset-exchange graph for
               XRPL pathfinding.

    Ported from rippled PR #7392 ("Pathfinding 2.0 using Dijkstra's Algorithm").
    Adapted to xahaud: Issue-based (no MPT / no permissioned-domain support),
    namespace ripple.

    LIFETIME
    --------
    PayGraph is created ONCE at startup (or after a long catchup) and lives
    for the duration of the process.  It is NOT rebuilt per ledger.  Instead,
    applyLedgerDelta() is called at each ledger close — typically updating
    fewer than 100 edges in a few microseconds.

    ASSET-EXCHANGE GRAPH
    --------------------
    Vertices = distinct assets  (IOU {currency,issuer}, XRP)
    Edges    = order books (and AMM-derived books) between asset pairs
    Scale    = a few hundred vertices, ~1 000 edges on mainnet -> trivially
               small.

    Trust-line rippling between issuers of the same currency is handled
    implicitly by the XRPL payment engine (rippleCalculate) and does not
    require explicit traversal here.

    SNAPSHOT / COPY-ON-WRITE MODEL
    --------------------------------
    Pathfinding threads and the ledger-close thread access the graph
    concurrently.  We use an atomic shared_ptr to an immutable Snapshot:

        Pathfinder thread:
            auto snap = graph.snapshot();
            // Use snap freely -- no locks held.

        Ledger-close thread:
            graph.applyLedgerDelta(newLedger, changedBooks);
            // Copies current snapshot, patches changed edges O(C),
            // atomically publishes new snapshot.  Readers already
            // holding the old snapshot are unaffected.

    Since the entire graph fits in ~50 KB, copying on each ledger close
    is negligible.  Pathfinders hold zero locks during search.
*/
//------------------------------------------------------------------------------

#include <xrpld/app/ledger/OrderBookDB.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/UintTypes.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace ripple {

class PayGraph
{
public:
    //--------------------------------------------------------------------------
    // Public types
    //--------------------------------------------------------------------------

    /// Opaque vertex identifier.  Stable across incremental updates
    /// (vertices are never removed once added; only edge weights change).
    using VID = std::uint32_t;
    static constexpr VID kNull = ~VID{0};

    /// How value flows across this edge.
    enum class EdgeKind : std::uint8_t {
        OrderBook,  ///< Best offer in an order book
        AMM,        ///< Constant-product AMM pool
    };

    /// One directed edge in the asset-exchange graph.
    struct Edge
    {
        VID to{};                      ///< Receiving-asset vertex
        std::uint32_t qualityFixed{};  ///< Top-of-book rate as fixed-point.
                                       ///  Lower = cheaper.  Used additively as
                                       ///  the Dijkstra edge weight.
        EdgeKind kind{};
        std::uint8_t pad[3]{};
    };

    static_assert(sizeof(Edge) == 12, "Edge must be 12 bytes");

    /// Sentinel quality: edge structurally exists but has no current offers.
    /// Dijkstra traverses these at maximum cost so they rank last; structural
    /// presence is preserved because offers may exist in the ledger that
    /// weren't visible when the snapshot was built.
    static constexpr std::uint32_t kNoLiquidity = 0xFFFF'FFFFu;

    //--------------------------------------------------------------------------
    // Abstract path through the asset-exchange graph.
    //   vids[0]       = source asset
    //   vids[1..n-2]  = bridge assets
    //   vids[n-1]     = destination asset
    //--------------------------------------------------------------------------
    struct AssetPath
    {
        std::vector<VID> vids;
        std::uint64_t cumQuality{};  ///< Sum of qualityFixed (lower = better)
    };

    //--------------------------------------------------------------------------
    // Diagnostic counters
    //--------------------------------------------------------------------------
    struct Stats
    {
        std::uint32_t vertices{};
        std::uint32_t edges{};
        std::uint32_t orderBooks{};
        std::uint32_t ammPools{};
        std::uint32_t lastDeltaBooks{};  ///< Books patched in most recent delta
        std::uint32_t totalDeltasCalled{};  ///< Cumulative applyLedgerDelta()
    };

    //--------------------------------------------------------------------------
    // Immutable graph snapshot
    //--------------------------------------------------------------------------
    struct Snapshot
    {
        /// Adjacency list indexed by VID.  adj[v] = outgoing edges from v.
        std::vector<std::vector<Edge>> adj;

        /// Vertex -> Issue mapping (adj.size() == assets.size()).
        std::vector<Issue> assets;

        /// Issue -> VID for O(1) lookup.
        hash_map<Issue, VID> index;

        Stats stats;
    };

    //--------------------------------------------------------------------------
    // Lifecycle
    //--------------------------------------------------------------------------

    /// Full build from scratch.  Called once at startup (or post-catchup).
    static std::shared_ptr<PayGraph>
    build(OrderBookDB& bookDB, ReadView const& ledger, beast::Journal j);

    PayGraph(PayGraph const&) = delete;
    PayGraph&
    operator=(PayGraph const&) = delete;
    ~PayGraph() = default;

    //--------------------------------------------------------------------------
    // Incremental update -- call at each ledger close.
    //--------------------------------------------------------------------------
    void
    applyLedgerDelta(
        OrderBookDB& bookDB,
        ReadView const& newLedger,
        std::vector<Book> const& changedBooks);

    /// Full rebuild.  Safe to call at any time; replaces the snapshot
    /// atomically like applyLedgerDelta() does.
    void
    rebuild(OrderBookDB& bookDB, ReadView const& ledger);

    //--------------------------------------------------------------------------
    // Snapshot access for pathfinding threads
    //--------------------------------------------------------------------------
    std::shared_ptr<Snapshot const>
    snapshot() const;

    //--------------------------------------------------------------------------
    // Vertex helpers (operate on current snapshot)
    //--------------------------------------------------------------------------
    VID
    vertexOf(Issue const& asset) const;

    Issue const&
    assetOf(VID v) const;

    //--------------------------------------------------------------------------
    // K-Shortest asset paths  (Yen's algorithm over Dijkstra)
    //--------------------------------------------------------------------------
    static std::vector<AssetPath>
    kShortestPaths(Snapshot const& snap, VID src, VID dst, int k);

    /// Convenience: grab current snapshot and run kShortestPaths.
    std::vector<AssetPath>
    findPaths(Issue const& src, Issue const& dst, int k) const;

    //--------------------------------------------------------------------------
    Stats
    currentStats() const;

private:
    explicit PayGraph(beast::Journal j);

    //--------------------------------------------------------------------------
    // Build / patch helpers
    //--------------------------------------------------------------------------

    /// Allocate a Snapshot populated from bookDB + ledger (no atomic store).
    static std::shared_ptr<Snapshot>
    buildSnapshot(OrderBookDB& bookDB, ReadView const& ledger, beast::Journal j);

    /// Ensure a vertex for 'asset' exists in snap.  Returns its VID.
    static VID
    ensureVertex(Snapshot& snap, Issue const& asset);

    /// Find or create the directed edge (from -> to) of the given kind in snap.
    static Edge&
    ensureEdge(Snapshot& snap, VID from, VID to, EdgeKind kind);

    /// Query top-of-book quality from the ledger for a given order book.
    /// Returns kNoLiquidity when no offers remain in the book.
    static std::uint32_t
    topOfBookQuality(ReadView const& ledger, Book const& book);

    //--------------------------------------------------------------------------
    // Dijkstra internals
    //--------------------------------------------------------------------------
    struct DijkResult
    {
        std::vector<std::uint64_t> dist;  ///< min cumulative cost from src
        std::vector<VID> prev;            ///< predecessor (kNull = none)
    };

    /// A set of directed edges (from, to) to treat as absent during Dijkstra.
    using BlockedEdges = std::vector<std::pair<VID, VID>>;

    static DijkResult
    dijkstra(
        Snapshot const& snap,
        VID src,
        std::vector<bool> const* blockedVerts = nullptr,
        BlockedEdges const* blockedEdges = nullptr);

    static std::vector<VID>
    reconstructPath(DijkResult const& res, VID src, VID dst);

    //--------------------------------------------------------------------------
    // State
    //--------------------------------------------------------------------------

    /// Current snapshot.  Read via snapshot(); written only by writeMu_ holder.
    mutable std::shared_ptr<Snapshot const> snap_;

    /// Serialises applyLedgerDelta() / rebuild() calls.
    /// Never held during pathfinding.
    std::mutex writeMu_;

    beast::Journal j_;
};

}  // namespace ripple

#endif
