#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSLinker.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSRuntimeInternal.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <wasmtime.h>

namespace hook {
namespace {

using LinkerPtr =
    std::unique_ptr<wasmtime_linker_t, decltype(&wasmtime_linker_delete)>;
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

bool
findFunction(
    wasmtime_context_t* context,
    wasmtime_instance_t const& instance,
    char const* name,
    wasmtime_func_t& result,
    std::string& error)
{
    wasmtime_extern_t item;
    if (!wasmtime_instance_export_get(
            context, &instance, name, std::strlen(name), &item) ||
        item.kind != WASMTIME_EXTERN_FUNC)
    {
        error = std::string{"missing QuickJS provider function export: "} +
            name;
        return false;
    }
    result = item.of.func;
    return true;
}

bool
call(
    wasmtime_context_t* context,
    wasmtime_func_t const& function,
    wasmtime_val_t const* arguments,
    std::size_t argumentCount,
    wasmtime_val_t* results,
    std::size_t resultCount,
    std::string& error)
{
    wasm_trap_t* trap = nullptr;
    auto* callError = wasmtime_func_call(
        context,
        &function,
        arguments,
        argumentCount,
        results,
        resultCount,
        &trap);
    if (callError)
    {
        error = takeError(callError);
        if (trap)
            wasm_trap_delete(trap);
        return false;
    }
    if (trap)
    {
        error = takeTrap(trap);
        return false;
    }
    return true;
}

std::string
readDiagnostic(
    wasmtime_context_t* context,
    wasmtime_instance_t const& instance,
    wasmtime_memory_t const& memory)
{
    std::string ignored;
    wasmtime_func_t pointerFunction;
    wasmtime_func_t lengthFunction;
    if (!findFunction(
            context,
            instance,
            "qjs_get_result_ptr",
            pointerFunction,
            ignored) ||
        !findFunction(
            context,
            instance,
            "qjs_get_result_len",
            lengthFunction,
            ignored))
        return {};

    wasmtime_val_t pointerResult[1];
    wasmtime_val_t lengthResult[1];
    if (!call(context, pointerFunction, nullptr, 0, pointerResult, 1, ignored) ||
        !call(context, lengthFunction, nullptr, 0, lengthResult, 1, ignored))
        return {};

    auto const pointer = static_cast<std::uint32_t>(pointerResult[0].of.i32);
    auto const length = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(lengthResult[0].of.i32), 4096);
    HookGuestMemory view{
        wasmtime_memory_data(context, &memory),
        wasmtime_memory_data_size(context, &memory)};
    auto const bytes = view.read(pointer, length);
    if (!bytes)
        return {};
    return {reinterpret_cast<char const*>(bytes->data()), bytes->size()};
}

}  // namespace

std::optional<std::string>
validateQuickJSBytecode(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool& hasCallback)
{
    hasCallback = false;
    if (!runtime)
        return "QuickJS runtime profile is not registered";
    if (bytecode.empty() ||
        bytecode.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        return "QuickJS Hook bytecode is empty or too large";

    std::string error;
    StorePtr store{
        wasmtime_store_new(runtime->engine.get(), nullptr, nullptr),
        wasmtime_store_delete};
    if (!store)
        return "QuickJS validation Store creation failed";
    auto* context = wasmtime_store_context(store.get());
    if (auto* fuelError = wasmtime_context_set_fuel(
            context, runtime->profile.initializationFuel))
        return takeError(fuelError);

    LinkerPtr linker{
        wasmtime_linker_new(runtime->engine.get()), wasmtime_linker_delete};
    if (!linker ||
        !quickjs::defineImports(linker.get(), runtime->hostPolicy, error))
        return error.empty() ? "QuickJS validation linker creation failed"
                             : error;

    wasmtime_instance_t instance;
    wasm_trap_t* trap = nullptr;
    auto* instantiateError = wasmtime_linker_instantiate(
        linker.get(), context, runtime->module.get(), &instance, &trap);
    if (instantiateError || trap)
    {
        error = instantiateError ? takeError(instantiateError) : takeTrap(trap);
        if (instantiateError && trap)
            wasm_trap_delete(trap);
        return error;
    }

    auto callExport = [&](char const* name,
                          wasmtime_val_t const* arguments,
                          std::size_t argumentCount,
                          wasmtime_val_t* results,
                          std::size_t resultCount) {
        wasmtime_func_t function;
        return findFunction(context, instance, name, function, error) &&
            call(context,
                 function,
                 arguments,
                 argumentCount,
                 results,
                 resultCount,
                 error);
    };

    if (!callExport("_initialize", nullptr, 0, nullptr, 0) ||
        !callExport("qjs_init", nullptr, 0, nullptr, 0))
        return error;

    wasmtime_val_t limit[1] = {
        {.kind = WASMTIME_I32,
         .of = {
             .i32 = static_cast<std::int32_t>(runtime->profile.heapBytes)}}};
    if (!callExport("qjs_set_memory_limit", limit, 1, nullptr, 0))
        return error;
    limit[0].of.i32 = static_cast<std::int32_t>(runtime->profile.stackBytes);
    if (!callExport("qjs_set_max_stack_size", limit, 1, nullptr, 0))
        return error;
    if (auto* fuelError = wasmtime_context_set_fuel(
            context, runtime->profile.invocationFuel))
        return takeError(fuelError);

    wasmtime_val_t allocateArgument[1] = {
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(bytecode.size())}}};
    wasmtime_val_t allocateResult[1];
    if (!callExport("malloc", allocateArgument, 1, allocateResult, 1))
        return error;
    auto const pointer =
        static_cast<std::uint32_t>(allocateResult[0].of.i32);

    wasmtime_extern_t memoryExport;
    if (!wasmtime_instance_export_get(
            context, &instance, "memory", 6, &memoryExport) ||
        memoryExport.kind != WASMTIME_EXTERN_MEMORY)
        return "QuickJS provider exports no memory";
    HookGuestMemory memory{
        wasmtime_memory_data(context, &memoryExport.of.memory),
        wasmtime_memory_data_size(context, &memoryExport.of.memory)};
    auto destination = memory.write(
        pointer, static_cast<std::uint32_t>(bytecode.size()));
    if (!destination)
        return "QuickJS validation bytecode copy was out of bounds";
    std::copy(bytecode.begin(), bytecode.end(), destination->begin());

    wasmtime_val_t arguments[2] = {
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(pointer)}},
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(bytecode.size())}}};
    wasmtime_val_t result[1];
    if (!callExport(
            "qjs_validate_hook_module", arguments, 2, result, 1))
        return error;
    auto const flags = result[0].of.i32;
    if (flags != 1 && flags != 3)
    {
        auto detail =
            readDiagnostic(context, instance, memoryExport.of.memory);
        return detail.empty() ? "QuickJS Hook bytecode validation failed"
                              : detail;
    }
    hasCallback = (flags & 2) != 0;
    return std::nullopt;
}

}  // namespace hook
