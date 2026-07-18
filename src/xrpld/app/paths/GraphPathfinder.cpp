//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    GraphPathfinder implementation. Ported from rippled PR #7392 and adapted to
    xahaud (Issue-based, namespace ripple, no MPT / permissioned domains,
    RippleLineCache).
*/
//------------------------------------------------------------------------------

#include <xrpld/app/paths/GraphPathfinder.h>

#include <xrpld/app/main/Application.h>
#include <xrpld/app/paths/RippleCalc.h>
#include <xrpld/app/paths/RippleLineCache.h>
#include <xrpld/app/paths/TrustLine.h>
#include <xrpld/app/paths/detail/PathfinderUtils.h>
#include <xrpld/app/paths/detail/Steps.h>
#include <xrpld/ledger/PaymentSandbox.h>

#include <xrpl/basics/Log.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STPathSet.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/UintTypes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ripple {

//==============================================================================
// Construction
//==============================================================================

GraphPathfinder::GraphPathfinder(
    std::shared_ptr<PayGraph> const& graph,
    std::shared_ptr<RippleLineCache> const& cache,
    AccountID const& srcAccount,
    AccountID const& dstAccount,
    Currency const& srcCurrency,
    std::optional<AccountID> const& srcIssuer,
    STAmount const& dstAmount,
    std::optional<STAmount> const& srcAmount,
    Application& app)
    : graph_(graph)
    , snap_(graph ? graph->snapshot() : nullptr)
    , srcAccount_(srcAccount)
    , dstAccount_(dstAccount)
    , effectiveDst_(
          isXRP(dstAmount.getIssuer()) ? dstAccount : dstAmount.getIssuer())
    , dstAmount_(dstAmount)
    , srcCurrency_(srcCurrency)
    , srcIssuer_(srcIssuer)
    , srcAmount_(srcAmount.value_or(STAmount(
          Issue{
              srcCurrency,
              srcIssuer.value_or(
                  isXRP(srcCurrency) ? xrpAccount() : srcAccount)},
          1u,
          0,
          true)))
    , convertAll_(convertAllCheck(dstAmount))
    , ledger_(cache->getLedger())
    , cache_(cache)
    , app_(app)
    , j_(app.journal("GraphPathfinder"))
{
}

//==============================================================================
// findPaths
//
// Phase 1: run Yen's K-Shortest on the asset-exchange graph (microseconds).
// Phase 2: materialise each abstract path into a concrete STPath.
//==============================================================================

