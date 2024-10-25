//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#include <ripple/app/tx/impl/SetHook.h>

#include <ripple/app/hook/Enum.h>
#include <ripple/app/hook/Guard.h>
#include <ripple/app/hook/applyHook.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STTx.h>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <ostream>
#include <stack>
#include <stdio.h>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include <wasmedge/wasmedge.h>

#define DEBUG_GUARD_CHECK 1
#define HS_ACC() \
    ctx.tx.getAccountID(sfAccount) << "-" << ctx.tx.getTransactionID()

namespace ripple {

using GrantKey = std::pair<uint256, std::optional<AccountID>>;

bool
validateHookGrants(SetHookCtx& ctx, STArray const& hookGrants)
{
    if (hookGrants.size() > 8)
    {
        JLOG(ctx.j.trace())
            << "HookSet(" << hook::log::GRANTS_EXCESS << ")[" << HS_ACC()
            << "]: Malformed transaction: SetHook sfHookGrants contains more "
               "than 8 entries.";
        return false;
    }

    AccountID const& hookAcc = ctx.tx.getAccountID(sfAccount);

    std::set<GrantKey> already;
    for (auto const& hookGrantObj : hookGrants)
    {
        if (hookGrantObj.getFName() != sfHookGrant)
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::GRANTS_ILLEGAL << ")[" << HS_ACC()
                << "]: Malformed transaction: SetHook sfHookGrants did not "
                   "contain sfHookGrant object.";
            return false;
        }

        if (hookGrantObj.isFieldPresent(sfAuthorize) &&
            hookGrantObj.getAccountID(sfAuthorize) == hookAcc)
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::GRANTS_ILLEGAL << ")[" << HS_ACC()
                << "]: Malformed transaction: SetHook sfHookGrants cannot self "
                   "grant an account.";
            return false;
        }

        auto const hash = hookGrantObj.getFieldH256(sfHookHash);

        GrantKey entry{hash, hookGrantObj.at(~sfAuthorize)};
        if (already.find(entry) != already.end())
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::GRANTS_ILLEGAL << ")[" << HS_ACC()
                << "]: Malformed transaction, SetHook sfHookGrants contains "
                   "the same sfHookHash/sfAuthorize "
                << "pair multiple times.";
            return false;
        }
        already.emplace(entry);
    }

    return true;
}

bool
validateHookParams(SetHookCtx& ctx, STArray const& hookParams)
{
    const int paramKeyMax = hook::maxHookParameterKeySize();
    const int paramValueMax = hook::maxHookParameterValueSize();

    int paramCount = (int)(hookParams.size());
    if (paramCount > 16)
    {
        JLOG(ctx.j.trace())
            << "HookSet(" << hook::log::HOOK_PARAMS_COUNT << ")[" << HS_ACC()
            << "]: Malformed transaction: Txn would result in too many "
               "parameters on hook";
        return false;
    }

    std::set<ripple::Blob> alreadySet;

    for (auto const& hookParamObj : hookParams)
    {
        if (hookParamObj.getFName() != sfHookParameter)
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::PARAMETERS_ILLEGAL << ")["
                << HS_ACC() << "]: Malformed transaction: "
                << "SetHook sfHookParameters contains obj other than "
                   "sfHookParameter.";
            return false;
        }

        // RH NOTE: templating system already does most of these checks but if a
        // change is made in future then they are double checked here
        bool nameFound = false;
        for (auto const& paramElement : hookParamObj)
        {
            auto const& name = paramElement.getFName();

            if (name != sfHookParameterName && name != sfHookParameterValue)
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::PARAMETERS_FIELD << ")["
                    << HS_ACC() << "]: Malformed transaction: "
                    << "SetHook sfHookParameter contains object other than "
                       "sfHookParameterName/Value.";
                return false;
            }

            if (name == sfHookParameterName)
                nameFound = true;
        }

        if (!nameFound)
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::PARAMETERS_NAME << ")[" << HS_ACC()
                << "]: Malformed transaction: "
                << "SetHook sfHookParameter must contain at least "
                   "sfHookParameterName";
            return false;
        }

        ripple::Blob const& paramName =
            hookParamObj.getFieldVL(sfHookParameterName);

        if (paramName.size() > paramKeyMax ||
            (hookParamObj.isFieldPresent(sfHookParameterValue) &&
             hookParamObj.getFieldVL(sfHookParameterValue).size() >
                 paramValueMax))
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::HOOK_PARAM_SIZE << ")[" << HS_ACC()
                << "]: Malformed transaction: Txn would result in a too large "
                   "parameter name/value on hook";
            return false;
        }

        if (alreadySet.find(paramName) != alreadySet.end())
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::PARAMETERS_NAME_REPEATED << ")["
                << HS_ACC() << "]: Malformed transaction: "
                << "SetHook sfHookParameters must contain each parameter name "
                   "at most once";
            return false;
        }

        alreadySet.emplace(paramName);
    }

    return true;
}

// infer which operation the user is attempting to execute from the present and
// absent fields
HookSetOperation
SetHook::inferOperation(STObject const& hookSetObj)
{
    uint64_t wasmByteCount = hookSetObj.isFieldPresent(sfCreateCode)
        ? hookSetObj.getFieldVL(sfCreateCode).size()
        : 0;

    bool hasHash = hookSetObj.isFieldPresent(sfHookHash);
    bool hasCode = hookSetObj.isFieldPresent(sfCreateCode);

    if (hasHash && hasCode)  // Both HookHash and CreateCode: invalid
        return hsoINVALID;
    else if (hasHash)  // Hookhash only: install
        return hsoINSTALL;
    else if (hasCode)  // CreateCode only: either delete or create
        return wasmByteCount > 0 ? hsoCREATE : hsoDELETE;
    else if (
        !hasHash && !hasCode && !hookSetObj.isFieldPresent(sfHookGrants) &&
        !hookSetObj.isFieldPresent(sfHookNamespace) &&
        !hookSetObj.isFieldPresent(sfHookParameters) &&
        !hookSetObj.isFieldPresent(sfHookOn) &&
        !hookSetObj.isFieldPresent(sfHookApiVersion) &&
        !hookSetObj.isFieldPresent(sfFlags))
        return hsoNOOP;

    uint32_t flags = hookSetObj.isFieldPresent(sfFlags)
        ? hookSetObj.getFieldU32(sfFlags)
        : 0;

    return hookSetObj.isFieldPresent(sfHookNamespace) && (flags & hsfNSDELETE)
        ? hsoNSDELETE
        : hsoUPDATE;
}

