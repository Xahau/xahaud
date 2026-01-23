//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <ripple/app/hook/GasValidator.h>
#include <ripple/app/hook/Guard.h>
#include <ripple/app/hook/Macro.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Feature.h>
#include <wasmedge/wasmedge.h>

namespace hook {

std::optional<std::string>
validateWasmHostFunctionsForGas(
    std::vector<uint8_t> const& wasm,
    Rules const& rules,
    beast::Journal const& j)
{
    // Create WasmEdge Loader
    WasmEdge_LoaderContext* loader = WasmEdge_LoaderCreate(NULL);
    if (!loader)
    {
        return "Failed to create WasmEdge Loader";
    }

    // Parse WASM binary
    WasmEdge_ASTModuleContext* astModule = NULL;
    WasmEdge_Result res = WasmEdge_LoaderParseFromBuffer(
        loader, &astModule, wasm.data(), wasm.size());

    if (!WasmEdge_ResultOK(res))
    {
        WasmEdge_LoaderDelete(loader);
        const char* msg = WasmEdge_ResultGetMessage(res);
        return std::string("Failed to parse WASM: ") +
            (msg ? msg : "unknown error");
    }

    // Get import count
    uint32_t importCount = WasmEdge_ASTModuleListImportsLength(astModule);

    if (importCount == 0)
    {
        WasmEdge_ASTModuleDelete(astModule);
        WasmEdge_LoaderDelete(loader);
        JLOG(j.trace()) << "HookSet(" << hook::log::IMPORTS_MISSING
                        << "): WASM must import at least hook API functions";
        return "WASM must import at least hook API functions";
    }

    // Get imports (max 256)
    const WasmEdge_ImportTypeContext* imports[256];
    uint32_t actualImportCount = std::min(importCount, 256u);
    actualImportCount =
        WasmEdge_ASTModuleListImports(astModule, imports, actualImportCount);

    std::optional<std::string> error;

    // Check each import
    for (uint32_t i = 0; i < actualImportCount; i++)
    {
        WasmEdge_String moduleName =
            WasmEdge_ImportTypeGetModuleName(imports[i]);
        WasmEdge_String externalName =
            WasmEdge_ImportTypeGetExternalName(imports[i]);
        WasmEdge_ExternalType extType =
            WasmEdge_ImportTypeGetExternalType(imports[i]);

        // Only check function imports
        if (extType != WasmEdge_ExternalType_Function)
            continue;

        // Convert WasmEdge_String to std::string for comparison
        std::string modName(moduleName.Buf, moduleName.Length);
        std::string extName(externalName.Buf, externalName.Length);

        // Check module name is "env"
        if (modName != "env")
        {
            JLOG(j.trace())
                << "HookSet(" << hook::log::IMPORT_MODULE_ENV
                << "): Import module must be 'env', found: " << modName;
            error = "Import module must be 'env', found: " + modName;
            break;
        }

        // Check for forbidden _g function (guard function)
        if (extName == "_g")
        {
            JLOG(j.trace())
                << "HookSet(" << hook::log::IMPORT_ILLEGAL
                << "): Gas-type hooks cannot import _g (guard) function";
            error = "Gas-type hooks cannot import _g (guard) function";
            break;
        }

        // Check external name length
        if (extName.length() < 1 || extName.length() > 64)
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::IMPORT_NAME_BAD
                            << "): Import name length invalid: " << extName;
            error = "Import name length invalid: " + extName;
            break;
        }

        // Check against whitelist using find()
        bool found = false;

        // Check base whitelist (import_whitelist)
        if (hook_api::import_whitelist.find(extName) !=
            hook_api::import_whitelist.end())
        {
            found = true;
        }

        // Check extended whitelist (import_whitelist_1)
        if (!found && rules.enabled(featureHooksUpdate1) &&
            hook_api::import_whitelist_1.find(extName) !=
                hook_api::import_whitelist_1.end())
        {
            found = true;
        }

        if (!found)
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::IMPORT_ILLEGAL
                            << "): Import not in whitelist: " << extName;
            error = "Import not in whitelist: " + extName;
            break;
        }
    }

    // Cleanup
    WasmEdge_ASTModuleDelete(astModule);
    WasmEdge_LoaderDelete(loader);

    return error;
}

}  // namespace hook
