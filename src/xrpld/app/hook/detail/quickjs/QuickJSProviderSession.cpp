#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSProviderSession.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace hook::quickjs {
namespace {

constexpr std::uint32_t maxDiagnosticLength = 4096;

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
    std::string& detail)
{
    wasmtime_extern_t item;
    if (!wasmtime_instance_export_get(
            context, &instance, name, std::strlen(name), &item) ||
        item.kind != WASMTIME_EXTERN_FUNC)
    {
        detail =
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
    std::string& detail)
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
        detail = takeError(callError);
        if (trap)
            wasm_trap_delete(trap);
        return false;
    }
    if (trap)
    {
        detail = takeTrap(trap);
        return false;
    }
    return true;
}

}  // namespace

std::string_view
providerStageName(ProviderStage stage) noexcept
{
    switch (stage)
    {
        case ProviderStage::none:
            return {};
        case ProviderStage::storeCreation:
            return "QuickJS Store creation failed";
        case ProviderStage::initializationFuel:
            return "QuickJS initialization fuel setup failed";
        case ProviderStage::importRegistration:
            return "QuickJS Hook import registration failed";
        case ProviderStage::instantiation:
            return "QuickJS provider instantiation failed";
        case ProviderStage::memoryExport:
            return "QuickJS provider exports no memory";
        case ProviderStage::reactorInitialization:
            return "QuickJS reactor initialization failed";
        case ProviderStage::quickJSInitialization:
            return "QuickJS initialization failed";
        case ProviderStage::memoryLimit:
            return "QuickJS memory limit setup failed";
        case ProviderStage::stackLimit:
            return "QuickJS stack limit setup failed";
        case ProviderStage::invocationFuel:
            return "QuickJS invocation fuel setup failed";
        case ProviderStage::bytecodeInput:
            return "QuickJS Hook bytecode is invalid";
        case ProviderStage::bytecodeAllocation:
            return "QuickJS bytecode allocation failed";
        case ProviderStage::bytecodeCopy:
            return "QuickJS bytecode copy was out of bounds";
    }
    return {};
}

ProviderSession::ProviderSession(
    QuickJSProviderRuntime const& runtime,
    StorePtr&& store,
    LinkerPtr&& linker,
    wasmtime_instance_t instance,
    wasmtime_memory_t memory) noexcept
    : runtime_(runtime)
    , store_(std::move(store))
    , linker_(std::move(linker))
    , context_(wasmtime_store_context(store_.get()))
    , instance_(instance)
    , memory_(memory)
{
}

std::unique_ptr<ProviderSession>
ProviderSession::create(
    QuickJSProviderRuntime const& runtime,
    void* storeData,
    ProviderStage& stage,
    std::string& detail)
{
    stage = ProviderStage::none;
    detail.clear();
    StorePtr store{
        wasmtime_store_new(runtime.engine.get(), storeData, nullptr),
        wasmtime_store_delete};
    if (!store)
    {
        stage = ProviderStage::storeCreation;
        return {};
    }
    auto* context = wasmtime_store_context(store.get());
    if (auto* fuelError = wasmtime_context_set_fuel(
            context, runtime.profile.initializationFuel))
    {
        stage = ProviderStage::initializationFuel;
        detail = takeError(fuelError);
        return {};
    }

    LinkerPtr linker{
        wasmtime_linker_new(runtime.engine.get()), wasmtime_linker_delete};
    if (!linker || !defineImports(linker.get(), runtime.hostPolicy, detail))
    {
        stage = ProviderStage::importRegistration;
        return {};
    }

    wasmtime_instance_t instance;
    wasm_trap_t* trap = nullptr;
    auto* instantiateError = wasmtime_linker_instantiate(
        linker.get(), context, runtime.module.get(), &instance, &trap);
    if (instantiateError || trap)
    {
        stage = ProviderStage::instantiation;
        detail =
            instantiateError ? takeError(instantiateError) : takeTrap(trap);
        if (instantiateError && trap)
            wasm_trap_delete(trap);
        return {};
    }

    wasmtime_extern_t memoryExport;
    if (!wasmtime_instance_export_get(
            context, &instance, "memory", 6, &memoryExport) ||
        memoryExport.kind != WASMTIME_EXTERN_MEMORY)
    {
        stage = ProviderStage::memoryExport;
        return {};
    }

    return std::unique_ptr<ProviderSession>{new ProviderSession{
        runtime,
        std::move(store),
        std::move(linker),
        instance,
        memoryExport.of.memory}};
}

bool
ProviderSession::callExport(
    char const* name,
    wasmtime_val_t const* arguments,
    std::size_t argumentCount,
    wasmtime_val_t* results,
    std::size_t resultCount,
    std::string& detail)
{
    wasmtime_func_t function;
    return findFunction(context_, instance_, name, function, detail) &&
        call(context_,
             function,
             arguments,
             argumentCount,
             results,
             resultCount,
             detail);
}

