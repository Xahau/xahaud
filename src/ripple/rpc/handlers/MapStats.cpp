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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/beast/core/LexicalCast.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/Tuning.h>
#include <ripple/shamap/SHAMap.h>
#include <ripple/shamap/SHAMapInnerNode.h>
#include <ripple/shamap/SHAMapItem.h>
#include <ripple/shamap/SHAMapLeafNode.h>
#include <ripple/shamap/SHAMapNodeID.h>
#include <ripple/shamap/SHAMapTreeNode.h>
#include <cstdio>
#include <memory>
#include <stack>
#include <unordered_map>

namespace ripple {

Json::Value
doMapStats(RPC::JsonContext& context)
{
    auto& params = context.params;
    Json::Value result;

    // Get ledger based on parameters (mimicking ledgerFromRequest logic)
    std::shared_ptr<Ledger const> lgr;

    // Check if params came from command line (wrapped in params array)
    Json::Value actualParams = params;
    if (params.isMember(jss::params) && params[jss::params].isArray())
    {
        // Command line wraps the actual params in a params array
        // We need to parse this like parseLedger does
        if (params[jss::params].size() > 0)
        {
            std::string ledgerParam = params[jss::params][0u].asString();

            // Parse ledger parameter similar to jvParseLedger
            if (ledgerParam == "current" || ledgerParam == "closed" ||
                ledgerParam == "validated")
            {
                actualParams[jss::ledger_index] = ledgerParam;
            }
            else if (ledgerParam.length() == 64)
            {
                actualParams[jss::ledger_hash] = ledgerParam;
            }
            else
            {
                // Parse as numeric ledger index
                actualParams[jss::ledger_index] =
                    beast::lexicalCast<std::uint32_t>(ledgerParam);
            }
        }
    }

    auto indexValue = actualParams[jss::ledger_index];
    auto hashValue = actualParams[jss::ledger_hash];

    // Support legacy "ledger" field
    auto& legacyLedger = actualParams[jss::ledger];
    if (legacyLedger)
    {
        if (legacyLedger.asString().size() > 12)
            hashValue = legacyLedger;
        else
            indexValue = legacyLedger;
    }

    if (hashValue)
    {
        if (!hashValue.isString())
        {
            RPC::inject_error(rpcINVALID_PARAMS, result);
            result[jss::error_message] = "ledgerHashNotString";
            return result;
        }

        uint256 ledgerHash;
        if (!ledgerHash.parseHex(hashValue.asString()))
        {
            RPC::inject_error(rpcINVALID_PARAMS, result);
            result[jss::error_message] = "ledgerHashMalformed";
            return result;
        }
        lgr = context.ledgerMaster.getLedgerByHash(ledgerHash);
    }
    else
    {
        auto const index = indexValue.asString();

        if (index == "validated" || index.empty())
        {
            lgr = context.ledgerMaster.getValidatedLedger();
        }
        else if (index == "current")
        {
            lgr = std::dynamic_pointer_cast<Ledger const>(
                context.ledgerMaster.getCurrentLedger());
        }
        else if (index == "closed")
        {
            lgr = context.ledgerMaster.getClosedLedger();
        }
        else
        {
            // Try to parse as numeric index
            std::uint32_t iVal;
            if (beast::lexicalCastChecked(iVal, index))
            {
                lgr = context.ledgerMaster.getLedgerBySeq(iVal);
            }
            else
            {
                RPC::inject_error(rpcINVALID_PARAMS, result);
                result[jss::error_message] = "ledgerIndexMalformed";
                return result;
            }
        }
    }

    if (!lgr)
    {
        RPC::inject_error(rpcLGR_NOT_FOUND, result);
        result[jss::error_message] = "ledgerNotFound";
        return result;
    }

    // Determine which map to analyze
    bool analyzeStateMap = true;  // Default to state map
    if (params.isMember(jss::type))
    {
        auto const typeStr = params[jss::type].asString();
        if (typeStr == "state")
            analyzeStateMap = true;
        else if (typeStr == "transactions" || typeStr == "tx")
            analyzeStateMap = false;
        else
        {
            RPC::inject_error(rpcINVALID_PARAMS, result);
            result[jss::error_message] =
                "Invalid map type. Use 'state' or 'transactions'";
            return result;
        }
    }

    const SHAMap& map = analyzeStateMap ? lgr->stateMap() : lgr->txMap();

    // Initialize counters
    std::uint64_t leafCount = 0;
    std::uint64_t innerCount = 0;
    std::uint64_t totalLeafDataSize = 0;  // Total size of leaf data in bytes

    // Arrays for histogram data (depth 0 to 63 for inner nodes)
    std::array<std::uint64_t, 64> nodesAtDepth{};
    std::array<std::uint64_t, 64> totalChildrenAtDepth{};

    // We'll use visitNodes and track parent-child relationships to determine
    // depth This is a bit of a hack but works without modifying SHAMap

    // First pass: collect all nodes and their relationships
    struct NodeStats
    {
        bool isInner = false;
        bool isLeaf = false;
        int childCount = 0;
        int depth = -1;  // Will be computed
        SHAMapTreeNode* nodePtr = nullptr;
    };

    std::unordered_map<SHAMapTreeNode*, NodeStats> nodeMap;
    std::unordered_map<SHAMapTreeNode*, std::vector<SHAMapTreeNode*>>
        parentToChildren;
    SHAMapTreeNode* rootNode = nullptr;

    // Collect all nodes and relationships
    try
    {
        map.visitNodes([&](SHAMapTreeNode& node) -> bool {
            NodeStats stats;
            stats.nodePtr = &node;
            stats.isInner = node.isInner();
            stats.isLeaf = node.isLeaf();

            if (rootNode == nullptr)
            {
                rootNode = &node;  // First node visited is the root
                stats.depth = 0;
            }

            if (node.isInner())
            {
                auto& inner = static_cast<SHAMapInnerNode&>(node);
                for (int i = 0; i < SHAMapInnerNode::branchFactor; ++i)
                {
                    if (!inner.isEmptyBranch(i))
                    {
                        stats.childCount++;
                        // Note: We can't get the actual child pointer here
                        // without const_cast but we can count them
                    }
                }
            }
            else if (node.isLeaf())
            {
                // Get the data size from the leaf node
                auto& leaf = static_cast<SHAMapLeafNode&>(node);
                auto const& item = leaf.peekItem();
                if (item)
                {
                    totalLeafDataSize += item->size();
                }
            }

            nodeMap[&node] = stats;
            return true;  // Continue traversal
        });
    }
    catch (const std::exception& e)
    {
        throw;
    }
    catch (...)
    {
        throw;
    }

    // Now compute the actual statistics from what we collected
    for (auto& [nodePtr, stats] : nodeMap)
    {
        if (stats.isLeaf)
        {
            leafCount++;
        }
        else if (stats.isInner)
        {
            innerCount++;
            // For depth calculation, we'd need parent-child links which we
            // can't get without modifying SHAMap or using const_cast tricks For
            // now, we'll have to skip the depth histogram
        }
    }

    // Build the result JSON
    try
    {
        result[jss::ledger_hash] = to_string(lgr->info().hash);
        result[jss::ledger_index] = lgr->info().seq;
        result["map_type"] = analyzeStateMap ? "state" : "transactions";
        result["leaf_count"] = static_cast<Json::UInt>(leafCount);
        result["inner_count"] = static_cast<Json::UInt>(innerCount);
        result["total_nodes"] = static_cast<Json::UInt>(leafCount + innerCount);
        result["total_leaf_data_bytes"] =
            static_cast<Json::UInt>(totalLeafDataSize);

        // Calculate average leaf data size
        if (leafCount > 0)
        {
            double avgLeafSize =
                static_cast<double>(totalLeafDataSize) / leafCount;
            result["avg_leaf_data_bytes"] = avgLeafSize;

            // Also show in MB for readability
            result["total_leaf_data_mb"] =
                static_cast<double>(totalLeafDataSize) / (1024.0 * 1024.0);
        }

        // Add hash statistics
        auto& hashStats = getHashStats();
        result["total_sha512h"] = static_cast<Json::UInt>(
            hashStats.totalSha512HalfCount.load(std::memory_order_relaxed));
        result["index_sha512h"] =
            static_cast<Json::UInt>(hashStats.indexSha512HalfCount());
        result["non_index_sha512h"] =
            static_cast<Json::UInt>(hashStats.nonIndexSha512HalfCount());

        // Add timing statistics (convert to milliseconds for readability)
        auto totalTimeNs =
            hashStats.totalSha512HalfTimeNs.load(std::memory_order_relaxed);
        auto indexTimeNs =
            hashStats.indexSha512HalfTimeNs.load(std::memory_order_relaxed);
        auto nonIndexTimeNs = hashStats.nonIndexSha512HalfTimeNs();

        result["total_sha512h_time_ms"] =
            static_cast<Json::UInt>(totalTimeNs / 1000000);
        result["index_sha512h_time_ms"] =
            static_cast<Json::UInt>(indexTimeNs / 1000000);
        result["non_index_sha512h_time_ms"] =
            static_cast<Json::UInt>(nonIndexTimeNs / 1000000);

        // Add time ratios (as percentages)
        if (totalTimeNs > 0)
        {
            double indexRatio =
                (static_cast<double>(indexTimeNs) / totalTimeNs) * 100.0;
            double nonIndexRatio =
                (static_cast<double>(nonIndexTimeNs) / totalTimeNs) * 100.0;
            result["index_time_ratio_pct"] = indexRatio;
            result["non_index_time_ratio_pct"] = nonIndexRatio;
        }

        // Add first full ledger tracking
        auto firstLedgerSeq = context.ledgerMaster.getFirstFullLedgerSeq();
        if (firstLedgerSeq > 0)
        {
            result["first_full_ledger_seq"] =
                static_cast<Json::UInt>(firstLedgerSeq);
            result["ledgers_since_first_sync"] = static_cast<Json::UInt>(
                context.ledgerMaster.getLedgersSinceFirstSync());

            // Add ledger age in sequence numbers (how many ledgers we've
            // validated since first sync)
            auto validatedSeq = context.ledgerMaster.getValidLedgerIndex();
            if (validatedSeq >= firstLedgerSeq)
            {
                result["first_full_ledger_age_seq"] =
                    static_cast<Json::UInt>(validatedSeq - firstLedgerSeq);
            }

            auto firstLedgerTime =
                context.ledgerMaster.getFirstFullLedgerTime();
            if (firstLedgerTime.time_since_epoch().count() > 0)
            {
                auto now = std::chrono::steady_clock::now();
                auto ageMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - firstLedgerTime)
                        .count();
                result["first_full_ledger_age_ms"] =
                    static_cast<Json::UInt>(ageMs);

                // Add transaction statistics
                auto totalTxns = context.ledgerMaster.getTotalTxnsSinceSync();
                result["total_txns_since_sync"] =
                    static_cast<Json::UInt>(totalTxns);

                // Calculate average txns per ledger
                auto ledgersSinceSync =
                    context.ledgerMaster.getLedgersSinceFirstSync();
                if (ledgersSinceSync > 0)
                {
                    double avgTxnsPerLedger =
                        static_cast<double>(totalTxns) / ledgersSinceSync;
                    result["avg_txns_per_ledger"] = avgTxnsPerLedger;

                    // Calculate txns per second
                    if (ageMs > 0)
                    {
                        double txnsPerSec =
                            (static_cast<double>(totalTxns) * 1000.0) / ageMs;
                        result["txns_per_sec"] = txnsPerSec;
                    }
                }

                // Get recent window stats
                auto [recentTxns, recentLedgers] =
                    context.ledgerMaster.getRecentTxnStats();
                if (recentLedgers > 0)
                {
                    result["recent_window_ledgers"] =
                        static_cast<Json::UInt>(recentLedgers);
                    result["recent_window_txns"] =
                        static_cast<Json::UInt>(recentTxns);
                    double recentAvg =
                        static_cast<double>(recentTxns) / recentLedgers;
                    result["recent_avg_txns_per_ledger"] = recentAvg;
                }
            }
        }

        // Add namespace histogram
        Json::Value namespaceHistogram(Json::objectValue);
        for (std::uint16_t i = 0; i < 256; ++i)
        {
            auto count = hashStats.indexSha512HalfBySpace[i].load(
                std::memory_order_relaxed);
            if (count > 0)
            {
                const char* name = getLedgerNameSpaceName(i);
                if (name)
                {
                    namespaceHistogram[name] = static_cast<Json::UInt>(count);
                }
                else
                {
                    // Unknown namespace, use hex representation
                    char hexName[16];
                    snprintf(hexName, sizeof(hexName), "UNKNOWN_0x%02X", i);
                    namespaceHistogram[hexName] =
                        static_cast<Json::UInt>(count);
                }
            }
        }
        result["index_sha512h_by_namespace"] = namespaceHistogram;
    }
    catch (const std::exception& e)
    {
        throw;
    }

    return result;
}

}  // namespace ripple