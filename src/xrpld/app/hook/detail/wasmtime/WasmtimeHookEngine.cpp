#include <xrpld/app/hook/HookHostTypes.h>
#include <xrpld/app/hook/HookWasmEngine.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/wasmtime/HookWasmMemoryExport.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <wasmtime.h>

namespace hook {
namespace {

constexpr std::size_t maxHookHostParameters = 12;
constexpr std::uint64_t testExecutionFuel = 100'000'000'000ULL;

using EnginePtr = std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>;
using LinkerPtr =
    std::unique_ptr<wasmtime_linker_t, decltype(&wasmtime_linker_delete)>;
using ModulePtr =
    std::unique_ptr<wasmtime_module_t, decltype(&wasmtime_module_delete)>;
using StorePtr =
    std::unique_ptr<wasmtime_store_t, decltype(&wasmtime_store_delete)>;

std::string
takeError(wasmtime_error_t* error)
{
    wasm_name_t message;
    wasmtime_error_message(error, &message);
    std::string result{message.data, message.size};
    wasm_byte_vec_delete(&message);
    wasmtime_error_delete(error);
    return result;
}

std::string
takeTrap(wasm_trap_t* trap)
{
    wasm_name_t message;
    wasm_trap_message(trap, &message);
    std::string result{message.data, message.size};
    wasm_byte_vec_delete(&message);
    wasm_trap_delete(trap);
    return result;
}

wasm_trap_t*
callbackTrap(char const* message) noexcept
{
    return wasmtime_trap_new(message, std::strlen(message));
}

bool
matches(HookHostValueKind expected, wasmtime_valkind_t actual) noexcept
{
    return (expected == HookHostValueKind::i32 && actual == WASMTIME_I32) ||
        (expected == HookHostValueKind::i64 && actual == WASMTIME_I64);
}

wasm_valkind_t
wasmType(HookHostValueKind kind) noexcept
{
    return kind == HookHostValueKind::i32 ? WASM_I32 : WASM_I64;
}

std::optional<HookHostValueKind>
hostType(wasm_valkind_t kind) noexcept
{
    if (kind == WASM_I32)
        return HookHostValueKind::i32;
    if (kind == WASM_I64)
        return HookHostValueKind::i64;
    return std::nullopt;
}

EnginePtr
makeDeterministicTestEngine()
{
    auto* configuration = wasm_config_new();
    if (!configuration)
        return {nullptr, &wasm_engine_delete};
    wasmtime_config_consume_fuel_set(configuration, true);
    wasmtime_config_cranelift_nan_canonicalization_set(configuration, true);
    wasmtime_config_wasm_threads_set(configuration, false);
    wasmtime_config_wasm_relaxed_simd_set(configuration, false);
    wasmtime_config_wasm_memory64_set(configuration, false);
    wasmtime_config_wasm_multi_memory_set(configuration, false);
    wasmtime_config_wasm_tail_call_set(configuration, false);
    return {wasm_engine_new_with_config(configuration), &wasm_engine_delete};
}

wasm_functype_t*
functionType(HookHostFunctionDescriptor const& descriptor)
{
    wasm_valtype_vec_t parameters;
    wasm_valtype_vec_new_uninitialized(
        &parameters, descriptor.parameters.size());
    for (std::size_t index = 0; index < descriptor.parameters.size(); ++index)
        parameters.data[index] =
            wasm_valtype_new(wasmType(descriptor.parameters[index]));

    wasm_valtype_vec_t results;
    wasm_valtype_vec_new_uninitialized(&results, 1);
    results.data[0] = wasm_valtype_new(wasmType(descriptor.result));
    return wasm_functype_new(&parameters, &results);
}

struct ExecutionState
{
    HookContext* context;
    bool terminal = false;
};

struct BridgeData
{
    HookHostFunctionDescriptor const* descriptor;
    ExecutionState* execution;
};

wasm_trap_t*
bridgeHostFunction(
    void* environment,
    wasmtime_caller_t* caller,
    wasmtime_val_t const* arguments,
    std::size_t argumentCount,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept
{
    try
    {
        auto const* bridge = static_cast<BridgeData const*>(environment);
        if (!bridge || !bridge->descriptor || !bridge->execution ||
            !bridge->execution->context)
            return callbackTrap("missing Xahau C Hook callback context");
        auto const& descriptor = *bridge->descriptor;
        if (argumentCount != descriptor.parameters.size() ||
            (argumentCount != 0 && !arguments) || resultCount != 1 || !results)
            return callbackTrap("invalid Xahau C Hook callback shape");
        if (argumentCount > maxHookHostParameters)
            return callbackTrap("Xahau C Hook callback has too many arguments");
        for (std::size_t index = 0; index < argumentCount; ++index)
            if (!matches(descriptor.parameters[index], arguments[index].kind))
                return callbackTrap("invalid Xahau C Hook callback type");

        std::array<HookHostValue, maxHookHostParameters> neutralInputs{};
        for (std::size_t index = 0; index < argumentCount; ++index)
            neutralInputs[index] =
                descriptor.parameters[index] == HookHostValueKind::i32
                ? HookHostValue::i32(
                      static_cast<std::uint32_t>(arguments[index].of.i32))
                : HookHostValue::i64(
                      static_cast<std::uint64_t>(arguments[index].of.i64));

        HookGuestMemory memory{nullptr, 0};
        wasmtime_extern_t memoryExport;
        if (wasmtime_caller_export_get(caller, "memory", 6, &memoryExport) &&
            memoryExport.kind == WASMTIME_EXTERN_MEMORY)
        {
            auto* context = wasmtime_caller_context(caller);
            memory = HookGuestMemory{
                wasmtime_memory_data(context, &memoryExport.of.memory),
                wasmtime_memory_data_size(context, &memoryExport.of.memory)};
        }

        HookHostValue neutralOutput{};
        auto const status = descriptor.function(
            bridge->execution->context,
            memory,
            neutralInputs.data(),
            argumentCount,
            &neutralOutput,
            1);
        if (status == HookHostCallStatus::terminate)
        {
            bridge->execution->terminal = true;
            return callbackTrap("Xahau C Hook terminal");
        }
        if (status == HookHostCallStatus::trap)
            return callbackTrap("Xahau C Hook host fault");

        if (descriptor.result == HookHostValueKind::i32)
            results[0] = wasmtime_val_t{
                .kind = WASMTIME_I32,
                .of = {
                    .i32 = std::bit_cast<std::int32_t>(neutralOutput.asI32())}};
        else
            results[0] = wasmtime_val_t{
                .kind = WASMTIME_I64,
                .of = {
                    .i64 = std::bit_cast<std::int64_t>(neutralOutput.asI64())}};
        return nullptr;
    }
    catch (...)
    {
        return callbackTrap("uncaught Xahau C Hook host exception");
    }
}

bool
defineHostFunctions(
    wasmtime_linker_t* linker,
    std::vector<BridgeData>& bridgeData,
    ExecutionState& execution,
    std::string& error)
{
    auto const& catalogue = hookHostFunctionCatalogue();
    bridgeData.reserve(catalogue.size());
    for (auto const& descriptor : catalogue)
    {
        bridgeData.push_back({&descriptor, &execution});
        auto* type = functionType(descriptor);
        if (!type)
        {
            error = std::string{"could not create C Hook import type for "} +
                descriptor.name;
            return false;
        }
        auto* defineError = wasmtime_linker_define_func(
            linker,
            "env",
            3,
            descriptor.name,
            std::strlen(descriptor.name),
            type,
            &bridgeHostFunction,
            &bridgeData.back(),
            nullptr);
        wasm_functype_delete(type);
        if (defineError)
        {
            error = takeError(defineError);
            return false;
        }
    }
    return true;
}

bool
defineLegacyTableAndMemory(
    wasmtime_linker_t* linker,
    wasmtime_context_t* context,
    std::string& error)
{
    wasm_limits_t tableLimits{10, 20};
    auto* tableType =
        wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &tableLimits);
    if (!tableType)
    {
        error = "could not create C Hook table type";
        return false;
    }
    wasmtime_val_t initial;
    initial.kind = WASMTIME_FUNCREF;
    wasmtime_funcref_set_null(&initial.of.funcref);
    wasmtime_table_t table;
    auto* tableError = wasmtime_table_new(context, tableType, &initial, &table);
    wasm_tabletype_delete(tableType);
    if (tableError)
    {
        error = takeError(tableError);
        return false;
    }
    wasmtime_extern_t tableExport;
    tableExport.kind = WASMTIME_EXTERN_TABLE;
    tableExport.of.table = table;
    if (auto* defineError = wasmtime_linker_define(
            linker, context, "env", 3, "table", 5, &tableExport))
    {
        error = takeError(defineError);
        return false;
    }

    wasm_memorytype_t* memoryType = nullptr;
    if (auto* typeError =
            wasmtime_memorytype_new(1, true, 1, false, false, 0, &memoryType))
    {
        error = takeError(typeError);
        return false;
    }
    wasmtime_memory_t memory;
    auto* memoryError = wasmtime_memory_new(context, memoryType, &memory);
    wasm_memorytype_delete(memoryType);
    if (memoryError)
    {
        error = takeError(memoryError);
        return false;
    }
    wasmtime_extern_t memoryExport;
    memoryExport.kind = WASMTIME_EXTERN_MEMORY;
    memoryExport.of.memory = memory;
    if (auto* defineError = wasmtime_linker_define(
            linker, context, "env", 3, "memory", 6, &memoryExport))
    {
        error = takeError(defineError);
        return false;
    }
    return true;
}

class WasmtimeHookEngine : public HookWasmEngine
{
public:
    WasmtimeHookEngine() : engine_(makeDeterministicTestEngine())
    {
    }

    std::optional<std::string>
    validate(void const* wasm, std::size_t length) override
    {
        if (!engine_)
            return "could not create Wasmtime engine";
        auto normalized = wasmtime::ensureHookMemoryExport(
            std::span{static_cast<std::uint8_t const*>(wasm), length});
        if (normalized.error)
            return *normalized.error;
        wasmtime_module_t* module = nullptr;
        if (auto* error = wasmtime_module_new(
                engine_.get(),
                normalized.bytes.data(),
                normalized.bytes.size(),
                &module))
            return "Wasmtime validation failed: " + takeError(error);
        wasmtime_module_delete(module);
        return {};
    }

    HookWasmExecutionResult
    execute(
        void const* wasm,
        std::size_t length,
        bool callback,
        std::uint32_t wasmParameter,
        HookContext& hookContext,
        beast::Journal const& journal) override
    {
        wasmtime_val_t argument{
            .kind = WASMTIME_I32,
            .of = {.i32 = static_cast<std::int32_t>(wasmParameter)}};
        return invokeExport(
            std::span{static_cast<std::uint8_t const*>(wasm), length},
            callback ? "cbak" : "hook",
            &argument,
            1,
            hookContext,
            journal);
    }

    HookWasmExecutionResult
    invokeExport(
        std::span<std::uint8_t const> wasm,
        std::string_view entryName,
        wasmtime_val_t const* arguments,
        std::size_t argumentCount,
        HookContext& hookContext,
        beast::Journal const& journal)
    {
        if (!engine_)
            return {false, 0, "could not create Wasmtime engine"};
        auto normalized = wasmtime::ensureHookMemoryExport(wasm);
        if (normalized.error)
            return {false, 0, *normalized.error};

        wasmtime_module_t* rawModule = nullptr;
        if (auto* compileError = wasmtime_module_new(
                engine_.get(),
                normalized.bytes.data(),
                normalized.bytes.size(),
                &rawModule))
            return {
                false,
                0,
                "Wasmtime compilation failed: " + takeError(compileError)};
        ModulePtr module{rawModule, &wasmtime_module_delete};

        StorePtr store{
            wasmtime_store_new(engine_.get(), nullptr, nullptr),
            &wasmtime_store_delete};
        if (!store)
            return {false, 0, "could not create Wasmtime Store"};
        auto* context = wasmtime_store_context(store.get());
        if (auto* fuelError =
                wasmtime_context_set_fuel(context, testExecutionFuel))
            return {
                false,
                0,
                "could not set Wasmtime fuel: " + takeError(fuelError)};

        LinkerPtr linker{
            wasmtime_linker_new(engine_.get()), &wasmtime_linker_delete};
        if (!linker)
            return {false, 0, "could not create Wasmtime Linker"};
        ExecutionState execution{&hookContext};
        std::vector<BridgeData> bridgeData;
        std::string error;
        if (!defineHostFunctions(linker.get(), bridgeData, execution, error) ||
            !defineLegacyTableAndMemory(linker.get(), context, error))
            return {false, 0, error};

        JLOG(journal.trace()) << "HookInfo[" << hookContext.result.account
                              << "-" << hookContext.result.otxnAccount
                              << "]: creating Wasmtime C Hook instance";

        wasmtime_instance_t instance;
        wasm_trap_t* instantiateTrap = nullptr;
        auto* instantiateError = wasmtime_linker_instantiate(
            linker.get(), context, module.get(), &instance, &instantiateTrap);
        if (instantiateError || instantiateTrap)
        {
            auto detail = instantiateError ? takeError(instantiateError)
                                           : takeTrap(instantiateTrap);
            if (instantiateError && instantiateTrap)
                wasm_trap_delete(instantiateTrap);
            return {false, 0, "Wasmtime instantiation failed: " + detail};
        }

        wasmtime_extern_t entry;
        if (!wasmtime_instance_export_get(
                context,
                &instance,
                entryName.data(),
                entryName.size(),
                &entry) ||
            entry.kind != WASMTIME_EXTERN_FUNC)
            return {
                false,
                0,
                std::string{"missing C Hook function export: "} +
                    std::string{entryName}};

        wasmtime_val_t result;
        wasm_trap_t* callTrap = nullptr;
        auto* callError = wasmtime_func_call(
            context,
            &entry.of.func,
            arguments,
            argumentCount,
            &result,
            1,
            &callTrap);

        std::uint64_t remainingFuel = 0;
        if (auto* fuelError =
                wasmtime_context_get_fuel(context, &remainingFuel))
        {
            wasmtime_error_delete(fuelError);
            remainingFuel = testExecutionFuel;
        }
        auto const consumedFuel =
            testExecutionFuel - std::min(testExecutionFuel, remainingFuel);

        if (execution.terminal)
        {
            if (callError)
                wasmtime_error_delete(callError);
            if (callTrap)
                wasm_trap_delete(callTrap);
            return {true, consumedFuel, {}};
        }
        if (callError)
        {
            auto detail = takeError(callError);
            if (callTrap)
                wasm_trap_delete(callTrap);
            return {
                false, consumedFuel, "Wasmtime C Hook call failed: " + detail};
        }
        if (callTrap)
            return {
                false,
                consumedFuel,
                "Wasmtime C Hook trapped: " + takeTrap(callTrap)};
        if (result.kind == WASMTIME_I32)
            return {
                true,
                consumedFuel,
                {},
                HookHostValue::i32(
                    std::bit_cast<std::uint32_t>(result.of.i32))};
        if (result.kind == WASMTIME_I64)
            return {
                true,
                consumedFuel,
                {},
                HookHostValue::i64(
                    std::bit_cast<std::uint64_t>(result.of.i64))};
        return {
            false,
            consumedFuel,
            "Wasmtime C Hook returned a non-integer value"};
    }

private:
    EnginePtr engine_;
};

}  // namespace

std::unique_ptr<HookWasmEngine>
makeWasmtimeHookEngine()
{
    return std::make_unique<WasmtimeHookEngine>();
}

std::optional<std::string>
inspectWasmtimeHookModule(
    std::span<std::uint8_t const> wasm,
    HookWasmModuleSurface& surface)
{
    surface = {};
    auto engine = makeDeterministicTestEngine();
    if (!engine)
        return "could not create Wasmtime engine";
    wasmtime_module_t* rawModule = nullptr;
    if (auto* error = wasmtime_module_new(
            engine.get(), wasm.data(), wasm.size(), &rawModule))
        return "Wasmtime inspection compile failed: " + takeError(error);
    ModulePtr module{rawModule, &wasmtime_module_delete};

    wasm_importtype_vec_t imports;
    wasmtime_module_imports(module.get(), &imports);
    for (std::size_t index = 0; index < imports.size; ++index)
    {
        auto const* import = imports.data[index];
        auto const* moduleName = wasm_importtype_module(import);
        auto const* itemName = wasm_importtype_name(import);
        auto const* externalType = wasm_importtype_type(import);
        if (!moduleName || !itemName || !externalType ||
            wasm_externtype_kind(externalType) != WASM_EXTERN_FUNC)
        {
            wasm_importtype_vec_delete(&imports);
            return "raw Hook fixture has a non-function import";
        }
        auto const* type = wasm_externtype_as_functype_const(externalType);
        auto const* parameters = wasm_functype_params(type);
        auto const* results = wasm_functype_results(type);
        if (!parameters || !results || results->size != 1)
        {
            wasm_importtype_vec_delete(&imports);
            return "raw Hook fixture import has an invalid result shape";
        }
        HookWasmModuleSurface::Import item{
            .module = {moduleName->data, moduleName->size},
            .name = {itemName->data, itemName->size}};
        item.parameters.reserve(parameters->size);
        for (std::size_t parameter = 0; parameter < parameters->size;
             ++parameter)
        {
            auto const kind =
                hostType(wasm_valtype_kind(parameters->data[parameter]));
            if (!kind)
            {
                wasm_importtype_vec_delete(&imports);
                return "raw Hook fixture import uses a non-integer parameter";
            }
            item.parameters.push_back(*kind);
        }
        item.result = hostType(wasm_valtype_kind(results->data[0]));
        if (!item.result)
        {
            wasm_importtype_vec_delete(&imports);
            return "raw Hook fixture import uses a non-integer result";
        }
        surface.imports.push_back(std::move(item));
    }
    wasm_importtype_vec_delete(&imports);

    wasm_exporttype_vec_t exports;
    wasmtime_module_exports(module.get(), &exports);
    surface.exports.reserve(exports.size);
    for (std::size_t index = 0; index < exports.size; ++index)
    {
        auto const* name = wasm_exporttype_name(exports.data[index]);
        if (name)
            surface.exports.emplace_back(name->data, name->size);
    }
    wasm_exporttype_vec_delete(&exports);
    return {};
}

HookWasmExecutionResult
invokeWasmtimeHookTestExport(
    std::span<std::uint8_t const> wasm,
    std::string_view exportName,
    HookContext& hookContext,
    beast::Journal const& journal)
{
    WasmtimeHookEngine engine;
    return engine.invokeExport(
        wasm, exportName, nullptr, 0, hookContext, journal);
}

}  // namespace hook
