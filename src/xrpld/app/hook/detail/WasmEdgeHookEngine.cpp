#include <xrpld/app/hook/HookHostTypes.h>
#include <xrpld/app/hook/HookWasmEngine.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/basics/contract.h>
#include <array>
#include <bit>
#include <optional>
#include <string>
#include <vector>
#include <wasmedge/wasmedge.h>

namespace hook {
namespace {

constexpr std::size_t maxHookHostParameters = 12;
constexpr std::uint64_t wasmPageSize = 65536;

struct WasmEdgeVM
{
    WasmEdge_ConfigureContext* configuration = nullptr;
    WasmEdge_VMContext* vm = nullptr;

    WasmEdgeVM()
    {
        configuration = WasmEdge_ConfigureCreate();
        if (!configuration)
            return;
        WasmEdge_ConfigureStatisticsSetInstructionCounting(
            configuration, true);
        vm = WasmEdge_VMCreate(configuration, nullptr);
    }

    ~WasmEdgeVM()
    {
        if (vm)
            WasmEdge_VMDelete(vm);
        if (configuration)
            WasmEdge_ConfigureDelete(configuration);
    }

    bool
    sane() const noexcept
    {
        return vm && configuration;
    }
};

std::optional<std::string>
wasmError(std::string const& prefix, WasmEdge_Result result)
{
    if (WasmEdge_ResultOK(result))
        return {};
    auto const* message = WasmEdge_ResultGetMessage(result);
    return prefix + ": " + (message ? message : "unknown error");
}

struct BridgeData
{
    HookHostFunctionDescriptor const* descriptor;
    HookContext* context;
};

bool
legacyWrite(
    void* context,
    std::uint32_t offset,
    std::span<std::uint8_t const> bytes) noexcept
{
    auto* memory = static_cast<WasmEdge_MemoryInstanceContext*>(context);
    return memory && WasmEdge_ResultOK(WasmEdge_MemoryInstanceSetData(
                         memory, bytes.data(), offset, bytes.size()));
}

WasmEdge_Result
bridgeHostFunction(
    void* data,
    WasmEdge_CallingFrameContext const* frame,
    WasmEdge_Value const* inputs,
    WasmEdge_Value* outputs)
{
    auto* bridge = static_cast<BridgeData*>(data);
    auto const& descriptor = *bridge->descriptor;

    auto* memoryContext =
        WasmEdge_CallingFrameGetMemoryInstance(frame, 0);
    std::uint8_t* memoryData = nullptr;
    std::size_t memorySize = 0;
    if (memoryContext)
    {
        memoryData = WasmEdge_MemoryInstanceGetPointer(memoryContext, 0, 0);
        memorySize = WasmEdge_MemoryInstanceGetPageSize(memoryContext) *
            wasmPageSize;
    }
    HookGuestMemory memory{
        memoryData, memorySize, memoryContext, &legacyWrite};

    XRPL_ASSERT(
        descriptor.parameters.size() <= maxHookHostParameters,
        "WasmEdge Hook host parameter count");
    std::array<HookHostValue, maxHookHostParameters> neutralInputs{};
    for (std::size_t i = 0; i < descriptor.parameters.size(); ++i)
    {
        neutralInputs[i] = descriptor.parameters[i] == HookHostValueKind::i32
            ? HookHostValue::i32(
                  static_cast<std::uint32_t>(WasmEdge_ValueGetI32(inputs[i])))
            : HookHostValue::i64(
                  static_cast<std::uint64_t>(WasmEdge_ValueGetI64(inputs[i])));
    }

    HookHostValue neutralOutput{};
    auto const status = descriptor.function(
        bridge->context,
        memory,
        neutralInputs.data(),
        descriptor.parameters.size(),
        &neutralOutput,
        1);

    if (status != HookHostCallStatus::success)
        return WasmEdge_Result_Terminate;

    outputs[0] = descriptor.result == HookHostValueKind::i32
        ? WasmEdge_ValueGenI32(
              std::bit_cast<std::int32_t>(neutralOutput.asI32()))
        : WasmEdge_ValueGenI64(
              std::bit_cast<std::int64_t>(neutralOutput.asI64()));
    return WasmEdge_Result_Success;
}

WasmEdge_ValType
wasmEdgeType(HookHostValueKind kind) noexcept
{
    return kind == HookHostValueKind::i32 ? WasmEdge_ValType_I32
                                          : WasmEdge_ValType_I64;
}

class WasmEdgeHookEngine : public HookWasmEngine
{
public:
    std::optional<std::string>
    validate(void const* wasm, std::size_t length) override
    {
        WasmEdgeVM engine;
        if (!engine.sane())
            return "Could not create WASMEDGE instance";

        auto result = WasmEdge_VMLoadWasmFromBuffer(
            engine.vm,
            static_cast<std::uint8_t const*>(wasm),
            length);
        if (auto error = wasmError("VMLoadWasmFromBuffer failed", result))
            return error;

        result = WasmEdge_VMValidate(engine.vm);
        return wasmError("VMValidate failed", result);
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
        static auto const envName = WasmEdge_StringCreateByCString("env");
        static auto const tableName = WasmEdge_StringCreateByCString("table");
        static auto const memoryName =
            WasmEdge_StringCreateByCString("memory");
        static auto const hookName = WasmEdge_StringCreateByCString("hook");
        static auto const callbackName =
            WasmEdge_StringCreateByCString("cbak");
        static auto* tableType = WasmEdge_TableTypeCreate(
            WasmEdge_RefType_FuncRef,
            {.HasMax = true, .Shared = false, .Min = 10, .Max = 20});
        static auto* memoryType = WasmEdge_MemoryTypeCreate(
            {.HasMax = true, .Shared = false, .Min = 1, .Max = 1});

        auto const& catalogue = hookHostFunctionCatalogue();
        std::vector<BridgeData> bridgeData;
        bridgeData.reserve(catalogue.size());

        auto* imports = WasmEdge_ModuleInstanceCreate(envName);
        for (auto const& descriptor : catalogue)
        {
            bridgeData.push_back({&descriptor, &hookContext});

            std::vector<WasmEdge_ValType> parameters;
            parameters.reserve(descriptor.parameters.size());
            for (auto const kind : descriptor.parameters)
                parameters.push_back(wasmEdgeType(kind));
            auto resultType = wasmEdgeType(descriptor.result);
            auto* functionType = WasmEdge_FunctionTypeCreate(
                parameters.data(), parameters.size(), &resultType, 1);
            auto* function = WasmEdge_FunctionInstanceCreate(
                functionType,
                &bridgeHostFunction,
                &bridgeData.back(),
                0);
            WasmEdge_FunctionTypeDelete(functionType);

            auto name = WasmEdge_StringCreateByCString(descriptor.name);
            WasmEdge_ModuleInstanceAddFunction(imports, name, function);
            WasmEdge_StringDelete(name);
        }

        WasmEdge_ModuleInstanceAddTable(
            imports,
            tableName,
            WasmEdge_TableInstanceCreate(tableType));
        WasmEdge_ModuleInstanceAddMemory(
            imports,
            memoryName,
            WasmEdge_MemoryInstanceCreate(memoryType));

        JLOG(journal.trace()) << "HookInfo[" << hookContext.result.account
                              << "-" << hookContext.result.otxnAccount
                              << "]: creating wasm instance";
        WasmEdge_LogOff();

        WasmEdgeVM engine;
        if (!engine.sane())
        {
            WasmEdge_ModuleInstanceDelete(imports);
            return {false, 0, "Could not create WASMEDGE instance"};
        }

        auto result = WasmEdge_VMRegisterModuleFromImport(engine.vm, imports);
        if (auto error = wasmError("Import phase failed", result))
        {
            WasmEdge_ModuleInstanceDelete(imports);
            return {false, 0, error};
        }

        WasmEdge_Value parameters[1] = {
            WasmEdge_ValueGenI32(static_cast<std::int32_t>(wasmParameter))};
        WasmEdge_Value results[1];
        result = WasmEdge_VMRunWasmFromBuffer(
            engine.vm,
            static_cast<std::uint8_t const*>(wasm),
            length,
            callback ? callbackName : hookName,
            parameters,
            1,
            results,
            1);

        HookWasmExecutionResult execution;
        if (auto error = wasmError("WASM VM error", result))
            execution = {false, 0, error};
        else
        {
            auto* statistics = WasmEdge_VMGetStatisticsContext(engine.vm);
            execution = {
                true, WasmEdge_StatisticsGetInstrCount(statistics), {}};
        }

        WasmEdge_ModuleInstanceDelete(imports);
        return execution;
    }
};

}  // namespace

std::unique_ptr<HookWasmEngine>
makeWasmEdgeHookEngine()
{
    return std::make_unique<WasmEdgeHookEngine>();
}

}  // namespace hook
