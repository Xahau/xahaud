#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSRuntimeInternal.h>
#include <xrpl/basics/scope.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <wasmtime.h>

namespace hook {
namespace {

constexpr std::uint32_t maxDiagnosticLength = 4096;

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
        error =
            std::string{"missing QuickJS provider function export: "} + name;
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
    wasmtime_func_t resultPointer;
    wasmtime_func_t resultLength;
    if (!findFunction(
            context, instance, "qjs_get_result_ptr", resultPointer, ignored) ||
        !findFunction(
            context, instance, "qjs_get_result_len", resultLength, ignored))
        return {};

    wasmtime_val_t pointerValue[1];
    wasmtime_val_t lengthValue[1];
    if (!call(context, resultPointer, nullptr, 0, pointerValue, 1, ignored) ||
        !call(context, resultLength, nullptr, 0, lengthValue, 1, ignored))
        return {};

    auto const pointer = static_cast<std::uint32_t>(pointerValue[0].of.i32);
    auto const length = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(lengthValue[0].of.i32), maxDiagnosticLength);
    if (length == 0)
        return {};
    HookGuestMemory view{
        wasmtime_memory_data(context, &memory),
        wasmtime_memory_data_size(context, &memory)};
    auto const bytes = view.read(pointer, length);
    if (!bytes)
        return {};
    return {reinterpret_cast<char const*>(bytes->data()), bytes->size()};
}

}  // namespace

void
executeQuickJSBytecode(
    HookContext& hookCtx,
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool callback,
    std::uint32_t reserved,
    beast::Journal const& journal)
{
    auto fail = [&](std::string_view phase, std::string const& detail) {
        JLOG(journal.warn()) << "HookError[" << hookCtx.result.account << '-'
                             << hookCtx.result.otxnAccount << "]: " << phase
                             << (detail.empty() ? "" : ": ") << detail;
        hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
    };

    try
    {
        std::string error;
        if (!runtime)
        {
            fail("QuickJS runtime profile is not registered", {});
            return;
        }

        quickjs::QuickJSInvocation invocation{
            hookCtx, journal, runtime->profile};
        StorePtr store{
            wasmtime_store_new(runtime->engine.get(), &invocation, nullptr),
            wasmtime_store_delete};
        if (!store)
        {
            fail("QuickJS Store creation failed", {});
            return;
        }
        auto* context = wasmtime_store_context(store.get());
        if (auto* fuelError = wasmtime_context_set_fuel(
                context, runtime->profile.initializationFuel))
        {
            fail(
                "QuickJS initialization fuel setup failed",
                takeError(fuelError));
            return;
        }

        LinkerPtr linker{
            wasmtime_linker_new(runtime->engine.get()), wasmtime_linker_delete};
        if (!linker ||
            !quickjs::defineImports(linker.get(), runtime->hostPolicy, error))
        {
            fail("QuickJS Hook import registration failed", error);
            return;
        }

        wasmtime_instance_t instance;
        wasm_trap_t* trap = nullptr;
        auto* instantiateError = wasmtime_linker_instantiate(
            linker.get(), context, runtime->module.get(), &instance, &trap);
        if (instantiateError || trap)
        {
            error =
                instantiateError ? takeError(instantiateError) : takeTrap(trap);
            if (instantiateError && trap)
                wasm_trap_delete(trap);
            fail("QuickJS provider instantiation failed", error);
            return;
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

        if (!callExport("_initialize", nullptr, 0, nullptr, 0))
        {
            fail("QuickJS reactor initialization failed", error);
            return;
        }
        if (!callExport("qjs_init", nullptr, 0, nullptr, 0))
        {
            fail("QuickJS initialization failed", error);
            return;
        }

        wasmtime_val_t limit[1] = {
            {.kind = WASMTIME_I32,
             .of = {
                 .i32 =
                     static_cast<std::int32_t>(runtime->profile.heapBytes)}}};
        if (!callExport("qjs_set_memory_limit", limit, 1, nullptr, 0))
        {
            fail("QuickJS memory limit setup failed", error);
            return;
        }
        limit[0].of.i32 =
            static_cast<std::int32_t>(runtime->profile.stackBytes);
        if (!callExport("qjs_set_max_stack_size", limit, 1, nullptr, 0))
        {
            fail("QuickJS stack limit setup failed", error);
            return;
        }

        if (auto* fuelError = wasmtime_context_set_fuel(
                context, runtime->profile.invocationFuel))
        {
            fail("QuickJS invocation fuel setup failed", takeError(fuelError));
            return;
        }
        ripple::scope_exit recordFuel{[&] {
            std::uint64_t remaining = 0;
            if (auto* fuelError =
                    wasmtime_context_get_fuel(context, &remaining))
            {
                wasmtime_error_delete(fuelError);
                return;
            }
            hookCtx.result.instructionCount = runtime->profile.invocationFuel -
                std::min(runtime->profile.invocationFuel, remaining);
        }};

        auto const bytecodeLength = bytecode.size();
        if (bytecodeLength == 0 ||
            bytecodeLength > static_cast<std::size_t>(
                                 std::numeric_limits<std::int32_t>::max()))
        {
            fail("QuickJS Hook bytecode is invalid", {});
            return;
        }

        wasmtime_val_t mallocArgument[1] = {
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(bytecodeLength)}}};
        wasmtime_val_t mallocResult[1];
        if (!callExport("malloc", mallocArgument, 1, mallocResult, 1))
        {
            fail("QuickJS bytecode allocation failed", error);
            return;
        }
        auto const bytecodePointer =
            static_cast<std::uint32_t>(mallocResult[0].of.i32);
        if (bytecodePointer == 0)
        {
            fail("QuickJS bytecode allocation failed", {});
            return;
        }

        wasmtime_extern_t memoryExport;
        if (!wasmtime_instance_export_get(
                context, &instance, "memory", 6, &memoryExport) ||
            memoryExport.kind != WASMTIME_EXTERN_MEMORY)
        {
            fail("QuickJS provider exports no memory", {});
            return;
        }
        HookGuestMemory memory{
            wasmtime_memory_data(context, &memoryExport.of.memory),
            wasmtime_memory_data_size(context, &memoryExport.of.memory)};
        auto destination = memory.write(
            bytecodePointer, static_cast<std::uint32_t>(bytecodeLength));
        if (!destination)
        {
            fail("QuickJS bytecode copy was out of bounds", {});
            return;
        }
        std::copy_n(bytecode.data(), bytecodeLength, destination->begin());

        wasmtime_val_t entryArguments[3] = {
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(bytecodePointer)}},
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(bytecodeLength)}},
            {.kind = WASMTIME_I32,
             .of = {.i32 = static_cast<std::int32_t>(reserved)}}};
        wasmtime_val_t entryResult[1];
        if (!callExport(
                callback ? "qjs_cbak" : "qjs_hook",
                entryArguments,
                3,
                entryResult,
                1))
        {
            if (invocation.terminal)
                return;
            fail("QuickJS entry invocation trapped", error);
            return;
        }

        if (entryResult[0].of.i32 != 0)
        {
            auto const detail =
                readDiagnostic(context, instance, memoryExport.of.memory);
            hookCtx.result.exitReason = detail;
            fail("QuickJS entry invocation returned an error", detail);
            return;
        }

        fail("JavaScript Hook returned without a terminal", {});
    }
    catch (std::exception const& exception)
    {
        fail("uncaught QuickJS runtime exception", exception.what());
    }
    catch (...)
    {
        fail("uncaught QuickJS runtime exception", {});
    }
}

}  // namespace hook