// This is a context-free validation, it does not take into account the current
// state of the ledger returns  < valid, instruction count > may throw
// overflow_error
HookSetValidation
SetHook::validateHookSetEntry(SetHookCtx& ctx, STObject const& hookSetObj)
{
    uint32_t flags = hookSetObj.isFieldPresent(sfFlags)
        ? hookSetObj.getFieldU32(sfFlags)
        : 0;

    switch (inferOperation(hookSetObj))
    {
        case hsoNOOP: {
            return true;
        }

        case hsoNSDELETE: {
            // namespace delete operation
            if (hookSetObj.isFieldPresent(sfHookGrants) ||
                hookSetObj.isFieldPresent(sfHookParameters) ||
                hookSetObj.isFieldPresent(sfHookOn) ||
                hookSetObj.isFieldPresent(sfHookApiVersion) ||
                !hookSetObj.isFieldPresent(sfFlags) ||
                !hookSetObj.isFieldPresent(sfHookNamespace))
            {
                JLOG(ctx.j.trace()) << "HookSet(" << hook::log::NSDELETE_FIELD
                                    << ")[" << HS_ACC()
                                    << "]: Malformed transaction: SetHook "
                                       "nsdelete operation should contain only "
                                    << "sfHookNamespace & sfFlags";
                return false;
            }

            if (flags != hsfNSDELETE)
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::NSDELETE_FLAGS << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook nsdelete operation "
                       "should only specify hsfNSDELETE";
                return false;
            }

            return true;
        }

        case hsoDELETE: {
            if (hookSetObj.isFieldPresent(sfHookGrants) ||
                hookSetObj.isFieldPresent(sfHookParameters) ||
                hookSetObj.isFieldPresent(sfHookOn) ||
                hookSetObj.isFieldPresent(sfHookApiVersion) ||
                hookSetObj.isFieldPresent(sfHookNamespace) ||
                !hookSetObj.isFieldPresent(sfFlags))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::DELETE_FIELD << ")[" << HS_ACC()
                    << "]: Malformed transaction: SetHook delete operation "
                       "should contain only sfCreateCode & sfFlags";
                return false;
            }

            if (!(flags & hsfOVERRIDE))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::OVERRIDE_MISSING << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook delete operation was "
                       "missing the hsfOVERRIDE flag";
                return false;
            }

            if (flags & ~(hsfOVERRIDE | hsfNSDELETE | hsfCOLLECT))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::FLAGS_INVALID << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook delete operation "
                       "specified invalid flags";
                return false;
            }

            return true;
        }

        case hsoINSTALL: {
            // validate hook params structure, if any
            if (hookSetObj.isFieldPresent(sfHookParameters) &&
                !validateHookParams(
                    ctx, hookSetObj.getFieldArray(sfHookParameters)))
                return false;

            // validate hook grants structure, if any
            if (hookSetObj.isFieldPresent(sfHookGrants) &&
                !validateHookGrants(
                    ctx, hookSetObj.getFieldArray(sfHookGrants)))
                return false;

            // api version not allowed in update
            if (hookSetObj.isFieldPresent(sfHookApiVersion))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::API_ILLEGAL << ")[" << HS_ACC()
                    << "]: Malformed transaction: SetHook install operation "
                       "sfHookApiVersion must not be included.";
                return false;
            }

            // namespace may be valid, if the user so chooses
            // hookon may be present if the user so chooses
            // flags may be present if the user so chooses

            return true;
        }

        case hsoUPDATE: {
            // must not specify override flag
            if ((flags & hsfOVERRIDE) ||
                ((flags & hsfNSDELETE) &&
                 !hookSetObj.isFieldPresent(sfHookNamespace)))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::FLAGS_INVALID << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook update operation "
                       "only hsfNSDELETE may be specified and "
                    << "only if a new HookNamespace is also specified.";
                return false;
            }

            // validate hook params structure
            if (hookSetObj.isFieldPresent(sfHookParameters) &&
                !validateHookParams(
                    ctx, hookSetObj.getFieldArray(sfHookParameters)))
                return false;

            // validate hook grants structure
            if (hookSetObj.isFieldPresent(sfHookGrants) &&
                !validateHookGrants(
                    ctx, hookSetObj.getFieldArray(sfHookGrants)))
                return false;

            // api version not allowed in update
            if (hookSetObj.isFieldPresent(sfHookApiVersion))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::API_ILLEGAL << ")[" << HS_ACC()
                    << "]: Malformed transaction: SetHook update operation "
                       "sfHookApiVersion must not be included.";
                return false;
            }

            // namespace may be valid, if the user so chooses
            // hookon may be present if the user so chooses
            // flags may be present if the user so chooses

            return true;
        }

        case hsoCREATE: {
            // validate hook params structure
            if (hookSetObj.isFieldPresent(sfHookParameters) &&
                !validateHookParams(
                    ctx, hookSetObj.getFieldArray(sfHookParameters)))
                return false;

            // validate hook grants structure
            if (hookSetObj.isFieldPresent(sfHookGrants) &&
                !validateHookGrants(
                    ctx, hookSetObj.getFieldArray(sfHookGrants)))
                return false;

            // ensure hooknamespace is present
            if (!hookSetObj.isFieldPresent(sfHookNamespace))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::NAMESPACE_MISSING << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook sfHookDefinition "
                       "must contain sfHookNamespace.";
                return false;
            }

            // ensure api version is provided
            if (!hookSetObj.isFieldPresent(sfHookApiVersion))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::API_MISSING << ")[" << HS_ACC()
                    << "]: Malformed transaction: SetHook sfHookApiVersion "
                       "must be included.";
                return false;
            }

            auto version = hookSetObj.getFieldU16(sfHookApiVersion);
            if (!ctx.rules.enabled(featureJSHooks) && version == 1)
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::API_INVALID << ")[" << HS_ACC()
                    << "]: Malformed transaction: SetHook "
                       "sfHook->sfHookApiVersion invalid. (JSHooks not enabled).";
                return false;
            }
            if (version > 1)
            {
                // we currently only accept api version 0 and 1
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::API_INVALID << ")[" << HS_ACC()
                    << "]: Malformed transaction: SetHook "
                       "sfHook->sfHookApiVersion invalid. (Try 0 or 1).";
                return false;
            }

            // validate sfHookOn
            if (!hookSetObj.isFieldPresent(sfHookOn))
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::HOOKON_MISSING << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook must include "
                       "sfHookOn when creating a new hook.";
                return false;
            }

            // finally validate byte code according to api version
            if (!hookSetObj.isFieldPresent(sfCreateCode))
                return {};

            Blob hook = hookSetObj.getFieldVL(sfCreateCode);


            if (version == 1)
            {
                // RHTODO: guard or other check for js, depending on design choices
               
                if (hookSetObj.isFieldPresent(sfFee) &&
                    isXRP(hookSetObj.getFieldAmount(sfFee)))
                {
                    STAmount amt = hookSetObj.getFieldAmount(sfFee);
                    uint64_t fee = amt.xrp().drops();
                    if (amt < beast::zero || fee < 1 || fee > 1000000)
                    {
                        JLOG(ctx.j.trace())
                            << "HookSet(" << hook::log::JS_FEE_TOO_HIGH << ")["
                            << HS_ACC()
                            << "]: Malformed transaction: When creating a JS Hook "
                            << "you must include a Fee <= 1000000.";
                        return false;

                    }
                }
                else
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::JS_FEE_MISSING << ")["
                        << HS_ACC()
                        << "]: Malformed transaction: When creating a JS Hook "
                        << "you must include a Fee field indicating the instruction limit.";
                    return false;
                }

                std::optional<std::string> result =
                    hook::HookExecutorJS::validate(
                        hook.data(), (size_t)hook.size());

                if (result)
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::JS_TEST_FAILURE << ")["
                        << HS_ACC()
                        << "Tried to set a hook with invalid code. VM error: "
                        << *result;
                    return false;
                }

                // RHTODO: fix
                return std::pair<uint64_t,uint64_t>{1,1};
            }
            
            if (version == 0)
            {
                // RH NOTE: validateGuards has a generic non-rippled specific
                // interface so it can be used in other projects (i.e. tooling).
                // As such the calling here is a bit convoluted.

                std::optional<std::reference_wrapper<std::basic_ostream<char>>>
                    logger;
                std::ostringstream loggerStream;
                std::string hsacc{""};
                if (ctx.j.trace())
                {
                    logger = loggerStream;
                    std::stringstream ss;
                    ss << HS_ACC();
                    hsacc = ss.str();
                }

                auto result = validateGuards(
                    hook,  // wasm to verify
                    logger,
                    hsacc,
                    ctx.rules.enabled(featureHooksUpdate1) ? 1 : 0);

                if (ctx.j.trace())
                {
                    // clunky but to get the stream to accept the output
                    // correctly we will split on new line and feed each line
                    // one by one into the trace stream beast::Journal should be
                    // updated to inherit from basic_ostream<char> then this
                    // wouldn't be necessary.

                    // is this a needless copy or does the compiler do copy
                    // elision here?
                    std::string s = loggerStream.str();

                    char* data = s.data();
                    size_t len = s.size();

                    char* last = data;
                    size_t i = 0;
                    for (; i < len; ++i)
                    {
                        if (data[i] == '\n')
                        {
                            data[i] = '\0';
                            ctx.j.trace() << last;
                            last = data + i;
                        }
                    }

                    if (last < data + i)
                        ctx.j.trace() << last;
                }

                if (!result)
                    return false;

                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::WASM_SMOKE_TEST << ")["
                    << HS_ACC()
                    << "]: Trying to wasm instantiate proposed hook "
                    << "size = " << hook.size();

                std::optional<std::string> result2 =
                    hook::HookExecutorWasm::validate(
                        hook.data(), (size_t)hook.size());

                if (result2)
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::WASM_TEST_FAILURE << ")["
                        << HS_ACC()
                        << "Tried to set a hook with invalid code. VM error: "
                        << *result2;
                    return false;
                }

                return *result;
            }

            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::HASH_OR_CODE << ")[" << HS_ACC()
                << "]: Malformed transaction: SetHook specified invalid HookApiVersion.";
            return false;
        }

        case hsoINVALID:
        default: {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::HASH_OR_CODE << ")[" << HS_ACC()
                << "]: Malformed transaction: SetHook must provide only one of "
                   "sfCreateCode or sfHookHash.";
            return false;
        }
    }
}

