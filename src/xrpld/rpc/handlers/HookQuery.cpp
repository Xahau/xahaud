//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/Tuning.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/STData.h>
#include <xrpl/protocol/STDataType.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>
#include <optional>

namespace ripple {

/** RPC command that retreives hook state objects from a particular namespace in
   a particular account.
    {
      hook_account: <account>|<account_public_key>
      source_account: <account>|<account_public_key>
      function_name: <string>
      function_params: <json>
    }
*/

Json::Value
doHookQuery(RPC::JsonContext& context)
{
    auto const& params = context.params;
    if (!params.isMember(jss::hook_account))
        return RPC::missing_field_error(jss::hook_account);
    if (!params.isMember(jss::source_account))
        return RPC::missing_field_error(jss::source_account);

    if (!params.isMember(jss::function_params))
        return RPC::missing_field_error(jss::function_params);

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);
    if (ledger == nullptr)
        return result;

    AccountID hook_account;
    AccountID source_account;
    {
        auto const strIdent = params[jss::hook_account].asString();
        if (auto jv = RPC::accountFromString(hook_account, strIdent))
        {
            for (auto it = jv.begin(); it != jv.end(); ++it)
                result[it.memberName()] = *it;
            return result;
        }
    }
    {
        auto const strIdent = params[jss::source_account].asString();
        if (auto jv = RPC::accountFromString(source_account, strIdent))
        {
            for (auto it = jv.begin(); it != jv.end(); ++it)
                result[it.memberName()] = *it;
            return result;
        }
    }

    if (!params.isMember(jss::function_name))
        return RPC::missing_field_error(jss::function_name);

    if (!params.isMember(jss::function_params))
        return RPC::missing_field_error(jss::function_params);

    if (!params[jss::function_params].isObject())
        return RPC::invalid_field_error(jss::function_params);

    std::vector<hook::FunctionParameterValueVecWithName> paramDataMap;

    auto const function_name = params[jss::function_name].asString();

    const Json::Value& param_obj = params[jss::function_params];
    for (const auto& param_name : param_obj.getMemberNames())
    {
        try
        {
            STData data =
                dataFromJson(sfFunctionParameterValue, param_obj[param_name]);
            auto const param_name_hex = strUnHex(strHex(param_name));
            if (!param_name_hex)
                return RPC::invalid_field_error(param_name);
            paramDataMap.emplace_back(param_name_hex.value(), data);
        }
        catch (std::exception const& e)
        {
            return RPC::invalid_field_error(param_name);
        }
    }

    if (!ledger->exists(keylet::account(hook_account)) ||
        !ledger->exists(keylet::account(source_account)))
        return rpcError(rpcACT_NOT_FOUND);

    // get hook object
    auto hooksArray = ledger->read(keylet::hook(hook_account));
    if (!hooksArray || !hooksArray->isFieldPresent(sfHooks))
        return rpcError(rpcHOOK_NOT_FOUND);

    auto const& hooks = hooksArray->getFieldArray(sfHooks);
    if (hooks.size() != 1)
        return rpcError(rpcHOOK_NOT_FOUND);

    auto const& hookObj = hooks[0];
    auto const& hookDef =
        ledger->read(keylet::hookDefinition(hookObj.getFieldH256(sfHookHash)));
    if (!hookDef || !hookDef->isFieldPresent(sfHookApiVersion))
        return rpcError(rpcHOOK_NOT_FOUND);

    if (hookDef->getFieldU16(sfHookApiVersion) != 3)
        return rpcError(rpcHOOK_NOT_FOUND);

    // get FunctionParameters from function_name
    STArray const functions = hookObj.isFieldPresent(sfHookFunctions)
        ? hookObj.getFieldArray(sfHookFunctions)
        : hookDef->isFieldPresent(sfHookFunctions)
        ? hookDef->getFieldArray(sfHookFunctions)
        : STArray();

    std::optional<STObject> function = [&]() -> std::optional<STObject> {
        for (const auto& function : functions)
        {
            if (function.getFieldVL(sfFunctionName) ==
                strUnHex(strHex(function_name)).value())
                return function;
        }
        return {};
    }();

    if (!function || !function->isFlag(hffQUERY))
        return RPC::invalid_field_error(jss::function_name);

    STArray const& parameters = function->getFieldArray(sfFunctionParameters);

    auto const paramTypeMap = hook::getFunctionParameterTypeVec(parameters);

    std::vector<hook::FunctionParameterValueVec> sortedDataMap;
    for (const auto& param : paramTypeMap)
    {
        bool found = false;
        for (const auto& data : paramDataMap)
        {
            if (data.name == param.name)
            {
                sortedDataMap.emplace_back(data.value);
                found = true;
                break;
            }
        }
        if (!found)
            return RPC::invalid_field_error(strHex(param.name));
    }

    // access vm
    auto queryResult = Transactor::doFunctionalHookQuery(
        context.app,
        *ledger,
        source_account,
        hookObj,
        function_name,
        sortedDataMap);

    if (!queryResult)
        return RPC::invalid_field_error(jss::function_params);

    Json::Value query_results;
    for (const auto& [key, value] : queryResult.value())
        query_results[key] = value.getJson(JsonOptions::none);

    result[jss::hook_account] = toBase58(hook_account);
    result[jss::source_account] = toBase58(source_account);
    result[jss::query_results] = query_results;
    context.loadType = Resource::feeMediumBurdenRPC;
    return result;
}

}  // namespace ripple