bool
ProviderSession::initialize(ProviderStage& stage, std::string& detail)
{
    stage = ProviderStage::none;
    detail.clear();
    if (!callExport("_initialize", nullptr, 0, nullptr, 0, detail))
    {
        stage = ProviderStage::reactorInitialization;
        return false;
    }
    if (!callExport("qjs_init", nullptr, 0, nullptr, 0, detail))
    {
        stage = ProviderStage::quickJSInitialization;
        return false;
    }

    wasmtime_val_t limit[1] = {
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(runtime_.profile.heapBytes)}}};
    if (!callExport("qjs_set_memory_limit", limit, 1, nullptr, 0, detail))
    {
        stage = ProviderStage::memoryLimit;
        return false;
    }
    limit[0].of.i32 = static_cast<std::int32_t>(runtime_.profile.stackBytes);
    if (!callExport("qjs_set_max_stack_size", limit, 1, nullptr, 0, detail))
    {
        stage = ProviderStage::stackLimit;
        return false;
    }
    return true;
}

bool
ProviderSession::resetInvocationFuel(ProviderStage& stage, std::string& detail)
{
    stage = ProviderStage::none;
    detail.clear();
    if (auto* fuelError = wasmtime_context_set_fuel(
            context_, runtime_.profile.invocationFuel))
    {
        stage = ProviderStage::invocationFuel;
        detail = takeError(fuelError);
        return false;
    }
    return true;
}

std::optional<std::uint32_t>
ProviderSession::allocateAndCopy(
    std::span<std::uint8_t const> bytes,
    ProviderStage& stage,
    std::string& detail)
{
    stage = ProviderStage::none;
    detail.clear();
    if (bytes.empty() ||
        bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
    {
        stage = ProviderStage::bytecodeInput;
        return std::nullopt;
    }

    wasmtime_val_t argument[1] = {
        {.kind = WASMTIME_I32,
         .of = {.i32 = static_cast<std::int32_t>(bytes.size())}}};
    wasmtime_val_t result[1];
    if (!callExport("malloc", argument, 1, result, 1, detail))
    {
        stage = ProviderStage::bytecodeAllocation;
        return std::nullopt;
    }
    auto const pointer = static_cast<std::uint32_t>(result[0].of.i32);
    if (pointer == 0)
    {
        stage = ProviderStage::bytecodeAllocation;
        return std::nullopt;
    }

    // malloc may grow the Wasm memory. Reacquire the native view only now.
    HookGuestMemory memory{
        wasmtime_memory_data(context_, &memory_),
        wasmtime_memory_data_size(context_, &memory_)};
    auto destination =
        memory.write(pointer, static_cast<std::uint32_t>(bytes.size()));
    if (!destination)
    {
        stage = ProviderStage::bytecodeCopy;
        return std::nullopt;
    }
    std::copy(bytes.begin(), bytes.end(), destination->begin());
    return pointer;
}

std::string
ProviderSession::readDiagnostic() noexcept
{
    try
    {
        std::string ignored;
        wasmtime_val_t pointerResult[1];
        wasmtime_val_t lengthResult[1];
        if (!callExport(
                "qjs_get_result_ptr", nullptr, 0, pointerResult, 1, ignored) ||
            !callExport(
                "qjs_get_result_len", nullptr, 0, lengthResult, 1, ignored))
            return {};

        auto const pointer =
            static_cast<std::uint32_t>(pointerResult[0].of.i32);
        auto const length = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(lengthResult[0].of.i32),
            maxDiagnosticLength);
        if (length == 0)
            return {};

        // Either diagnostic export may grow memory. Reacquire after both.
        HookGuestMemory memory{
            wasmtime_memory_data(context_, &memory_),
            wasmtime_memory_data_size(context_, &memory_)};
        auto const bytes = memory.read(pointer, length);
        if (!bytes)
            return {};
        return {reinterpret_cast<char const*>(bytes->data()), bytes->size()};
    }
    catch (...)
    {
        return {};
    }
}

std::optional<std::uint64_t>
ProviderSession::fuelConsumedFrom(std::uint64_t armed) const noexcept
{
    std::uint64_t remaining = 0;
    if (auto* fuelError = wasmtime_context_get_fuel(context_, &remaining))
    {
        wasmtime_error_delete(fuelError);
        return std::nullopt;
    }
    return armed - std::min(armed, remaining);
}

std::optional<std::uint64_t>
ProviderSession::initializationFuelConsumed() const noexcept
{
    return fuelConsumedFrom(runtime_.profile.initializationFuel);
}

std::optional<std::uint64_t>
ProviderSession::invocationFuelConsumed() const noexcept
{
    return fuelConsumedFrom(runtime_.profile.invocationFuel);
}

}  // namespace hook::quickjs