// Note that if fee calculation causes an overflow then INITIAL_XRP is returned
// as a way of ensuring that the txn cannot possibly meet the fee requirement.
XRPAmount
SetHook::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount hookFee{0};

    auto const& hookSets = tx.getFieldArray(sfHooks);

    for (auto const& hookSetObj : hookSets)
    {
        XRPAmount createFee{0};
        if (hookSetObj.isFieldPresent(sfCreateCode))
            createFee = XRPAmount{hook::computeCreationFee(
                hookSetObj.getFieldVL(sfCreateCode).size())};

        XRPAmount paramFee{0};
        // parameters are billed at the same rate as code bytes
        if (hookSetObj.isFieldPresent(sfHookParameters))
        {
            int64_t paramBytes = 0;
            auto const& params = hookSetObj.getFieldArray(sfHookParameters);
            for (auto const& param : params)
            {
                int64_t entryBytes = 0;
                if (param.isFieldPresent(sfHookParameterName))
                {
                    entryBytes += param.getFieldVL(sfHookParameterName).size();
                }
                if (param.isFieldPresent(sfHookParameterValue))
                {
                    entryBytes += param.getFieldVL(sfHookParameterValue).size();
                }

                // overflow
                if (paramBytes + entryBytes < paramBytes)
                    return INITIAL_XRP;

                paramBytes += entryBytes;
            }

            // one drop per byte
            paramFee = XRPAmount{paramBytes};
        }

        if (hookFee + paramFee < hookFee)
            return INITIAL_XRP;

        hookFee += paramFee;

        if (hookFee + createFee < hookFee)
            return INITIAL_XRP;

        hookFee += createFee;
    }

    auto fee = Transactor::calculateBaseFee(view, tx);
    if (fee + hookFee < fee)
        return INITIAL_XRP;

    return fee + hookFee;
}

TER
SetHook::preclaim(ripple::PreclaimContext const& ctx)
{
    auto const& hookSets = ctx.tx.getFieldArray(sfHooks);

    for (auto const& hookSetObj : hookSets)
    {
        if (!hookSetObj.isFieldPresent(sfHookHash))
            continue;

        auto const& hash = hookSetObj.getFieldH256(sfHookHash);
        {
            if (!ctx.view.exists(keylet::hookDefinition(hash)))
            {
                JLOG(ctx.j.trace()) << "HookSet(" << hook::log::HOOK_DEF_MISSING
                                    << ")[" << HS_ACC()
                                    << "]: Malformed transaction: No hook "
                                       "exists with the specified hash.";
                return terNO_HOOK;
            }
        }
    }

    return tesSUCCESS;
}

NotTEC
SetHook::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureHooks))
    {
        JLOG(ctx.j.warn()) << "HookSet(" << hook::log::AMENDMENT_DISABLED
                           << ")[" << HS_ACC()
                           << "]: Hooks Amendment not enabled!";
        return temDISABLED;
    }

    auto const ret = preflight1(ctx);
    if (!isTesSuccess(ret))
        return ret;

    if (!ctx.tx.isFieldPresent(sfHooks))
    {
        JLOG(ctx.j.trace())
            << "HookSet(" << hook::log::HOOKS_ARRAY_MISSING << ")[" << HS_ACC()
            << "]: Malformed transaction: SetHook lacked sfHooks array.";
        return temMALFORMED;
    }

    auto const& hookSets = ctx.tx.getFieldArray(sfHooks);

    if (hookSets.size() < 1)
    {
        JLOG(ctx.j.trace())
            << "HookSet(" << hook::log::HOOKS_ARRAY_EMPTY << ")[" << HS_ACC()
            << "]: Malformed transaction: SetHook sfHooks empty.";
        return temMALFORMED;
    }

    if (hookSets.size() > hook::maxHookChainLength())
    {
        JLOG(ctx.j.trace())
            << "HookSet(" << hook::log::HOOKS_ARRAY_TOO_BIG << ")[" << HS_ACC()
            << "]: Malformed transaction: SetHook sfHooks contains more than "
            << hook::maxHookChainLength() << " entries.";
        return temMALFORMED;
    }

    SetHookCtx shCtx{
        .j = ctx.j, .tx = ctx.tx, .app = ctx.app, .rules = ctx.rules};

    bool allBlank = true;

    for (auto const& hookSetObj : hookSets)
    {
        if (hookSetObj.getFName() != sfHook)
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::HOOKS_ARRAY_BAD << ")[" << HS_ACC()
                << "]: Malformed transaction: SetHook sfHooks contains obj "
                   "other than sfHook.";
            return temMALFORMED;
        }

        if (hookSetObj.isFieldPresent(sfCreateCode) &&
            hookSetObj.getFieldVL(sfCreateCode).size() >
                hook::maxHookWasmSize())
        {
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::WASM_TOO_BIG << ")[" << HS_ACC()
                << "]: Malformed transaction: SetHook operation would create "
                   "blob larger than max";
            return temMALFORMED;
        }

        if (hookSetObj.getCount() == 0)  // skip blanks
            continue;

        allBlank = false;

        for (auto const& hookSetElement : hookSetObj)
        {
            auto const& name = hookSetElement.getFName();

            if (name != sfCreateCode && name != sfHookHash &&
                name != sfHookNamespace && name != sfHookParameters &&
                name != sfHookOn && name != sfHookGrants &&
                name != sfHookApiVersion && name != sfFlags &&
                name != sfFee)
            {
                JLOG(ctx.j.trace())
                    << "HookSet(" << hook::log::HOOK_INVALID_FIELD << ")["
                    << HS_ACC()
                    << "]: Malformed transaction: SetHook sfHook contains "
                       "invalid field.";
                return temMALFORMED;
            }
        }

        try
        {
            // may throw if leb128 overflow is detected
            auto valid = validateHookSetEntry(shCtx, hookSetObj);

            if (std::holds_alternative<bool>(valid) && !std::get<bool>(valid))
                return temMALFORMED;
        }
        catch (std::exception& e)
        {
            std::cout << "Exception: " << e.what() << "\n";
            JLOG(ctx.j.trace())
                << "HookSet(" << hook::log::WASM_VALIDATION << ")[" << HS_ACC()
                << "]: Exception: " << e.what();
            return temMALFORMED;
        }
    }

    if (allBlank)
    {
        JLOG(ctx.j.trace())
            << "HookSet(" << hook::log::HOOKS_ARRAY_BLANK << ")[" << HS_ACC()
            << "]: Malformed transaction: SetHook sfHooks must contain at "
               "least one non-blank sfHook.";
        return temMALFORMED;
    }

    return preflight2(ctx);
}

TER
SetHook::doApply()
{
    preCompute();
    return setHook();
}

void
SetHook::preCompute()
{
    return Transactor::preCompute();
}

TER
SetHook::destroyNamespace(
    SetHookCtx& ctx,
    ApplyView& view,
    const AccountID& account,
    uint256 ns)
{
    JLOG(ctx.j.trace()) << "HookSet(" << hook::log::NSDELETE << ")[" << HS_ACC()
                        << "]: DeleteState "
                        << "Destroying Hook Namespace for " << account
                        << " namespace " << ns;

    auto sleAccount = view.peek(keylet::account(account));
    if (!sleAccount)
    {
        JLOG(ctx.j.fatal())
            << "HookSet(" << hook::log::NSDELETE_ACCOUNT << ")[" << HS_ACC()
            << "]: Account does not exist to destroy namespace from";
        return tefBAD_LEDGER;
    }

    // NSDELETE is an opportunistic deleter, following "delete if exists"
    // logic this way the flag can't block the SetHook transaction just
    // because, for example, the namespace was deleted in the previous
    // transaction but the hsFlags have not changed. Thus this is not an
    // error. Allow fall through to below in case, for some reason, the
    // namespace directory *does* exist but does not appear in the vector.

    Keylet dirKeylet = keylet::hookStateDir(account, ns);

    std::shared_ptr<SLE const> sleDirNode{};
    unsigned int uDirEntry{0};
    uint256 dirEntry{beast::zero};

    auto sleDir = view.peek(dirKeylet);

    bool const dirExists = !!sleDir;
    bool const dirEmpty = dirExists && dirIsEmpty(view, dirKeylet);

    if (!dirExists || dirEmpty)
    {
        // directory doesn't exist or is empty, this is a success condition
        if (hook::removeHookNamespaceEntry(*sleAccount, ns))
            view.update(sleAccount);
        if (dirExists)
            view.erase(sleDir);
        return tesSUCCESS;
    }

    // fall through to here means we must prune the entries from the directory
    if (!cdirFirst(view, dirKeylet.key, sleDirNode, uDirEntry, dirEntry))
    {
        JLOG(ctx.j.fatal()) << "HookSet(" << hook::log::NSDELETE_DIRECTORY
                            << ")[" << HS_ACC() << "]: DeleteState "
                            << "directory missing ";
        return tefINTERNAL;
    }

    bool const fixEnabled = ctx.rules.enabled(fixNSDelete);
    bool partialDelete = false;
    uint32_t oldStateCount = sleAccount->getFieldU32(sfHookStateCount);

    std::vector<uint256> toDelete;
    toDelete.reserve(sleDir->getFieldV256(sfIndexes).size());
    do
    {
        if (fixEnabled && toDelete.size() >= hook::maxNamespaceDelete())
        {
            partialDelete = true;
            break;
        }

        // Make sure any directory node types that we find are the kind
        // we can delete.
        Keylet const itemKeylet{ltCHILD, dirEntry};
        auto sleItem = view.peek(itemKeylet);
        if (!sleItem)
        {
            // Directory node has an invalid index.  Bail out.
            JLOG(ctx.j.fatal())
                << "HookSet(" << hook::log::NSDELETE_DIR_ENTRY << ")["
                << HS_ACC() << "]: DeleteState "
                << "directory node in ledger " << view.seq() << " "
                << "has index to object that is missing: "
                << to_string(dirEntry);
            return tefBAD_LEDGER;
        }

        auto nodeType = sleItem->getFieldU16(sfLedgerEntryType);

        if (nodeType != ltHOOK_STATE)
        {
            JLOG(ctx.j.fatal())
                << "HookSet(" << hook::log::NSDELETE_NONSTATE << ")["
                << HS_ACC() << "]: DeleteState "
                << "directory node in ledger " << view.seq() << " "
                << "has non-ltHOOK_STATE entry " << to_string(dirEntry);
            return tefBAD_LEDGER;
        }

        toDelete.push_back(uint256::fromVoid(itemKeylet.key.data()));

    } while (cdirNext(view, dirKeylet.key, sleDirNode, uDirEntry, dirEntry));

    // delete it!
    for (auto const& itemKey : toDelete)
    {
        auto const& sleItem = view.peek({ltHOOK_STATE, itemKey});

        if (!sleItem)
        {
            JLOG(ctx.j.warn())
                << "HookSet(" << hook::log::NSDELETE_ENTRY << ")[" << HS_ACC()
                << "]: DeleteState "
                << "Namespace ltHOOK_STATE entry was not found in ledger: "
                << itemKey;
            continue;
        }

        auto const hint = (*sleItem)[sfOwnerNode];
        if (!view.dirRemove(dirKeylet, hint, itemKey, false))
        {
            JLOG(ctx.j.fatal())
                << "HookSet(" << hook::log::NSDELETE_DIR << ")[" << HS_ACC()
                << "]: DeleteState "
                << "directory node in ledger " << view.seq() << " "
                << "could not be deleted.";
            return tefBAD_LEDGER;
        }
        view.erase(sleItem);
    }

    uint32_t stateCount = oldStateCount - toDelete.size();
    if (stateCount > oldStateCount)
    {
        JLOG(ctx.j.fatal()) << "HookSet(" << hook::log::NSDELETE_COUNT << ")["
                            << HS_ACC() << "]: DeleteState "
                            << "stateCount less than zero (overflow)";

        return tefBAD_LEDGER;
    }

    if (stateCount == 0)
        sleAccount->makeFieldAbsent(sfHookStateCount);
    else
        sleAccount->setFieldU32(sfHookStateCount, stateCount);

    if (ctx.rules.enabled(fixNSDelete))
        adjustOwnerCount(view, sleAccount, -toDelete.size(), ctx.j);

    if (!partialDelete && sleAccount->isFieldPresent(sfHookNamespaces))
        hook::removeHookNamespaceEntry(*sleAccount, ns);

    view.update(sleAccount);
    return partialDelete ? tesPARTIAL : tesSUCCESS;
}

// returns true if the reference counted ledger entry should be marked for
// deletion i.e. it has a zero reference count after the decrement is completed
// otherwise returns false (but still decrements reference count)
bool
reduceReferenceCount(std::shared_ptr<STLedgerEntry>& sle)
{
    if (sle && sle->isFieldPresent(sfReferenceCount))
    {
        // reduce reference count on reference counted object
        uint64_t refCount = sle->getFieldU64(sfReferenceCount);
        if (refCount > 0)
        {
            refCount--;
            sle->setFieldU64(sfReferenceCount, refCount);
        }

        return refCount <= 0;
    }
    return false;
}

void
incrementReferenceCount(std::shared_ptr<STLedgerEntry>& sle)
{
    if (!sle)
        return;

    uint64_t rc = sle->isFieldPresent(sfReferenceCount)
        ? sle->getFieldU64(sfReferenceCount) + 1
        : 1;

    if (rc == 0)
    {
        // overflow should be impossible...
        // it would require ~ 4.6*10^18 accounts each pointing all four hook
        // entries at one hook definition handle this somewhat gracefully anyway
        // if it somehow did happen
        return;
    }

    sle->setFieldU64(sfReferenceCount, rc);
}

