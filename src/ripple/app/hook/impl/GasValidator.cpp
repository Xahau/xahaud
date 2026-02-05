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
#include <ripple/basics/Expected.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Feature.h>
#include <wasmedge/wasmedge.h>

namespace hook {

Expected<bool, std::string>
validateExportSection(
    WasmEdge_ASTModuleContext* astModule,
    beast::Journal const& j)
{
    // Get export count
    uint32_t exportCount = WasmEdge_ASTModuleListExportsLength(astModule);
    if (exportCount == 0)
    {
        return Unexpected("WASM must export at least hook API functions");
    }

    // Get exports
    const WasmEdge_ExportTypeContext* exports[256];
    uint32_t actualExportCount = std::min(exportCount, 256u);
    actualExportCount =
        WasmEdge_ASTModuleListExports(astModule, exports, actualExportCount);

    // Track if we found required hook() function
    bool foundHook = false;
    bool foundCbak = false;

    // Check each export
    for (uint32_t i = 0; i < actualExportCount; i++)
    {
        // Only check function exports
        WasmEdge_ExternalType const type =
            WasmEdge_ExportTypeGetExternalType(exports[i]);
        if (type != WasmEdge_ExternalType_Function)
            continue;

        WasmEdge_String const name =
            WasmEdge_ExportTypeGetExternalName(exports[i]);
        std::string nameStr(name.Buf, name.Length);

        if (nameStr.starts_with("__"))
        {
            // skip runtime support functions
            continue;
        }

        // Only allow hook() and cbak() exports
        if (nameStr != "hook" && nameStr != "cbak")
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::EXPORT_MISSING
                            << "): Unauthorized export function '" << nameStr
                            << "'. Only 'hook' and 'cbak' are allowed";
            return Unexpected(
                "Unauthorized export function '" + nameStr +
                "'. Only 'hook' and 'cbak' are allowed");
        }

        if (nameStr == "hook")
            foundHook = true;
        if (nameStr == "cbak")
            foundCbak = true;

        // Get function type to validate signature
        WasmEdge_FunctionTypeContext const* functionType =
            WasmEdge_ExportTypeGetFunctionType(astModule, exports[i]);

        // Validate parameter count (must be exactly 1)
        uint32_t paramCount =
            WasmEdge_FunctionTypeGetParametersLength(functionType);
        if (paramCount != 1)
        {
            JLOG(j.trace())
                << "HookSet("
                << (nameStr == "hook" ? hook::log::EXPORT_HOOK_FUNC
                                      : hook::log::EXPORT_CBAK_FUNC)
                << "): Function '" << nameStr
                << "' must have exactly 1 parameter, found " << paramCount;
            return Unexpected(
                "Function '" + nameStr +
                "' must have exactly 1 parameter of type uint32_t");
        }

        // Validate parameter type (must be i32 / uint32_t)
        WasmEdge_ValType parameters[1];
        WasmEdge_FunctionTypeGetParameters(functionType, parameters, 1);
        if (parameters[0] != WasmEdge_ValType_I32)
        {
            JLOG(j.trace()) << "HookSet("
                            << (nameStr == "hook" ? hook::log::EXPORT_HOOK_FUNC
                                                  : hook::log::EXPORT_CBAK_FUNC)
                            << "): Function '" << nameStr
                            << "' parameter must be uint32_t (i32), found type "
                            << parameters[0];
            return Unexpected(
                "Function '" + nameStr + "' parameter must be uint32_t (i32)");
        }

        // Validate return type (must be i64 / uint64_t)
        uint32_t returnCount =
            WasmEdge_FunctionTypeGetReturnsLength(functionType);
        if (returnCount != 1)
        {
            JLOG(j.trace())
                << "HookSet("
                << (nameStr == "hook" ? hook::log::EXPORT_HOOK_FUNC
                                      : hook::log::EXPORT_CBAK_FUNC)
                << "): Function '" << nameStr
                << "' must return exactly 1 value, found " << returnCount;
            return Unexpected(
                "Function '" + nameStr +
                "' must return exactly 1 value of type uint64_t");
        }

        WasmEdge_ValType returns[1];
        WasmEdge_FunctionTypeGetReturns(functionType, returns, 1);
        if (returns[0] != WasmEdge_ValType_I64)
        {
            JLOG(j.trace())
                << "HookSet("
                << (nameStr == "hook" ? hook::log::EXPORT_HOOK_FUNC
                                      : hook::log::EXPORT_CBAK_FUNC)
                << "): Function '" << nameStr
                << "' return type must be uint64_t (i64), found type "
                << returns[0];
            return Unexpected(
                "Function '" + nameStr +
                "' return type must be uint64_t (i64)");
        }
    }

    // Ensure hook() function was exported (required)
    if (!foundHook)
    {
        JLOG(j.trace()) << "HookSet(" << hook::log::EXPORT_MISSING
                        << "): Required function 'hook' not found in exports";
        return Unexpected("Required function 'hook' not found in exports");
    }

    return foundCbak;
}

Expected<void, std::string>
validateImportSection(
    WasmEdge_ASTModuleContext* astModule,
    Rules const& rules,
    beast::Journal const& j)
{
    // Get import count
    uint32_t importCount = WasmEdge_ASTModuleListImportsLength(astModule);

    if (importCount == 0)
    {
        JLOG(j.trace()) << "HookSet(" << hook::log::IMPORTS_MISSING
                        << "): WASM must import at least hook API functions";
        return Unexpected("WASM must import at least hook API functions");
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
            return Unexpected("Import module must be 'env', found: " + modName);
        }

        // Check for forbidden _g function (guard function)
        if (extName == "_g")
        {
            JLOG(j.trace())
                << "HookSet(" << hook::log::IMPORT_ILLEGAL
                << "): Gas-type hooks cannot import _g (guard) function";
            return Unexpected(
                "Gas-type hooks cannot import _g (guard) function");
        }

        // Determine which whitelist contains the function and get expected
        // signature
        std::vector<uint8_t> const* expectedSig = nullptr;
        auto baseIt = hook_api::import_whitelist.find(extName);
        if (baseIt != hook_api::import_whitelist.end())
        {
            expectedSig = &baseIt->second;
        }
        else if (rules.enabled(featureHooksUpdate1))
        {
            auto extIt = hook_api::import_whitelist_1.find(extName);
            if (extIt != hook_api::import_whitelist_1.end())
            {
                expectedSig = &extIt->second;
            }
        }

        // Function not in any whitelist
        if (!expectedSig)
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::IMPORT_ILLEGAL
                            << "): Import not in whitelist: " << extName;
            return Unexpected("Import not in whitelist: " + extName);
        }

        // Get function type for signature validation
        WasmEdge_FunctionTypeContext const* functionType =
            WasmEdge_ImportTypeGetFunctionType(astModule, imports[i]);

        if (!functionType)
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::FUNC_TYPELESS
                            << "): Import function '" << extName
                            << "' has no function type definition";
            return Unexpected(
                "Import function '" + extName +
                "' has no function type definition");
        }

        // Validate return type
        // expectedSig[0] is the return type
        uint32_t returnCount =
            WasmEdge_FunctionTypeGetReturnsLength(functionType);

        if (returnCount != 1)
        {
            JLOG(j.trace())
                << "HookSet(" << hook::log::FUNC_RETURN_COUNT
                << "): Import function '" << extName
                << "' must return exactly 1 value, found " << returnCount;
            return Unexpected(
                "Import function '" + extName +
                "' must return exactly 1 value");
        }

        WasmEdge_ValType actualReturnType;
        WasmEdge_FunctionTypeGetReturns(functionType, &actualReturnType, 1);

        if (actualReturnType != (*expectedSig)[0])
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::FUNC_RETURN_INVALID
                            << "): Import function '" << extName
                            << "' has incorrect return type. Expected "
                            << static_cast<int>((*expectedSig)[0]) << ", found "
                            << static_cast<int>(actualReturnType);
            return Unexpected(
                "Import function '" + extName + "' has incorrect return type");
        }

        // Validate parameter count and types
        // expectedSig[1..N] are the parameter types
        uint32_t expectedParamCount =
            expectedSig->size() > 0 ? expectedSig->size() - 1 : 0;
        uint32_t actualParamCount =
            WasmEdge_FunctionTypeGetParametersLength(functionType);

        if (actualParamCount != expectedParamCount)
        {
            JLOG(j.trace()) << "HookSet(" << hook::log::FUNC_PARAM_INVALID
                            << "): Import function '" << extName << "' has "
                            << actualParamCount << " parameters, expected "
                            << expectedParamCount;
            return Unexpected(
                "Import function '" + extName +
                "' has incorrect parameter count");
        }

        // Validate each parameter type
        if (actualParamCount > 0)
        {
            std::vector<WasmEdge_ValType> actualParams(actualParamCount);
            WasmEdge_FunctionTypeGetParameters(
                functionType, actualParams.data(), actualParamCount);

            for (uint32_t p = 0; p < actualParamCount; p++)
            {
                uint8_t expectedParamType = (*expectedSig)[1 + p];
                if (actualParams[p] != expectedParamType)
                {
                    JLOG(j.trace())
                        << "HookSet(" << hook::log::FUNC_PARAM_INVALID
                        << "): Import function '" << extName << "' parameter "
                        << p << " has incorrect type. Expected "
                        << static_cast<int>(expectedParamType) << ", found "
                        << static_cast<int>(actualParams[p]);
                    return Unexpected(
                        "Import function '" + extName +
                        "' has incorrect parameter types");
                }
            }
        }
    }

    return {};
}

Expected<bool, std::string>
validateWasmHostFunctionsForGas(
    std::vector<uint8_t> const& wasm,
    Rules const& rules,
    beast::Journal const& j)
{
    // Create WasmEdge Loader
    WasmEdge_LoaderContext* loader = WasmEdge_LoaderCreate(NULL);
    if (!loader)
    {
        return Unexpected("Failed to create WasmEdge Loader");
    }

    // Parse WASM binary
    WasmEdge_ASTModuleContext* astModule = NULL;
    WasmEdge_Result res = WasmEdge_LoaderParseFromBuffer(
        loader, &astModule, wasm.data(), wasm.size());

    if (!WasmEdge_ResultOK(res))
    {
        WasmEdge_LoaderDelete(loader);
        const char* msg = WasmEdge_ResultGetMessage(res);
        return Unexpected(
            std::string("Failed to parse WASM: ") +
            (msg ? msg : "unknown error"));
    }

    bool foundCbak = false;

    //
    // check export section
    //
    auto resultExport = validateExportSection(astModule, j);
    if (!resultExport)
    {
        WasmEdge_ASTModuleDelete(astModule);
        WasmEdge_LoaderDelete(loader);
        return Unexpected(resultExport.error());
    }
    foundCbak = resultExport.value();

    //
    // check import section
    //
    if (auto result = validateImportSection(astModule, rules, j); !result)
    {
        WasmEdge_ASTModuleDelete(astModule);
        WasmEdge_LoaderDelete(loader);
        return Unexpected(result.error());
    }

    // Cleanup
    WasmEdge_ASTModuleDelete(astModule);
    WasmEdge_LoaderDelete(loader);

    return foundCbak;
}

}  // namespace hook
