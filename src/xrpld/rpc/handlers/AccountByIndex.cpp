//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
    Copyright (c) 2026 XRPL Labs

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

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/AccountIndexer.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>

#include <charconv>
#include <optional>
#include <string>

namespace ripple {

namespace {

// Accepts a JSON unsigned integer or a string of decimal digits.
std::optional<std::uint64_t>
parseAccountIndex(Json::Value const& v)
{
    if (v.isUInt())
        return v.asUInt();
    if (v.isInt())
    {
        auto const i = v.asInt();
        if (i < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(i);
    }
    if (!v.isString())
        return std::nullopt;

    auto const s = v.asString();
    if (s.empty() || s.size() > 20 || (s.size() > 1 && s.front() == '0') ||
        s.find_first_not_of("0123456789") != std::string::npos)
        return std::nullopt;

    std::uint64_t out = 0;
    auto const [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return out;
}

}  // namespace

// {
//   account_index: <unsigned integer or decimal string>
// }
//
// Resolves an AccountRoot's AccountIndex to its address, using the index
// maintained by AccountIndexer (enabled with [account_index]).
//
// Errors:
//   notEnabled        - [account_index] is not configured on this server
//   notSynced         - no validated ledger to judge existence against
//   actNotFound       - no account has this index in the validated ledger
//   actIndexBuilding  - the account exists but the index has not reached it
Json::Value
doAccountByIndex(RPC::JsonContext& context)
{
    auto const& params = context.params;

    if (!params.isMember(jss::account_index))
        return RPC::missing_field_error(jss::account_index);

    auto const index = parseAccountIndex(params[jss::account_index]);
    if (!index)
        return RPC::invalid_field_error(jss::account_index);

    auto& indexer = context.app.getAccountIndexer();
    if (!indexer.enabled())
        return rpcError(rpcNOT_ENABLED);

    // Existence is judged against the validated ledger: an account with
    // AccountIndex i exists iff i < AccountCount on FeeSettings. Deleted
    // accounts do not exist on Xahau, so the mapping is immutable and a
    // hit from the index is authoritative regardless of build state.
    auto const ledger = context.ledgerMaster.getValidatedLedger();
    if (!ledger)
        return rpcError(rpcNOT_SYNCED);

    std::uint64_t accountCount = 0;
    if (auto const fees = ledger->read(keylet::fees());
        fees && fees->isFieldPresent(sfAccountCount))
        accountCount = fees->getFieldU64(sfAccountCount);

    auto const account = indexer.lookup(*index);

    if (!account)
    {
        if (*index >= accountCount)
            return rpcError(rpcACT_NOT_FOUND);

        // The account exists but is not in the index. Either the index is
        // still building / lagging the validated ledger, or something is
        // inconsistent (which the indexer detects and repairs by rescanning).
        auto const status = indexer.status();
        if (!status.built || status.indexedSeq < ledger->info().seq)
            return rpcError(rpcACT_INDEX_BUILDING);
        return rpcError(
            rpcINTERNAL,
            "Account index is inconsistent with the ledger; rebuilding.");
    }

    Json::Value result(Json::objectValue);
    result[jss::account] = toBase58(*account);
    result[jss::account_index] = std::to_string(*index);
    result[jss::account_count] = std::to_string(accountCount);
    result[jss::ledger_index] = ledger->info().seq;
    result[jss::validated] = true;
    return result;
}

}  // namespace ripple