TER
updateHookParameters(
    SetHookCtx& ctx,
    ripple::STObject const& hookSetObj,
    std::shared_ptr<STLedgerEntry>& oldDefSLE,
    ripple::STArray const& oldParameters,
    ripple::STObject& newHook)
{
    const int paramKeyMax = hook::maxHookParameterKeySize();
    const int paramValueMax = hook::maxHookParameterValueSize();

    std::map<ripple::Blob, std::optional<ripple::Blob>> parameters;

    // When parameters are specified in a HookSetObj they can come in three
    // flavours:
    // 1. Name + Value both present and populated   {ParameterName: "X",
    // ParameterValue: "Y"}
    // 2. Name + Empty Value   (EV)                 {ParameterName: "X",
    // ParameterValue: ""}
    // 3. Name + Absent Value  (AV)                 {ParameterName: "X"}
    //
    // Case 1 will set the pair in the user's ltHook, unless it matches the
    // HookDef default,
    //        in which case it will be deleted.
    // Case 2 will set the pair with null-value in the user's ltHook, unless
    // there is no default,
    //        in which case it will be deleted.
    // Case 3 will delete the pair in the user's ltHook irrespective of any
    // other factor, effectively
    //        producing a reset to default, even if the default is that there is
    //        no parameter by that name.
    // Unpopulated optional in the map means AV, populated with {} (.empty())
    // means EV.

    // first pull the old parameters into a map
    for (auto const& hookParameterObj : oldParameters)
    {
        auto const pname = hookParameterObj.getFieldVL(sfHookParameterName);
        if (hookParameterObj.isFieldPresent(sfHookParameterValue))
            parameters[pname] = {
                hookParameterObj.getFieldVL(sfHookParameterValue)};
        else
            parameters[pname] = {};
    }

    // ... then override old parameters with specified parameters
    auto const& hookParameters = hookSetObj.getFieldArray(sfHookParameters);
    for (auto const& hookParameterObj : hookParameters)
    {
        auto const& pname = hookParameterObj.getFieldVL(sfHookParameterName);
        // specifying a missing value:
        // deletes the parameter if there is no default value for that parameter
        // on the hook definition sits as a blank / deletion marker if there is
        // a default value on the hook definition
        if (hookParameterObj.isFieldPresent(sfHookParameterValue))
            parameters[pname] = {
                hookParameterObj.getFieldVL(sfHookParameterValue)};
        else
            parameters[pname] = {};
    }

    std::set<Blob> explicitBlanks;

    // then erase anything that is the same as the definition's default
    // parameters
    if (parameters.size() > 0 && oldDefSLE &&
        oldDefSLE->isFieldPresent(sfHookParameters))
    {
        auto const& defParameters = oldDefSLE->getFieldArray(sfHookParameters);

        for (auto const& hookParameterObj : defParameters)
        {
            ripple::Blob n = hookParameterObj.getFieldVL(sfHookParameterName);
            ripple::Blob v = hookParameterObj.getFieldVL(sfHookParameterValue);

            if (parameters.find(n) != parameters.end())
            {
                auto const& pv = parameters[n];
                if (!pv || *pv == v)
                {
                    // Absent value, reset to default
                    // Or value matches exactly, so reset to default also
                    parameters.erase(n);
                }
                else if ((*pv).empty())
                {
                    // Empty value, this is explicit override
                    explicitBlanks.emplace(n);
                }
            }
        }
    }

    // remove anything that was EV or AV but there was no corresponding existing
    // param in the hook definition to blank against (this is then a param
    // delete)

    std::set<Blob> toErase;
    for (auto& [pn, pv] : parameters)
    {
        if (!pv)  // MV
            toErase.emplace(pn);
        else if (
            (*pv).empty()  // EV
            && explicitBlanks.find(pn) == explicitBlanks.end())
            toErase.emplace(pn);
    }

    for (auto& pn : toErase)
        parameters.erase(pn);

    int parameterCount = (int)(parameters.size());
    if (parameterCount > 16)
    {
        JLOG(ctx.j.fatal())
            << "HookSet(" << hook::log::HOOK_PARAMS_COUNT << ")[" << HS_ACC()
            << "]: Malformed transaction: Txn would result in too many "
               "parameters on hook";
        return tecINTERNAL;
    }

    STArray newParameters{sfHookParameters, parameterCount};
    for (const auto& [parameterName, parameterValue] : parameters)
    {
        if (!parameterValue)
        {
            JLOG(ctx.j.warn()) << "HookSet did not filter out Absent Values "
                                  "before getting to set routine.";
            continue;
        }

        if (parameterName.size() > paramKeyMax ||
            (*parameterValue).size() > paramValueMax)
        {
            JLOG(ctx.j.fatal())
                << "HookSet(" << hook::log::HOOK_PARAM_SIZE << ")[" << HS_ACC()
                << "]: Malformed transaction: Txn would result in a too large "
                   "parameter name/value on hook";
            return tecINTERNAL;
        }

        STObject param{sfHookParameter};
        param.setFieldVL(sfHookParameterName, parameterName);
        param.setFieldVL(sfHookParameterValue, *parameterValue);
        newParameters.push_back(std::move(param));
    }

    if (newParameters.size() > 0)
        newHook.setFieldArray(sfHookParameters, std::move(newParameters));

    return tesSUCCESS;
}

struct KeyletComparator
{
    bool
    operator()(const Keylet& lhs, const Keylet& rhs) const
    {
        return lhs.type < rhs.type ||
            (lhs.type == rhs.type && lhs.key < rhs.key);
    }
};

