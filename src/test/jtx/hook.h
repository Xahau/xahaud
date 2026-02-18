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

#ifndef RIPPLE_TEST_JTX_HOOK_H_INCLUDED
#define RIPPLE_TEST_JTX_HOOK_H_INCLUDED

#include <test/jtx/Account.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/json/json_value.h>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
hook(
    Account const& account,
    std::optional<std::vector<Json::Value>> hooks,
    std::uint32_t flags);

Json::Value
hso(std::vector<uint8_t> const& wasmBytes, void (*f)(Json::Value& jv) = 0);

Json::Value
hso(std::string const& wasmHex, void (*f)(Json::Value& jv) = 0);

Json::Value
hso_delete(void (*f)(Json::Value& jv) = 0);

struct StubHookResult
{
    ripple::uint256 const hookSetTxnID = ripple::uint256();
    ripple::uint256 const hookHash = ripple::uint256();
    ripple::uint256 const hookCanEmit = ripple::uint256();
    ripple::uint256 const hookNamespace = ripple::uint256();

    std::queue<std::shared_ptr<ripple::Transaction>> emittedTxn{};
    std::optional<hook::HookStateMap> stateMap = std::nullopt;
    uint16_t changedStateCount = 0;
    std::map<
        ripple::uint256,  // hook hash
        std::map<
            std::vector<uint8_t>,  // hook param name
            std::vector<uint8_t>   // hook param value
            >>
        hookParamOverrides = {};

    std::optional<std::map<std::vector<uint8_t>, std::vector<uint8_t>>>
        hookParams = std::nullopt;
    std::set<ripple::uint256> hookSkips = {};
    hook_api::ExitType exitType = hook_api::ExitType::ROLLBACK;
    std::string exitReason{""};
    int64_t exitCode{-1};
    uint64_t instructionCount{0};
    bool hasCallback = false;
    bool isCallback = false;
    bool isStrong = false;
    uint32_t wasmParam = 0;
    uint32_t overrideCount = 0;
    uint8_t hookChainPosition = 0;
    bool foreignStateSetDisabled = false;
    bool executeAgainAsWeak = false;
    std::shared_ptr<STObject const> provisionalMeta = nullptr;
};

struct StubHookContext
{
    std::map<uint32_t, hook::SlotEntry> slot{};
    std::queue<uint32_t> slot_free{};
    uint32_t slot_counter{0};
    uint16_t emit_nonce_counter{0};
    uint16_t ledger_nonce_counter{0};
    int64_t expected_etxn_count{-1};
    std::map<ripple::uint256, bool> nonce_used{};
    uint32_t generation = 0;
    uint64_t burden = 0;
    std::map<uint32_t, uint32_t> guard_map{};
    StubHookResult result = {};
    std::optional<ripple::STObject> emitFailure = std::nullopt;
    const hook::HookExecutor* module = 0;
};

// Overload that takes external stateMap to avoid dangling reference
hook::HookContext
makeStubHookContext(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& hookAccount,
    ripple::AccountID const& otxnAccount,
    StubHookContext const& stubHookContext,
    hook::HookStateMap& stateMap);

hook::HookContext
makeStubHookContext(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& hookAccount,
    ripple::AccountID const& otxnAccount,
    StubHookContext const& stubHookContext);

}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif
