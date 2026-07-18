#ifndef RIPPLE_APP_PATHS_GRAPHPATHFINDER_H_INCLUDED
#define RIPPLE_APP_PATHS_GRAPHPATHFINDER_H_INCLUDED

//------------------------------------------------------------------------------
/*
    GraphPathfinder — Drop-in replacement for Pathfinder that uses PayGraph.

    Ported from rippled PR #7392 ("Pathfinding 2.0 using Dijkstra's Algorithm")
    and adapted to xahaud (Issue-based, namespace ripple, no MPT / permissioned
    domains, RippleLineCache).

    DESIGN
    ------
    Instead of BFS over the combined account+asset space, GraphPathfinder:

      1. Looks up the pre-built PayGraph for the current ledger.
      2. Runs Yen's K-Shortest (via PayGraph::kShortestPaths) on the tiny
         asset-exchange graph to get <=6 abstract asset-type paths.
      3. For each abstract path, "materialises" it into a concrete STPath.
      4. Passes the materialised STPathSet to rippleCalculate for final
         liquidity confirmation and quality scoring.
      5. Results are returned immediately; each successive call can augment
         the previous result — maintaining the existing progressive-refinement
         WebSocket contract.

    GraphPathfinder exposes the same public interface shape as Pathfinder so it
    can be swapped in with minimal changes to PathRequest.cpp.
*/
//------------------------------------------------------------------------------

#include <xrpld/app/paths/PayGraph.h>
#include <xrpld/app/paths/RippleLineCache.h>

#include <xrpl/basics/CountedObject.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/protocol/UintTypes.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ripple {

class Application;
class ReadView;

class GraphPathfinder : public CountedObject<GraphPathfinder>
{
public:
    //--------------------------------------------------------------------------
    // Construction
    //
    // Mirrors the Pathfinder constructor signature (with a PayGraph prepended)
    // so PathRequest can instantiate either class with the same code path.
    //--------------------------------------------------------------------------
    GraphPathfinder(
        std::shared_ptr<PayGraph> const& graph,
        std::shared_ptr<RippleLineCache> const& cache,
        AccountID const& srcAccount,
        AccountID const& dstAccount,
        Currency const& srcCurrency,
        std::optional<AccountID> const& srcIssuer,
        STAmount const& dstAmount,
        std::optional<STAmount> const& srcAmount,
        Application& app);

    GraphPathfinder(GraphPathfinder const&) = delete;
    GraphPathfinder&
    operator=(GraphPathfinder const&) = delete;
    ~GraphPathfinder() = default;

    //--------------------------------------------------------------------------
    // findPaths — core entry point.
    //--------------------------------------------------------------------------
    bool
    findPaths(std::function<bool()> const& continueCallback = {});

    //--------------------------------------------------------------------------
    // computePathRanks — rank completePaths_ by quality and liquidity.
    //--------------------------------------------------------------------------
    void
    computePathRanks(
        int maxPaths,
        std::function<bool()> const& continueCallback = {});

    //--------------------------------------------------------------------------
    // getBestPaths — select up to maxPaths from the ranked candidates.
    //--------------------------------------------------------------------------
    STPathSet
    getBestPaths(
        int maxPaths,
        STPathSet const& extraPaths,
        AccountID const& srcIssuer,
        std::function<bool()> const& continueCallback = {});

    /** Number of concrete paths discovered by findPaths (for tests/diagnostics). */
    std::size_t
    completePathCount() const
    {
        return completePaths_.size();
    }

    //--------------------------------------------------------------------------
    // PathRank — identical layout to Pathfinder::PathRank.
    //--------------------------------------------------------------------------
    struct PathRank
    {
        std::uint64_t quality{};
        std::uint64_t length{};
        STAmount liquidity;
        int index{};
    };

private:
    //--------------------------------------------------------------------------
    // Materialise one abstract AssetPath into a concrete STPath.
    //--------------------------------------------------------------------------
    std::optional<STPath>
    materialise(PayGraph::AssetPath const& assetPath);

    //--------------------------------------------------------------------------
    // Compute liquidity for a single path using rippleCalculate.
    //--------------------------------------------------------------------------
    TER
    getPathLiquidity(
        STPath const& path,
        STAmount const& minDstAmount,
        STAmount& amountOut,
        std::uint64_t& qualityOut) const;

    //--------------------------------------------------------------------------
    // Rank paths (fills pathRanks_ from completePaths_).
    //--------------------------------------------------------------------------
    void
    rankPaths(
        int maxPaths,
        STPathSet const& paths,
        std::vector<PathRank>& rankedPaths,
        std::function<bool()> const& continueCallback);

    //--------------------------------------------------------------------------
    // Member data
    //--------------------------------------------------------------------------

    std::shared_ptr<PayGraph> graph_;
    std::shared_ptr<PayGraph::Snapshot const> snap_;  // stable view for request

    AccountID srcAccount_;
    AccountID dstAccount_;
    AccountID effectiveDst_;
    STAmount dstAmount_;
    Currency srcCurrency_;
    std::optional<AccountID> srcIssuer_;
    STAmount srcAmount_;
    bool convertAll_;

    std::shared_ptr<ReadView const> ledger_;
    std::shared_ptr<RippleLineCache> cache_;

    STPathSet completePaths_;
    std::vector<PathRank> pathRanks_;
    STAmount remainingAmount_;

    Application& app_;
    beast::Journal j_;
};

}  // namespace ripple

#endif
