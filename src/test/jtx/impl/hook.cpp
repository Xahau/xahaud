//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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

#include <ripple/app/hook/Enum.h>
#include <ripple/app/hook/applyHook.h>
#include <ripple/basics/contract.h>
#include <ripple/protocol/Keylet.h>
#include <ripple/protocol/jss.h>
#include <stdexcept>
#include <test/jtx/hook.h>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
hook(
    Account const& account,
    std::optional<std::vector<Json::Value>> hooks,
    std::uint32_t flags)
{
    Json::Value jv;
    jv[jss::Account] = account.human();
    jv[jss::TransactionType] = jss::SetHook;
    jv[jss::Flags] = flags;

    if (hooks)
    {
        jv[jss::Hooks] = Json::Value{Json::arrayValue};
        for (uint64_t i = 0; i < hooks->size(); ++i)
            jv[jss::Hooks][i][jss::Hook] = (*hooks)[i];
    }
    return jv;
}

Json::Value
hso_delete(void (*f)(Json::Value& jv))
{
    Json::Value jv;
    jv[jss::CreateCode] = "";
    jv[jss::Flags] = hsfOVERRIDE;

    if (f)
        f(jv);

    return jv;
}

Json::Value
hso(std::vector<uint8_t> const& wasmBytes, void (*f)(Json::Value& jv))
{
    if (wasmBytes.size() == 0)
        throw std::runtime_error("empty hook wasm passed to hso()");

    Json::Value jv;
    jv[jss::CreateCode] = strHex(wasmBytes);
    {
        jv[jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        jv[jss::HookNamespace] = to_string(uint256{beast::zero});
        jv[jss::HookApiVersion] = Json::Value{0};
    }

    if (f)
        f(jv);

    return jv;
}

Json::Value
hso(std::string const& wasmHex, void (*f)(Json::Value& jv))
{
    if (wasmHex.size() == 0)
        throw std::runtime_error("empty hook wasm passed to hso()");

    Json::Value jv;
    jv[jss::CreateCode] = wasmHex;
    {
        jv[jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        jv[jss::HookNamespace] = to_string(uint256{beast::zero});
        jv[jss::HookApiVersion] = Json::Value{0};
    }

    if (f)
        f(jv);

    return jv;
}

// Helper function to create HookContext with external stateMap
hook::HookContext
makeStubHookContext(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& hookAccount,
    ripple::AccountID const& otxnAccount,
    StubHookContext const& stubHookContext,
    hook::HookStateMap& stateMap)
{
    auto& result = stubHookContext.result;
    auto hookParams = result.hookParams.value_or(
        std::map<std::vector<uint8_t>, std::vector<uint8_t>>{});
    return hook::HookContext{
        .applyCtx = applyCtx,
        .slot = stubHookContext.slot,
        .slot_free = stubHookContext.slot_free,
        .slot_counter = stubHookContext.slot_counter,
        .emit_nonce_counter = stubHookContext.emit_nonce_counter,
        .ledger_nonce_counter = stubHookContext.ledger_nonce_counter,
        .expected_etxn_count = stubHookContext.expected_etxn_count,
        .nonce_used = stubHookContext.nonce_used,
        .generation = stubHookContext.generation,
        .burden = stubHookContext.burden,
        .guard_map = stubHookContext.guard_map,
        .result =
            {
                .hookSetTxnID = result.hookSetTxnID,
                .hookHash = result.hookHash,
                .hookCanEmit = result.hookCanEmit,
                .accountKeylet = keylet::account(hookAccount),
                .hookKeylet = keylet::hook(hookAccount),
                .account = hookAccount,
                .otxnAccount = otxnAccount,
                .hookNamespace = result.hookNamespace,
                .emittedTxn = result.emittedTxn,
                .stateMap = stateMap,
                .changedStateCount = result.changedStateCount,
                .hookParamOverrides = result.hookParamOverrides,
                .hookParams = hookParams,
                .hookSkips = result.hookSkips,
                .exitType = result.exitType,
                .exitReason = result.exitReason,
                .exitCode = result.exitCode,
                .instructionCount = result.instructionCount,
                .hasCallback = result.hasCallback,
                .isCallback = result.isCallback,
                .isStrong = result.isStrong,
                .wasmParam = result.wasmParam,
                .overrideCount = result.overrideCount,
                .hookChainPosition = result.hookChainPosition,
                .foreignStateSetDisabled = result.foreignStateSetDisabled,
                .executeAgainAsWeak = result.executeAgainAsWeak,
                .provisionalMeta = result.provisionalMeta,
            },
        .emitFailure = stubHookContext.emitFailure,
        .module = nullptr};
}

// Original function - WARNING: stateMap reference may become dangling
// Only use when stateMap access is not needed after HookContext creation
hook::HookContext
makeStubHookContext(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& hookAccount,
    ripple::AccountID const& otxnAccount,
    StubHookContext const& stubHookContext)
{
    // Use thread_local to keep stateMap alive
    // Note: This is a workaround; each call resets the stateMap
    thread_local hook::HookStateMap stateMap;
    stateMap = stubHookContext.result.stateMap.value_or(hook::HookStateMap{});
    return makeStubHookContext(
        applyCtx, hookAccount, otxnAccount, stubHookContext, stateMap);
}

}  // namespace jtx
}  // namespace test
}  // namespace ripple
