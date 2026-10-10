//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
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

#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/rpc/Context.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/hook/Guard.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>
#include <sstream>

namespace ripple {

Json::Value
doHookValidate(RPC::JsonContext& context)
{
    if (!context.app.config().HOOK_VALIDATE_RPC)
        return rpcError(rpcNOT_SUPPORTED);

    context.loadType = Resource::feeHeavyBurdenRPC;

    if (!context.params.isMember(jss::code) ||
        !context.params[jss::code].isString())
        return rpcError(rpcINVALID_PARAMS);

    auto const blob = strUnHex(context.params[jss::code].asString());
    if (!blob || blob->empty())
        return rpcError(rpcINVALID_PARAMS);

    if (blob->size() > hook::maxHookWasmSize())
        return RPC::make_error(
            rpcINVALID_PARAMS,
            "code exceeds the maximum hook size of " +
                std::to_string(hook::maxHookWasmSize()) + " bytes");

    auto const& rules = context.app.openLedger().current()->rules();

    std::ostringstream loggerStream;
    std::optional<std::reference_wrapper<std::basic_ostream<char>>> logger =
        loggerStream;
    std::string exceptionMsg;
    std::optional<std::pair<uint64_t, uint64_t>> result;
    try
    {
        result = validateGuards(
            *blob,
            logger,
            "",
            hook_api::getImportWhitelist(rules),
            hook_api::getGuardRulesVersion(rules));
    }
    catch (std::exception const& e)
    {
        exceptionMsg = e.what();
    }

    Json::Value jvResult(Json::objectValue);
    jvResult[jss::valid] = result.has_value();
    if (result)
    {
        jvResult[jss::instruction_count_hook] = std::to_string(result->first);
        jvResult[jss::instruction_count_cbak] = std::to_string(result->second);

        if (auto const vmError =
                hook::HookExecutor::validateWasm(blob->data(), blob->size()))
        {
            jvResult[jss::valid] = false;
            jvResult[jss::vm_error] = *vmError;
        }
    }

    Json::Value& log = jvResult[jss::log] = Json::arrayValue;
    std::istringstream lines(loggerStream.str());
    for (std::string line; std::getline(lines, line);)
        if (!line.empty())
            log.append(line);
    if (!exceptionMsg.empty())
        log.append(exceptionMsg);

    return jvResult;
}

}  // namespace ripple