TER
SetHook::setHook()
{
    /**
     * Each account has optionally an ltHOOK object
     * Which contains an array (sfHooks) of sfHook objects
     * The set hook transaction also contains an array (sfHooks) of sfHook
     * objects These two arrays are mapped 1-1 when updating, inserting or
     * deleting hooks When the user submits a new hook that does not yet exist
     * on the ledger an ltHOOK_DEFINITION object is created Further users
     * setting the same hook code will reference this object using sfHookHash.
     */

    SetHookCtx ctx{
        .j = ctx_.app.journal("View"),
        .tx = ctx_.tx,
        .app = ctx_.app,
        .rules = ctx_.view().rules()};

    const int blobMax = hook::maxHookWasmSize();
    auto const accountKeylet = keylet::account(account_);
    auto const hookKeylet = keylet::hook(account_);

    auto accountSLE = view().peek(accountKeylet);

    ripple::STArray newHooks{sfHooks, 8};
    auto newHookSLE = std::make_shared<SLE>(hookKeylet);

    int oldHookCount = 0;
    std::optional<std::reference_wrapper<ripple::STArray const>> oldHooks;
    auto const& oldHookSLE = view().peek(hookKeylet);

    if (oldHookSLE)
    {
        oldHooks = oldHookSLE->getFieldArray(sfHooks);
        oldHookCount = (oldHooks->get()).size();
    }

    std::set<ripple::Keylet, KeyletComparator> keyletsToDestroy{};
    std::map<ripple::Keylet, std::shared_ptr<SLE>, KeyletComparator>
        slesToInsert{};
    std::map<ripple::Keylet, std::shared_ptr<SLE>, KeyletComparator>
        slesToUpdate{};

    std::set<uint256> namespacesToDestroy{};

    int hookSetNumber = -1;
    auto const& hookSets = ctx.tx.getFieldArray(sfHooks);

    int hookSetCount = hookSets.size();

    for (hookSetNumber = 0;
         hookSetNumber < std::max(oldHookCount, hookSetCount);
         ++hookSetNumber)
    {
        ripple::STObject newHook{sfHook};
        std::optional<std::reference_wrapper<ripple::STObject const>> oldHook;
        // an existing hook would only be present if the array slot also exists
        // on the ltHOOK object
        if (hookSetNumber < oldHookCount)
            oldHook = std::cref((oldHooks->get()[hookSetNumber])
                                    .downcast<ripple::STObject const>());

        std::optional<std::reference_wrapper<ripple::STObject const>>
            hookSetObj;
        if (hookSetNumber < hookSetCount)
            hookSetObj = std::cref(
                (hookSets[hookSetNumber]).downcast<ripple::STObject const>());

        std::optional<ripple::uint256> oldNamespace;
        std::optional<ripple::uint256> defNamespace;
        std::optional<ripple::Keylet> oldDirKeylet;
        std::optional<ripple::Keylet> oldDefKeylet;
        std::optional<ripple::Keylet> newDefKeylet;
        std::shared_ptr<STLedgerEntry> oldDefSLE;
        std::shared_ptr<STLedgerEntry> newDefSLE;
        std::shared_ptr<STLedgerEntry> oldDirSLE;

        std::optional<ripple::uint256> newNamespace;
        std::optional<ripple::Keylet> newDirKeylet;

        std::optional<uint256> oldHookOn;
        std::optional<uint256> newHookOn;
        std::optional<uint256> defHookOn;

        // when hsoCREATE is invoked it populates this variable in case the hook
        // definition already exists and the operation falls through into a
        // hsoINSTALL operation instead
        std::optional<ripple::uint256> createHookHash;
        /**
         * This is the primary HookSet loop. We iterate the sfHooks array inside
         * the txn each entry of this array is available as hookSetObj.
         * Depending on whether or not an existing hook is present in the array
         * slot we are currently up to this hook and its various attributes are
         * available in the optionals prefixed with old. Even if an existing
         * hook is being modified by the sethook obj, we create a newHook obj so
         * a degree of copying is required.
         */

        std::optional<uint32_t> flags;

        if (hookSetObj && hookSetObj->get().isFieldPresent(sfFlags))
            flags = hookSetObj->get().getFieldU32(sfFlags);

        HookSetOperation op = hsoNOOP;

        if (hookSetObj)
            op = inferOperation(hookSetObj->get());

        // these flags are not able to be passed onto the ledger object
        int newFlags = 0;
        if (flags)
        {
            newFlags = *flags;
            if (newFlags & hsfOVERRIDE)
                newFlags -= hsfOVERRIDE;

            if (newFlags & hsfNSDELETE)
                newFlags -= hsfNSDELETE;
        }

        // if an existing hook exists at this position in the chain then extract
        // the relevant fields
        if (oldHook && oldHook->get().isFieldPresent(sfHookHash))
        {
            oldDefKeylet =
                keylet::hookDefinition(oldHook->get().getFieldH256(sfHookHash));
            oldDefSLE = view().peek(*oldDefKeylet);
            if (oldDefSLE)
                defNamespace = oldDefSLE->getFieldH256(sfHookNamespace);

            if (oldHook->get().isFieldPresent(sfHookNamespace))
                oldNamespace = oldHook->get().getFieldH256(sfHookNamespace);
            else if (defNamespace)
                oldNamespace = *defNamespace;

            oldDirKeylet = keylet::hookStateDir(account_, *oldNamespace);
            oldDirSLE = view().peek(*oldDirKeylet);
            if (oldDefSLE)
                defHookOn = oldDefSLE->getFieldH256(sfHookOn);

            if (oldHook->get().isFieldPresent(sfHookOn))
                oldHookOn = oldHook->get().getFieldH256(sfHookOn);
            else if (defHookOn)
                oldHookOn = *defHookOn;
        }

        // in preparation for three way merge populate fields if they are
        // present on the HookSetObj
        if (hookSetObj)
        {
            if (hookSetObj->get().isFieldPresent(sfHookHash))
            {
                newDefKeylet = keylet::hookDefinition(
                    hookSetObj->get().getFieldH256(sfHookHash));
                newDefSLE = view().peek(*newDefKeylet);
            }

            if (hookSetObj->get().isFieldPresent(sfHookOn))
                newHookOn = hookSetObj->get().getFieldH256(sfHookOn);

            if (hookSetObj->get().isFieldPresent(sfHookNamespace))
            {
                newNamespace = hookSetObj->get().getFieldH256(sfHookNamespace);
                newDirKeylet = keylet::hookStateDir(account_, *newNamespace);
            }
        }

        // users may destroy a namespace in any operation except NOOP and
        // INVALID
        if (flags && (*flags & hsfNSDELETE))
        {
            if (op == hsoNOOP || op == hsoINVALID)
            {
                // don't do any namespace deletion
            }
            else if (op == hsoNSDELETE && newDirKeylet)
            {
                printf("Marking a namespace for destruction.... NSDELETE\n");
                namespacesToDestroy.emplace(*newNamespace);
            }
            else if (oldDirKeylet)
            {
                printf(
                    "Marking a namespace for destruction.... non-NSDELETE\n");
                namespacesToDestroy.emplace(*oldNamespace);
            }
            else
            {
                JLOG(ctx.j.warn())
                    << "HookSet(" << hook::log::NSDELETE_NOTHING << ")["
                    << HS_ACC()
                    << "]: SetHook hsoNSDELETE specified but nothing to delete";
            }
        }

        // if there is only an existing hook, without a HookSetObj then it is
        // logically impossible for the operation to not be NOOP
        if (!hookSetObj && op != hsoNOOP)
        {
            JLOG(ctx.j.warn())
                << "HookSet(" << hook::log::INTERNAL_ERROR << ")[" << HS_ACC()
                << "]: Logic error. !hookSetObj && op != hsoNOOP";
            return tecINTERNAL;
        }

        switch (op)
        {
            // this case is handled directly above already
            case hsoNSDELETE:
            case hsoNOOP: {
                // if a hook already exists here then migrate it to the new
                // array if it doesn't exist just place a blank object here
                newHooks.push_back(
                    oldHook ? oldHook->get() : ripple::STObject{sfHook});
                continue;
            }

                // every case below here is guarenteed to have a populated
                // hookSetObj by the assert statement above

            case hsoDELETE: {
                if (!flags || !(*flags & hsfOVERRIDE))
                {
                    JLOG(ctx.j.trace()) << "HookSet(" << hook::log::DELETE_FLAG
                                        << ")[" << HS_ACC()
                                        << "]: SetHook delete operation "
                                           "requires hsfOVERRIDE flag";
                    return tecREQUIRES_FLAG;
                }

                // place an empty corresponding Hook
                newHooks.push_back(ripple::STObject{sfHook});

                if (!oldHook)
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::DELETE_NOTHING << ")["
                        << HS_ACC()
                        << "]: SetHook delete operation deletes non-existent "
                           "hook";

                    continue;
                }

                // decrement the hook definition and mark it for deletion if
                // appropriate
                if (oldDefSLE)
                {
                    if (reduceReferenceCount(oldDefSLE))
                        keyletsToDestroy.emplace(*oldDefKeylet);
                    else
                        slesToUpdate.emplace(*oldDefKeylet, oldDefSLE);
                }

                continue;
            }

            case hsoUPDATE: {
                // check if there's a hook here to modify
                if (!oldDefSLE || !oldHook)
                    return tecNO_ENTRY;

                // initially carry over the prior non-array values, whatever
                // those were
                newHook.setFieldH256(
                    sfHookHash, oldHook->get().getFieldH256(sfHookHash));
                if (oldHook->get().isFieldPresent(sfHookOn))
                    newHook.setFieldH256(
                        sfHookOn, oldHook->get().getFieldH256(sfHookOn));
                if (oldHook->get().isFieldPresent(sfHookNamespace))
                    newHook.setFieldH256(
                        sfHookNamespace,
                        oldHook->get().getFieldH256(sfHookNamespace));

                // set the namespace if it differs from the definition namespace
                if (newNamespace)
                {
                    if (*defNamespace == *newNamespace)
                    {
                        if (newHook.isFieldPresent(sfHookNamespace))
                            newHook.makeFieldAbsent(sfHookNamespace);
                    }
                    else
                        newHook.setFieldH256(sfHookNamespace, *newNamespace);
                }

                // set the hookon field if it differs from definition
                if (newHookOn)
                {
                    if (*defHookOn == *newHookOn)
                    {
                        if (newHook.isFieldPresent(sfHookOn))
                            newHook.makeFieldAbsent(sfHookOn);
                    }
                    else
                        newHook.setFieldH256(sfHookOn, *newHookOn);
                }

                // parameters
                if (hookSetObj->get().isFieldPresent(sfHookParameters) &&
                    hookSetObj->get().getFieldArray(sfHookParameters).empty())
                {
                    // delete parameters operation
                }
                else
                {
                    STArray const oldParams =
                        oldHook->get().isFieldPresent(sfHookParameters)
                        ? oldHook->get().getFieldArray(sfHookParameters)
                        : STArray{sfHookParameters};

                    TER result = updateHookParameters(
                        ctx, hookSetObj->get(), oldDefSLE, oldParams, newHook);

                    if (!isTesSuccess(result))
                        return result;
                }

                // grants
                if (hookSetObj->get().isFieldPresent(sfHookGrants))
                {
                    auto const& grants =
                        hookSetObj->get().getFieldArray(sfHookGrants);
                    if (grants.empty())
                    {
                        // delete operation
                    }
                    else
                        newHook.setFieldArray(sfHookGrants, grants);
                }
                else
                {
                    // if they're not then retain them as they were
                    if (oldHook->get().isFieldPresent(sfHookGrants))
                        newHook.setFieldArray(
                            sfHookGrants,
                            oldHook->get().getFieldArray(sfHookGrants));
                }

                if (flags)
                    newHook.setFieldU32(sfFlags, *flags);

                newHooks.push_back(std::move(newHook));
                continue;
            }

            case hsoCREATE: {
                if (oldHook && oldHook->get().isFieldPresent(sfHookHash) &&
                    (!flags || !(*flags & hsfOVERRIDE)))
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::CREATE_FLAG << ")["
                        << HS_ACC()
                        << "]: SetHook create operation would override but "
                           "hsfOVERRIDE flag wasn't specified";
                    return tecREQUIRES_FLAG;
                }

                uint16_t hookApiVersion = hookSetObj->get().getFieldU16(sfHookApiVersion);

                if (hookApiVersion == 1 && !hookSetObj->get().isFieldPresent(sfFee))
                {
                    JLOG(ctx.j.warn())
                        << "HookSet(" << hook::log::JS_FEE_MISSING << ")["
                        << HS_ACC()
                        << "]: Malformed transaction: SetHook operation for JS Hook missing fee.";
                    return tecINTERNAL;
                }

                ripple::Blob wasmBytes =
                    hookSetObj->get().getFieldVL(sfCreateCode);

                if (wasmBytes.size() > blobMax)
                {
                    JLOG(ctx.j.warn())
                        << "HookSet(" << hook::log::WASM_TOO_BIG << ")["
                        << HS_ACC()
                        << "]: Malformed transaction: SetHook operation would "
                           "create blob larger than max";
                    return tecINTERNAL;
                }

                createHookHash = ripple::sha512Half_s(
                    ripple::Slice(wasmBytes.data(), wasmBytes.size()));

                auto keylet = ripple::keylet::hookDefinition(*createHookHash);

                if (view().exists(keylet))
                {
                    newDefSLE = view().peek(keylet);
                    newDefKeylet = keylet;

                    // this falls through to hsoINSTALL
                }
                else if (slesToInsert.find(keylet) != slesToInsert.end())
                {
                    // this hook was created in this very loop but isn't yet on
                    // the ledger
                    newDefSLE = slesToInsert[keylet];
                    newDefKeylet = keylet;

                    // this falls through to hsoINSTALL
                }
                else
                {
                    uint64_t maxInstrCountHook = 0;
                    uint64_t maxInstrCountCbak = 0;

                    // create hook definition SLE
                    try
                    {
                        auto valid =
                            validateHookSetEntry(ctx, hookSetObj->get());

                        // if invalid return an error
                        if (std::holds_alternative<bool>(valid))
                        {
                            if (!std::get<bool>(valid))
                            {
                                JLOG(ctx.j.warn())
                                    << "HookSet(" << hook::log::WASM_INVALID
                                    << ")[" << HS_ACC()
                                    << "]: Malformed transaction: SetHook "
                                       "operation would create invalid hook "
                                       "wasm";
                                return tecINTERNAL;
                            }
                            else
                                assert(false);  // should never happen
                        }

                        // otherwise assign instruction counts
                        std::tie(maxInstrCountHook, maxInstrCountCbak) =
                            std::get<std::pair<uint64_t, uint64_t>>(valid);
                    }
                    catch (std::exception& e)
                    {
                        JLOG(ctx.j.warn())
                            << "HookSet(" << hook::log::WASM_INVALID << ")["
                            << HS_ACC()
                            << "]: Malformed transaction: SetHook operation "
                               "would create invalid hook wasm: " << e.what();
                        return tecINTERNAL;
                    }

                    // decrement the hook definition and mark it for deletion if
                    // appropriate
                    if (oldDefSLE)
                    {
                        if (reduceReferenceCount(oldDefSLE))
                            keyletsToDestroy.emplace(*oldDefKeylet);
                        else
                            slesToUpdate.emplace(*oldDefKeylet, oldDefSLE);
                    }

                    // override instruction count with fee for js
                    if (hookApiVersion == 1)
                    {
                        uint64_t fee = hookSetObj->get().getFieldAmount(sfFee).xrp().drops();
                        maxInstrCountHook = fee;
                        maxInstrCountCbak = fee; // RH TODO: add a second fee for cbak?
                    }

                    auto newHookDef = std::make_shared<SLE>(keylet);
                    newHookDef->setFieldH256(sfHookHash, *createHookHash);
                    newHookDef->setFieldH256(sfHookOn, *newHookOn);
                    newHookDef->setFieldH256(sfHookNamespace, *newNamespace);
                    newHookDef->setFieldArray(
                        sfHookParameters,
                        hookSetObj->get().isFieldPresent(sfHookParameters)
                            ? hookSetObj->get().getFieldArray(sfHookParameters)
                            : STArray{});
                    newHookDef->setFieldU16(sfHookApiVersion, hookApiVersion);
                    newHookDef->setFieldVL(sfCreateCode, wasmBytes);
                    newHookDef->setFieldH256(
                        sfHookSetTxnID, ctx.tx.getTransactionID());
                    newHookDef->setFieldU64(sfReferenceCount, 1);
                    newHookDef->setFieldAmount(
                        sfFee,
                        XRPAmount{
                            hook::computeExecutionFee(maxInstrCountHook)});
                    if (maxInstrCountCbak > 0)
                        newHookDef->setFieldAmount(
                            sfHookCallbackFee,
                            XRPAmount{
                                hook::computeExecutionFee(maxInstrCountCbak)});

                    if (flags)
                        newHookDef->setFieldU32(sfFlags, newFlags);
                    else
                        newHookDef->setFieldU32(sfFlags, 0);

                    if (hookSetObj->get().isFieldPresent(sfHookGrants))
                    {
                        auto const& grants =
                            hookSetObj->get().getFieldArray(sfHookGrants);
                        if (!grants.empty())
                            newHook.setFieldArray(sfHookGrants, grants);
                    }

                    slesToInsert.emplace(keylet, newHookDef);
                    newHook.setFieldH256(sfHookHash, *createHookHash);
                    newHooks.push_back(std::move(newHook));
                    continue;
                }
                [[fallthrough]];
            }

            // the create operation above falls through to this install
            // operation if the sfCreateCode that would otherwise be created
            // already exists on the ledger
            case hsoINSTALL: {
                if (oldHook && oldHook->get().isFieldPresent(sfHookHash) &&
                    (!flags || !(*flags & hsfOVERRIDE)))
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::INSTALL_FLAG << ")["
                        << HS_ACC()
                        << "]: SetHook install operation would override but "
                           "hsfOVERRIDE flag wasn't specified";
                    return tecREQUIRES_FLAG;
                }

                // check if the target hook exists
                if (!newDefSLE)
                {
                    JLOG(ctx.j.trace())
                        << "HookSet(" << hook::log::INSTALL_MISSING << ")["
                        << HS_ACC()
                        << "]: SetHook install operation specified HookHash "
                           "which does not exist on ledger";
                    return tecNO_ENTRY;
                }

                // decrement the hook definition and mark it for deletion if
                // appropriate
                if (oldDefSLE)
                {
                    if (reduceReferenceCount(oldDefSLE))
                        keyletsToDestroy.emplace(*oldDefKeylet);
                    else
                        slesToUpdate.emplace(*oldDefKeylet, oldDefSLE);
                }

                // set the hookhash on the new hook, and allow for a fall
                // through event from hsoCREATE
                if (!createHookHash)
                    createHookHash = hookSetObj->get().getFieldH256(sfHookHash);

                newHook.setFieldH256(sfHookHash, *createHookHash);

                // increment reference count of target HookDefintion
                incrementReferenceCount(newDefSLE);

                // change which definition we're using to the new target
                defNamespace = newDefSLE->getFieldH256(sfHookNamespace);
                defHookOn = newDefSLE->getFieldH256(sfHookOn);

                // set the namespace if it differs from the definition namespace
                if (newNamespace && *defNamespace != *newNamespace)
                    newHook.setFieldH256(sfHookNamespace, *newNamespace);

                // set the hookon field if it differs from definition
                if (newHookOn && *defHookOn != *newHookOn)
                    newHook.setFieldH256(sfHookOn, *newHookOn);

                // parameters
                TER result = updateHookParameters(
                    ctx,
                    hookSetObj->get(),
                    newDefSLE,
                    STArray{sfHookParameters},
                    newHook);

                if (!isTesSuccess(result))
                    return result;

                // if grants are provided set them
                if (hookSetObj->get().isFieldPresent(sfHookGrants))
                {
                    auto const& grants =
                        hookSetObj->get().getFieldArray(sfHookGrants);
                    if (!grants.empty())
                        newHook.setFieldArray(sfHookGrants, grants);
                }

                if (flags)
                    newHook.setFieldU32(sfFlags, newFlags);

                newHooks.push_back(std::move(newHook));

                slesToUpdate.emplace(*newDefKeylet, newDefSLE);
                continue;
            }

            case hsoINVALID:
            default: {
                JLOG(ctx.j.warn()) << "HookSet(" << hook::log::OPERATION_INVALID
                                   << ")[" << HS_ACC()
                                   << "]: Malformed transaction: sethook could "
                                      "not understand the desired operation.";
                return tecCLAIM;
            }
        }
    }

    int reserveDelta = 0;
    {
        // compute owner counts before modifying anything on ledger

        // Owner reserve is billed as follows:
        // sfHook: 1 reserve PER non-blank entry
        // sfParameters: 1 reserve PER entry
        // sfGrants are: 1 reserve PER entry
        // sfHookHash, sfHookNamespace, sfHookOn, sfHookApiVersion, sfFlags:
        // free

        // sfHookDefinition is not reserved because it is an unowned object,
        // rather the uploader is billed via fee according to the following:
        // sfCreateCode:     5000 drops per byte
        // sfHookParameters: 5000 drops per byte
        // other fields: free

        int oldHookReserve = 0;
        int newHookReserve = 0;

        auto const computeHookReserve = [](STObject const& hookObj) -> int {
            if (!hookObj.isFieldPresent(sfHookHash))
                return 0;

            int reserve{1};

            if (hookObj.isFieldPresent(sfHookParameters))
                reserve += hookObj.getFieldArray(sfHookParameters).size();

            if (hookObj.isFieldPresent(sfHookGrants))
                reserve += hookObj.getFieldArray(sfHookGrants).size();

            return reserve;
        };

        for (int i = 0; i < hook::maxHookChainLength(); ++i)
        {
            if (oldHooks && i < oldHookCount)
                oldHookReserve += computeHookReserve(((*oldHooks).get())[i]);

            if (i < newHooks.size())
                newHookReserve += computeHookReserve(newHooks[i]);
        }

        reserveDelta = newHookReserve - oldHookReserve;

        JLOG(j_.trace()) << "SetHook: "
                         << "newHookReserve: " << newHookReserve << " "
                         << "oldHookReserve: " << oldHookReserve << " "
                         << "reserveDelta: " << reserveDelta;

        int64_t newOwnerCount =
            (int64_t)(accountSLE->getFieldU32(sfOwnerCount)) + reserveDelta;

        if (newOwnerCount < 0 || newOwnerCount > 0xFFFFFFFFUL)
            return tefINTERNAL;

        auto const requiredDrops =
            view().fees().accountReserve((uint32_t)(newOwnerCount));
        if (mSourceBalance < requiredDrops)
            return tecINSUFFICIENT_RESERVE;
    }
    TER nsDeleteResult = tesSUCCESS;
    {
        // execution to here means we will enact changes to the ledger:

        // do any pending insertions
        for (auto const& [_, s] : slesToInsert)
            view().insert(s);

        // do any pending updates
        for (auto const& [_, s] : slesToUpdate)
            view().update(s);

        // clean up any Namespace directories marked for deletion and any zero
        // reference Hook Definitions
        for (auto const& ns : namespacesToDestroy)
            if (nsDeleteResult = destroyNamespace(ctx, view(), account_, ns);
                !isTesSuccess(nsDeleteResult))
                break;

        // do any pending removals
        for (auto const& p : keyletsToDestroy)
        {
            auto const sle = view().peek(p);
            if (!sle)
                continue;
            if (sle->isFieldPresent(sfReferenceCount))
            {
                uint64_t refCount = sle->getFieldU64(sfReferenceCount);
                if (refCount <= 0)
                    view().erase(sle);
            }
            else
                view().erase(sle);
        }

        // check if the new hook object is empty
        bool newHooksEmpty = true;
        for (auto const& h : newHooks)
        {
            if (h.isFieldPresent(sfHookHash))
            {
                newHooksEmpty = false;
                break;
            }
        }

        newHookSLE->setFieldArray(sfHooks, newHooks);
        newHookSLE->setAccountID(sfAccount, account_);

        // There are three possible final outcomes
        // Either the account's ltHOOK is deleted, updated or created.

        if (oldHookSLE && newHooksEmpty)
        {
            // DELETE ltHOOK
            auto const hint = (*oldHookSLE)[sfOwnerNode];
            if (!view().dirRemove(
                    keylet::ownerDir(account_), hint, hookKeylet.key, false))
            {
                JLOG(j_.fatal())
                    << "HookSet(" << hook::log::HOOK_DELETE << ")[" << HS_ACC()
                    << "]: Unable to delete ltHOOK from owner";
                return tefBAD_LEDGER;
            }
            view().erase(oldHookSLE);
        }
        else if (oldHookSLE && !newHooksEmpty)
        {
            // UPDATE ltHOOK
            if (view().rules().enabled(fixXahauV1))
            {
                (*newHookSLE)[sfOwnerNode] = (*oldHookSLE)[sfOwnerNode];
            }
            view().erase(oldHookSLE);
            view().insert(newHookSLE);
        }
        else if (!oldHookSLE && !newHooksEmpty)
        {
            // CREATE ltHOOK
            auto const page = view().dirInsert(
                keylet::ownerDir(account_),
                hookKeylet,
                describeOwnerDir(account_));

            JLOG(j_.trace())
                << "HookSet(" << hook::log::HOOK_ADD << ")[" << HS_ACC()
                << "]: Adding ltHook to account directory "
                << to_string(hookKeylet.key) << ": "
                << (page ? "success" : "failure");

            if (!page)
                return tecDIR_FULL;

            newHookSLE->setFieldU64(sfOwnerNode, *page);
            view().insert(newHookSLE);
        }
        else
        {
            // for clarity if this is a NO-OP
        }
    }

    if (reserveDelta != 0)
    {
        adjustOwnerCount(view(), accountSLE, reserveDelta, j_);
        view().update(accountSLE);
    }

    return nsDeleteResult;
}

}  // namespace ripple