bool
GraphPathfinder::findPaths(std::function<bool()> const& continueCallback)
{
    if (!snap_ || !ledger_)
    {
        JLOG(j_.debug()) << "GraphPathfinder: no snapshot or ledger";
        return false;
    }

    if (dstAmount_.signum() == 0)
    {
        JLOG(j_.debug()) << "GraphPathfinder: zero destination amount";
        return false;
    }

    // Resolve source and destination assets.
    // Prefer srcAmount_ (often saSendMax with the real issuer) over reconstructing
    // from currency + srcAccount, which mis-attributes IOU issuers and misses
    // order-book vertices (e.g. receive-max USD→XRP).
    Issue const srcAsset = srcAmount_.issue();
    Issue const dstAsset = dstAmount_.issue();

    if (srcAccount_ == dstAccount_ && srcAsset == dstAsset)
    {
        JLOG(j_.debug()) << "GraphPathfinder: trivial same-account same-asset";
        return true;
    }

    // kMaxK = 6: we want at most 6 paths for the subscriber.
    static constexpr int kMaxK = 6;

    // Resolve graph vertex IDs for source and destination.
    //
    // The exact lookup by {currency, issuer} may fail for non-XRP sources when
    // the sender's account is used as the issuer.  For the source, fall back to
    // a currency-only search constrained to issuers that the source account
    // actually holds trust lines for — this prevents false-positive paths.
    //
    // For the destination we do an unconstrained currency-only fallback: the
    // suffix logic in materialise() handles the trust-line hops from the OB
    // gateway to the actual destination account.
    auto findVIDsSrc = [&](Issue const& asset) -> std::vector<PayGraph::VID> {
        auto it = snap_->index.find(asset);
        if (it != snap_->index.end())
            return {it->second};

        if (!isXRP(asset.currency))
        {
            Currency const& cur = asset.currency;

            // Collect issuers that srcAccount_ has trust lines for.
            hash_set<AccountID> validIssuers;
            if (auto const lines =
                    cache_->getRippleLines(srcAccount_, LineDirection::outgoing))
            {
                for (auto const& line : *lines)
                {
                    if (line.getBalance().getCurrency() != cur)
                        continue;
                    validIssuers.insert(line.getAccountIDPeer());
                }
            }

            std::vector<PayGraph::VID> vids;
            for (auto const& [a, vid] : snap_->index)
            {
                if (isXRP(a.currency))
                    continue;
                if (a.currency != cur)
                    continue;
                if (!validIssuers.empty() && !validIssuers.contains(a.account))
                    continue;
                vids.push_back(vid);
            }
            return vids;
        }
        return {};
    };

    auto findVIDsDst = [&](Issue const& asset) -> std::vector<PayGraph::VID> {
        auto it = snap_->index.find(asset);
        if (it != snap_->index.end())
            return {it->second};

        // Unconstrained currency-only fallback for destination — materialise()
        // suffix adds the intermediate trust-line hops to dstAccount_.
        if (!isXRP(asset.currency))
        {
            Currency const& cur = asset.currency;
            std::vector<PayGraph::VID> vids;
            for (auto const& [a, vid] : snap_->index)
            {
                if (!isXRP(a.currency) && a.currency == cur)
                    vids.push_back(vid);
            }
            return vids;
        }
        return {};
    };

    auto const srcVIDs = findVIDsSrc(srcAsset);
    auto const dstVIDs = findVIDsDst(dstAsset);

    // When the effective destination is the sender AND the source/destination
    // assets are the same (a genuine self-loop on a single asset), skip Yen's.
    bool const repayToSelf = !isXRP(dstAsset) &&
        (effectiveDst_ == srcAccount_) && (srcAsset == dstAsset);

    if (repayToSelf || srcVIDs.empty() || dstVIDs.empty())
    {
        JLOG(j_.debug()) << "GraphPathfinder: "
                         << (repayToSelf ? "repay-to-self, skipping PayGraph"
                                         : "src or dst asset not in graph");
    }
    else
    {
        // Yen's k-shortest paths between every (srcVID, dstVID) pair, sorted
        // by ascending cumQuality.  Materialise into concrete STPaths and
        // dedupe.
        //
        // Light oversample: rankPaths() runs expensive rippleCalculate per
        // candidate (and AMM overflows throw FlowException with stack dumps).
        // Keep the probe set small so startup/updateAll stays bounded.
        // Ported from rippled pathfinding-2.
        static constexpr int kOversample = 2;
        std::vector<PayGraph::AssetPath> candidates;
        for (PayGraph::VID const vSrc : srcVIDs)
        {
            for (PayGraph::VID const vDst : dstVIDs)
            {
                if (vSrc == vDst)
                    continue;
                auto paths = PayGraph::kShortestPaths(
                    *snap_, vSrc, vDst, kMaxK * kOversample);
                for (auto& p : paths)
                    candidates.push_back(std::move(p));
            }
        }

        std::stable_sort(
            candidates.begin(),
            candidates.end(),
            [](auto const& a, auto const& b) {
                return a.cumQuality < b.cumQuality;
            });

        int accepted = 0;
        // Materialise at most kMaxK concrete paths; ranking will probe these.
        int const acceptCap = kMaxK;
        for (auto const& ap : candidates)
        {
            if (accepted >= acceptCap)
                break;
            if (continueCallback && !continueCallback())
                return !completePaths_.empty();

            // Broken AMMs are excluded inside BookStep::getAMMOffer after a
            // single FlowException (noteFailedAMM).  Do not skip entire asset
            // paths here — CLOB liquidity on the same book must stay eligible.

            auto concrete = materialise(ap);
            if (!concrete || concrete->empty())
                continue;

            bool dup = false;
            for (auto const& existing : completePaths_)
            {
                if (existing == *concrete)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;

            completePaths_.push_back(*concrete);
            ++accepted;
        }

        JLOG(j_.debug()) << "GraphPathfinder: " << candidates.size()
                         << " abstract paths considered, " << accepted
                         << " concrete paths accepted (cap=" << acceptCap << ")";
    }

    // Also try the direct (no-bridge) path: src -> dst over trust lines.
    // This covers same-currency IOUs that don't go through any order book.
    if (srcAsset == dstAsset && !isXRP(srcAsset))
    {
        // Direct ripple: no path nodes needed. An empty completePath_ entry
        // signals "try the default path".
        completePaths_.push_back(STPath{});
    }

    // Phase 2: trust-line rippling paths.
    //
    // PayGraph only models order-book edges. Pure trust-line rippling paths
    // (e.g. alice -> gateway -> bob all in the same currency) require a
    // separate discovery step.  Only applicable when the source payment asset
    // is the same IOU currency as the destination.
    bool const srcIsTargetCcy = !isXRP(srcCurrency_) && !isXRP(dstAsset) &&
        srcCurrency_ == dstAsset.currency;

    if (srcIsTargetCcy)
    {
        Currency const& targetCcy = dstAsset.currency;

        auto const srcLines =
            cache_->getRippleLines(srcAccount_, LineDirection::outgoing);
        auto const dstLines =
            cache_->getRippleLines(dstAccount_, LineDirection::outgoing);

        if (srcLines && dstLines)
        {
            // Collect src peers in targetCcy (exclude frozen lines and the
            // src/dst accounts themselves).
            hash_set<AccountID> srcPeers;
            for (auto const& line : *srcLines)
            {
                if (line.getBalance().getCurrency() != targetCcy)
                    continue;
                if (line.getFreeze() || line.getDeepFreeze())
                    continue;
                AccountID const& peer = line.getAccountIDPeer();
                if (peer != srcAccount_ && peer != dstAccount_)
                    srcPeers.insert(peer);
            }

            // Collect dst peers in targetCcy.
            hash_set<AccountID> dstPeers;
            for (auto const& line : *dstLines)
            {
                if (line.getBalance().getCurrency() != targetCcy)
                    continue;
                if (line.getFreeze() || line.getDeepFreeze())
                    continue;
                AccountID const& peer = line.getAccountIDPeer();
                if (peer != srcAccount_ && peer != dstAccount_)
                    dstPeers.insert(peer);
            }

            // 1-hop: src -> I -> dst.  I must be in both srcPeers and dstPeers.
            for (auto const& i : srcPeers)
            {
                if (continueCallback && !continueCallback())
                    return !completePaths_.empty();
                if (dstPeers.contains(i))
                {
                    STPath path;
                    path.emplace_back(
                        STPathElement::typeAccount,
                        i,
                        xrpCurrency(),
                        xrpAccount());
                    completePaths_.push_back(path);
                }
            }

            // 2-hop: src -> A -> B -> dst.  A in srcPeers, B in dstPeers,
            // A != B.  We probe the A-B trust line via a single SHAMap
            // point-lookup rather than loading A's full trust-line list.
            [&] {
                static constexpr std::size_t kMaxHop2Probes = 100;
                std::size_t probes = 0;
                for (auto const& a : srcPeers)
                {
                    if (continueCallback && !continueCallback())
                        return;
                    for (auto const& b : dstPeers)
                    {
                        if (probes++ >= kMaxHop2Probes)
                            return;
                        if (a == b)
                            continue;
                        if (ledger_->read(keylet::line(a, b, targetCcy)))
                        {
                            STPath path;
                            path.emplace_back(
                                STPathElement::typeAccount,
                                a,
                                xrpCurrency(),
                                xrpAccount());
                            path.emplace_back(
                                STPathElement::typeAccount,
                                b,
                                xrpCurrency(),
                                xrpAccount());
                            completePaths_.push_back(path);
                        }
                    }
                }
            }();

            // 3-hop: src -> A -> C -> B -> dst.  A in srcPeers; C is a peer of
            // A (loaded from cache, capped to avoid expanding large gateway
            // trust-line lists); B in dstPeers.
            [&] {
                static constexpr std::size_t kMaxGatewayPeers = 50;
                static constexpr std::size_t kMaxHop3Probes = 200;
                std::size_t probes = 0;
                for (auto const& a : srcPeers)
                {
                    if (continueCallback && !continueCallback())
                        return;
                    auto const aLines =
                        cache_->getRippleLines(a, LineDirection::outgoing);
                    if (!aLines || aLines->size() > kMaxGatewayPeers)
                        continue;
                    for (auto const& aLine : *aLines)
                    {
                        if (aLine.getBalance().getCurrency() != targetCcy)
                            continue;
                        if (aLine.getFreeze() || aLine.getDeepFreeze())
                            continue;
                        AccountID const& c = aLine.getAccountIDPeer();
                        if (c == srcAccount_ || c == dstAccount_)
                            continue;
                        if (dstPeers.contains(c))
                            continue;  // already covered by 1-hop
                        for (auto const& b : dstPeers)
                        {
                            if (probes++ >= kMaxHop3Probes)
                                return;
                            if (c == b || c == a)
                                continue;
                            if (ledger_->read(keylet::line(c, b, targetCcy)))
                            {
                                STPath path;
                                path.emplace_back(
                                    STPathElement::typeAccount,
                                    a,
                                    xrpCurrency(),
                                    xrpAccount());
                                path.emplace_back(
                                    STPathElement::typeAccount,
                                    c,
                                    xrpCurrency(),
                                    xrpAccount());
                                path.emplace_back(
                                    STPathElement::typeAccount,
                                    b,
                                    xrpCurrency(),
                                    xrpAccount());
                                completePaths_.push_back(path);
                            }
                        }
                    }
                }
            }();
        }
    }

    JLOG(j_.debug()) << "GraphPathfinder: " << completePaths_.size()
                     << " concrete paths"
                     << " src=" << to_string(srcAsset)
                     << " dst=" << to_string(dstAsset);
    return true;
}

//==============================================================================
// materialise — convert an abstract AssetPath into a concrete STPath.
//
// An AssetPath is a sequence of asset vertices in the exchange graph:
//   assetPath.vids = [srcAsset, bridge1, bridge2, ..., dstAsset]
//
// For each consecutive pair (A, B) that crosses an order book we emit an offer
// node (currency+issuer only, no account).  The XRPL payment engine fills in
// the best offer.  The path does NOT include the source or destination
// accounts; those are implicit in the payment.
//==============================================================================

std::optional<STPath>
GraphPathfinder::materialise(PayGraph::AssetPath const& assetPath)
{
    if (assetPath.vids.size() < 2)
        return std::nullopt;

    PayGraph::VID const firstVID = assetPath.vids.front();
    PayGraph::VID const lastVID = assetPath.vids.back();
    if (firstVID >= snap_->assets.size() || lastVID >= snap_->assets.size())
        return std::nullopt;

    STPath path;

    // Prefix: when the PayGraph path's first vertex is issued by a gateway G
    // that differs from both the sender and the caller's explicit srcIssuer,
    // add G as an account node so rippleCalc can ripple through it.
    {
        Issue const& srcAsset = snap_->assets[firstVID];
        if (!isXRP(srcAsset))
        {
            AccountID const& g = srcAsset.account;
            AccountID const expectedIssuer = srcIssuer_.value_or(srcAccount_);
            if (g != srcAccount_ && g != xrpAccount() && g != expectedIssuer)
                path.emplace_back(
                    STPathElement::typeAccount,
                    g,
                    xrpCurrency(),
                    xrpAccount());
        }
    }

    for (std::size_t i = 0; i + 1 < assetPath.vids.size(); ++i)
    {
        PayGraph::VID const vTo = assetPath.vids[i + 1];

        if (vTo >= snap_->assets.size())
            return std::nullopt;

        Issue const& toAsset = snap_->assets[vTo];

        // Offer-book crossing: emit a node that has only the receiving asset's
        // currency and issuer.  The account field is the xrpAccount() sentinel
        // per XRPL convention for offer nodes.
        if (isXRP(toAsset))
        {
            path.emplace_back(
                STPathElement::typeCurrency,
                xrpAccount(),
                xrpCurrency(),
                xrpAccount());
        }
        else
        {
            path.emplace_back(
                STPathElement::typeCurrency | STPathElement::typeIssuer,
                xrpAccount(),
                toAsset.currency,
                toAsset.account);
        }
    }

    if (path.empty())
        return std::nullopt;

    JLOG(j_.trace()) << "GraphPathfinder::materialise src="
                     << toBase58(srcAccount_) << " dst="
                     << toBase58(dstAccount_)
                     << " srcAsset=" << to_string(snap_->assets[firstVID])
                     << " dstAsset=" << to_string(snap_->assets[lastVID])
                     << " path=" << path.getJson(JsonOptions::none);

    // Suffix: when the path ends at an OB node whose asset is issued by a
    // gateway G that is not the payment's effective destination, the engine
    // needs account nodes to ripple from G to dstAccount_.
    {
        Issue const& dstAsset = snap_->assets[lastVID];
        if (!isXRP(dstAsset))
        {
            AccountID const& g = dstAsset.account;
            if (g != effectiveDst_ && g != dstAccount_)
            {
                Currency const& ccy = dstAsset.currency;
                // First add G itself.
                path.emplace_back(
                    STPathElement::typeAccount,
                    g,
                    xrpCurrency(),
                    xrpAccount());
                // If dstAccount_ does not hold G's IOU directly, look for an
                // intermediate account B that has trust lines with both G and
                // dstAccount_.
                if (!ledger_->read(keylet::line(g, dstAccount_, ccy)))
                {
                    auto const dstLines = ledger_->read(
                                              keylet::account(dstAccount_))
                        ? cache_->getRippleLines(
                              dstAccount_, LineDirection::outgoing)
                        : nullptr;
                    if (dstLines)
                    {
                        for (auto const& dl : *dstLines)
                        {
                            if (dl.getBalance().getCurrency() != ccy)
                                continue;
                            AccountID const& b = dl.getAccountIDPeer();
                            if (b == g || b == dstAccount_ || b == srcAccount_)
                                continue;
                            if (ledger_->read(keylet::line(g, b, ccy)))
                            {
                                path.emplace_back(
                                    STPathElement::typeAccount,
                                    b,
                                    xrpCurrency(),
                                    xrpAccount());
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    return path;
}

//==============================================================================
// getPathLiquidity — same logic as Pathfinder::getPathLiquidity
//==============================================================================

TER
GraphPathfinder::getPathLiquidity(
    STPath const& path,
    STAmount const& minDstAmount,
    STAmount& amountOut,
    std::uint64_t& qualityOut) const
{
    STPathSet pathSet;
    pathSet.push_back(path);

    path::RippleCalc::Input rcInput;
    rcInput.defaultPathsAllowed = false;

    PaymentSandbox sandbox(&*ledger_, tapNONE);

    try
    {
        if (convertAll_)
            rcInput.partialPaymentAllowed = true;

        // Use the actual destination amount for testing to better align with
        // payment execution. This helps identify paths that may not have
        // sufficient liquidity when the full amount is attempted.
        STAmount const testDstAmount = convertAll_ ? minDstAmount : dstAmount_;

        auto rc = path::RippleCalc::rippleCalculate(
            sandbox,
            srcAmount_,
            testDstAmount,  // Use actual destination amount
            dstAccount_,
            srcAccount_,
            pathSet,
            app_.logs(),
            &rcInput);

        // Broken AMMs are excluded inside BookStep::getAMMOffer after a single
        // FlowException (noteFailedAMM).  Subsequent polls skip that AMM only;
        // CLOB liquidity on the same book remains available.

        if (!isTesSuccess(rc.result()))
        {
            JLOG(j_.trace()) << "GraphPathfinder::getPathLiquidity failed: "
                             << transHuman(rc.result())
                             << " src=" << toBase58(srcAccount_)
                             << " dst=" << toBase58(dstAccount_) << " path="
                             << pathSet.getJson(JsonOptions::none);
            return rc.result();
        }

        qualityOut = getRate(rc.actualAmountOut, rc.actualAmountIn);
        amountOut = rc.actualAmountOut;

        if (!convertAll_)
        {
            rcInput.partialPaymentAllowed = true;
            rc = path::RippleCalc::rippleCalculate(
                sandbox,
                srcAmount_,
                dstAmount_ - amountOut,
                dstAccount_,
                srcAccount_,
                pathSet,
                app_.logs(),
                &rcInput);

            if (isTesSuccess(rc.result()))
                amountOut += rc.actualAmountOut;
        }

        return tesSUCCESS;
    }
    catch (FlowException const& e)
    {
        // Rare: exception escaped StrandFlow. Prefer the AMM attached to the
        // exception; do not blacklist every hop on the path.
        if (e.ammBook)
            noteFailedAMM(*e.ammBook, ledger_ ? ledger_->seq() : 0);
        JLOG(j_.info()) << "GraphPathfinder::getPathLiquidity FlowException: "
                        << e.what();
        return tefEXCEPTION;
    }
    catch (std::exception const& e)
    {
        JLOG(j_.info()) << "GraphPathfinder::getPathLiquidity exception: "
                        << e.what();
        return tefEXCEPTION;
    }
}

//==============================================================================
// rankPaths
//==============================================================================

namespace {

STAmount
smallestUsefulAmount(STAmount const& amount, int maxPaths)
{
    return divide(amount, STAmount(maxPaths + 2), amount.issue());
}

}  // namespace

void
GraphPathfinder::rankPaths(
    int maxPaths,
    STPathSet const& paths,
    std::vector<PathRank>& rankedPaths,
    std::function<bool()> const& continueCallback)
{
    rankedPaths.clear();
    rankedPaths.reserve(paths.size());

    auto const saMinDstAmount = [&]() -> STAmount {
        if (!convertAll_)
            return smallestUsefulAmount(dstAmount_, maxPaths);
        return largestAmount(dstAmount_);
    }();

    // Hard cap on expensive rippleCalculate probes (pathfinding-2).  After the
    // first FlowException for an AMM, BookStep skips that AMM so later evals
    // should not re-throw; these caps still bound worst case.
    int const maxProbes = std::max(maxPaths * 3, maxPaths + 6);
    int consecutiveFailures = 0;
    int probes = 0;

    for (int i = 0; i < static_cast<int>(paths.size()); ++i)
    {
        if (continueCallback && !continueCallback())
            return;

        auto const& currentPath = paths[i];
        if (currentPath.empty())
            continue;

        // Enough ranked paths for the caller — stop probing.
        if (static_cast<int>(rankedPaths.size()) >= maxPaths)
            break;

        if (++probes > maxProbes)
            break;

        STAmount liquidity;
        std::uint64_t quality = 0;
        if (isTesSuccess(
                getPathLiquidity(currentPath, saMinDstAmount, liquidity, quality)))
        {
            consecutiveFailures = 0;
            rankedPaths.push_back({quality, currentPath.size(), liquidity, i});
        }
        else
        {
            // Bail after a short run of dry paths once we already have enough
            // successes.  Do not abort early while still under maxPaths — trust
            // line alternatives often appear after a few dry OB candidates.
            if (++consecutiveFailures >= 3 &&
                static_cast<int>(rankedPaths.size()) >= maxPaths)
                break;
        }
    }

    std::sort(
        rankedPaths.begin(),
        rankedPaths.end(),
        [&](PathRank const& a, PathRank const& b) {
            if (!convertAll_ && a.quality != b.quality)
                return a.quality < b.quality;
            if (a.liquidity != b.liquidity)
                return a.liquidity > b.liquidity;
            if (a.length != b.length)
                return a.length < b.length;
            return a.index > b.index;
        });
}

//==============================================================================
// computePathRanks
//==============================================================================

void
GraphPathfinder::computePathRanks(
    int maxPaths,
    std::function<bool()> const& continueCallback)
{
    remainingAmount_ = convertAmount(dstAmount_, convertAll_);

    // Subtract the default-path contribution (same as Pathfinder).
    try
    {
        PaymentSandbox sandbox(&*ledger_, tapNONE);
        path::RippleCalc::Input rcInput;
        rcInput.partialPaymentAllowed = true;
        auto rc = path::RippleCalc::rippleCalculate(
            sandbox,
            srcAmount_,
            remainingAmount_,
            dstAccount_,
            srcAccount_,
            STPathSet(),
            app_.logs(),
            &rcInput);

        if (isTesSuccess(rc.result()))
            remainingAmount_ -= rc.actualAmountOut;
    }
    catch (std::exception const&)
    {
        JLOG(j_.debug()) << "GraphPathfinder: default path exception";
    }

    rankPaths(maxPaths, completePaths_, pathRanks_, continueCallback);
}

//==============================================================================
// getBestPaths — identical selection logic to Pathfinder::getBestPaths
//==============================================================================

STPathSet
GraphPathfinder::getBestPaths(
    int maxPaths,
    STPathSet const& extraPaths,
    AccountID const& srcIssuer,
    std::function<bool()> const& continueCallback)
{
    if (completePaths_.empty() && extraPaths.empty())
        return completePaths_;

    // GraphPathfinder materialises paths as offer/currency nodes only — it
    // never prepends the issuer's account node the way Pathfinder does.  The
    // XRPL payment engine implicitly handles the sender->issuer trust-line
    // traversal, so always treat issuerIsSender = true.
    bool const issuerIsSender = true;

    // If all paths failed quality ranking, return the unranked concrete paths
    // so the caller's rippleCalc can attempt them with the real amounts.
    if (pathRanks_.empty() && !completePaths_.empty())
    {
        STPathSet result;
        for (auto const& path : completePaths_)
        {
            if (static_cast<int>(result.size()) >= maxPaths)
                break;
            if (!path.empty())
                result.push_back(path);
        }
        return result;
    }

    std::vector<PathRank> extraRanks;
    rankPaths(maxPaths, extraPaths, extraRanks, continueCallback);

    STPathSet bestPaths;
    STAmount remaining = remainingAmount_;

    auto itA = pathRanks_.begin();
    auto itB = extraRanks.begin();

    while (itA != pathRanks_.end() || itB != extraRanks.end())
    {
        if (continueCallback && !continueCallback())
            break;

        bool usePath = false;
        bool useExtra = false;

        if (itA == pathRanks_.end())
        {
            useExtra = true;
        }
        else if (itB == extraRanks.end())
        {
            usePath = true;
        }
        else if (itB->quality < itA->quality)
        {
            useExtra = true;
        }
        else if (itB->quality > itA->quality)
        {
            usePath = true;
        }
        else if (itB->liquidity > itA->liquidity)
        {
            useExtra = true;
        }
        else if (itB->liquidity < itA->liquidity)
        {
            usePath = true;
        }
        else
        {
            useExtra = true;
            usePath = true;
        }

        auto& rank = usePath ? *itA : *itB;
        auto const& p =
            usePath ? completePaths_[rank.index] : extraPaths[rank.index];

        if (useExtra)
            ++itB;
        if (usePath)
            ++itA;

        int iPathsLeft = maxPaths - static_cast<int>(bestPaths.size());
        if (iPathsLeft <= 0)
            break;

        if (p.empty())
            continue;

        // Skip paths that don't match issuer constraints.
        bool startsWithIssuer = false;
        if (!issuerIsSender && usePath)
        {
            if (p.size() == 1 || p.front().getAccountID() != srcIssuer)
                continue;
            startsWithIssuer = true;
        }

        STPath trimmed;
        if (startsWithIssuer)
        {
            for (auto it = p.begin() + 1; it != p.end(); ++it)
                trimmed.push_back(*it);
        }
        STPath const& finalPath = startsWithIssuer ? trimmed : p;

        if (iPathsLeft > 0)
        {
            --iPathsLeft;
            remaining -= rank.liquidity;
            bestPaths.push_back(finalPath);
        }
    }

    return bestPaths;
}

}  // namespace ripple
