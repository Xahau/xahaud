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
#include <xrpl/protocol/AccountNumber.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>

#include <charconv>
#include <optional>
#include <string>

namespace ripple {

namespace {

// uint64 the way STUInt64::getJson renders it: lowercase hex, no prefix,
// no padding. Used for account_index and account_count so they look the
// same here as on the AccountRoot / FeeSettings objects.
std::string
toHex(std::uint64_t v)
{
    char buf[16];
    auto const [p, ec] = std::to_chars(buf, buf + sizeof(buf), v, 16);
    return std::string(buf, p);
}

// account_index: JSON unsigned integer (decimal), or a string in the hex
// form the ledger renders (1..16 hex digits, either case, no prefix).
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
    if (s.empty() || s.size() > 16 ||
        s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return std::nullopt;

    std::uint64_t out = 0;
    auto const [p, ec] =
        std::from_chars(s.data(), s.data() + s.size(), out, 16);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return out;
}

// account_number: JSON unsigned integer, or a digits-only decimal string.
std::optional<std::uint64_t>
parseAccountNumber(Json::Value const& v)
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
    return accountNumber::parse(v.asString());
}

}  // namespace

// Exactly one of:
//   account_index:  <hex string as rendered on the AccountRoot, or integer>
//   account_number: <decimal digits string or integer; index*1000 + check>
//
// Resolves an AccountRoot's AccountIndex to its address, using the index
// maintained by AccountIndexer (enabled with [account_index]). See
// AccountNumber.h for how an account number differs from an account index.
//
// Errors:
//   invalidParams     - both or neither selector given, or malformed
//   notEnabled        - [account_index] is not configured on this server
//   notSynced         - no validated ledger to judge existence against
//   actNotFound       - no account has this index in the validated ledger
//   actIndexBuilding  - the account exists but the index has not reached it
//   actNumberMismatch - account_number's check digits do not match the
//                       account at that index (almost certainly a typo)
Json::Value
doAccountByIndex(RPC::JsonContext& context)
{
    auto const& params = context.params;

    bool const haveIndex = params.isMember(jss::account_index);
    bool const haveNumber = params.isMember(jss::account_number);
    if (haveIndex == haveNumber)
        return RPC::make_error(
            rpcINVALID_PARAMS,
            "Specify exactly one of account_index or account_number.");

    std::uint64_t index = 0;
    std::optional<std::uint64_t> wantCheck;

    if (haveIndex)
    {
        auto const parsed = parseAccountIndex(params[jss::account_index]);
        if (!parsed)
            return RPC::invalid_field_error(jss::account_index);
        index = *parsed;
    }
    else
    {
        auto const parsed = parseAccountNumber(params[jss::account_number]);
        if (!parsed)
            return RPC::invalid_field_error(jss::account_number);
        auto const s = accountNumber::split(*parsed);
        index = s.index;
        wantCheck = s.check;
    }

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

    auto const account = indexer.lookup(index);

    if (!account)
    {
        if (index >= accountCount)
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

    // A number's check digits must match the account actually found. Do
    // this after the existence checks so a typo that lands on a not-yet
    // created index reports actNotFound rather than a mismatch.
    if (wantCheck && *wantCheck != accountNumber::check(*account))
        return rpcError(rpcACT_NUMBER_MISMATCH);

    Json::Value result(Json::objectValue);
    result[jss::account] = toBase58(*account);
    result[jss::account_index] = toHex(index);
    if (auto const number = accountNumber::make(index, *account))
        result[jss::account_number] = accountNumber::toString(*number);
    result[jss::account_count] = toHex(accountCount);
    result[jss::ledger_index] = ledger->info().seq;
    result[jss::validated] = true;
    return result;
}

}  // namespace ripple
