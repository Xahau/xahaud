#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/WasmEdgeEngine.h>
#include <xrpl/protocol/Feature.h>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include <wasmedge/wasmedge.h>

using namespace ripple;

namespace hook {

namespace {

// ── Type-level helpers ──────────────────────────────────────────────────────

template <typename T>
constexpr WasmValue::Kind
kindOf()
{
    if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, int64_t>)
        return WasmValue::Kind::I64;
    return WasmValue::Kind::I32;
}

template <typename... Ts>
std::vector<WasmValue::Kind>
buildKinds()
{
    return {kindOf<Ts>()...};
}

// ── WasmEdge VM RAII ────────────────────────────────────────────────────────

struct WasmEdgeVM
{
    WasmEdge_ConfigureContext* conf = nullptr;
    WasmEdge_VMContext* ctx = nullptr;

    WasmEdgeVM()
    {
        conf = WasmEdge_ConfigureCreate();
        if (!conf)
            return;
        WasmEdge_ConfigureStatisticsSetInstructionCounting(conf, true);
        ctx = WasmEdge_VMCreate(conf, nullptr);
    }

    bool
    sane() const
    {
        return ctx && conf;
    }

    ~WasmEdgeVM()
    {
        if (ctx)
            WasmEdge_VMDelete(ctx);
        if (conf)
            WasmEdge_ConfigureDelete(conf);
    }
};

static std::optional<std::string>
getWasmError(std::string const& prefix, WasmEdge_Result res)
{
    if (WasmEdge_ResultOK(res))
        return {};
    const char* msg = WasmEdge_ResultGetMessage(res);
    return prefix + ": " + (msg ? msg : "unknown error");
}

// ── Bridge: single WasmEdge callback → HostFunctionFn ──────────────────────

struct BridgeData
{
    HostFunctionDecl const* decl;
    HookContext* ctx;
};

static WasmEdge_Result
bridgeFn(
    void* data_ptr,
    WasmEdge_CallingFrameContext const* frameCtx,
    WasmEdge_Value const* in,
    WasmEdge_Value* out)
{
    auto* bridge = static_cast<BridgeData*>(data_ptr);

    auto* memCtx = WasmEdge_CallingFrameGetMemoryInstance(frameCtx, 0);
    GuestMemory mem{
        WasmEdge_MemoryInstanceGetPointer(memCtx, 0, 0),
        WasmEdge_MemoryInstanceGetPageSize(memCtx) * 65536ULL};

    size_t const paramCount = bridge->decl->params.size();
    std::vector<WasmValue> inVals(paramCount);
    for (size_t i = 0; i < paramCount; ++i)
    {
        if (bridge->decl->params[i] == WasmValue::Kind::I32)
            inVals[i] = WasmValue::i32((uint32_t)WasmEdge_ValueGetI32(in[i]));
        else
            inVals[i] = WasmValue::i64((uint64_t)WasmEdge_ValueGetI64(in[i]));
    }

    WasmValue outVal;
    auto status = bridge->decl->fn(
        bridge->ctx, mem, inVals.data(), paramCount, &outVal, 1);

    if (status == HostCallStatus::Terminate || status == HostCallStatus::Trap)
        return WasmEdge_Result_Terminate;

    if (bridge->decl->result == WasmValue::Kind::I32)
        out[0] = WasmEdge_ValueGenI32((int32_t)outVal.asI32());
    else
        out[0] = WasmEdge_ValueGenI64((int64_t)outVal.asI64());

    return WasmEdge_Result_Success;
}

// ── WasmEdgeEngineImpl ──────────────────────────────────────────────────────

class WasmEdgeEngineImpl : public IWasmEngine
{
public:
    std::optional<std::string>
    validate(void const* wasm, size_t len) override
    {
        WasmEdgeVM vm;
        if (!vm.sane())
            return "Could not create WASMEDGE instance";

        WasmEdge_Result res = WasmEdge_VMLoadWasmFromBuffer(
            vm.ctx, reinterpret_cast<uint8_t const*>(wasm), len);
        if (auto err = getWasmError("VMLoadWasmFromBuffer failed", res))
            return *err;

        res = WasmEdge_VMValidate(vm.ctx);
        if (auto err = getWasmError("VMValidate failed", res))
            return *err;

        return {};
    }

    ExecutionResult
    execute(
        void const* wasm,
        size_t len,
        bool isCallback,
        uint32_t wasmParam,
        HookContext& ctx,
        std::vector<HostFunctionDecl> const& imports,
        ripple::Rules const& rules,
        beast::Journal const& j) override
    {
        static WasmEdge_String exportName =
            WasmEdge_StringCreateByCString("env");
        static WasmEdge_String tableName =
            WasmEdge_StringCreateByCString("table");
        static auto* tableType = WasmEdge_TableTypeCreate(
            WasmEdge_RefType_FuncRef,
            {.HasMax = true, .Shared = false, .Min = 10, .Max = 20});
        static auto* memType = WasmEdge_MemoryTypeCreate(
            {.HasMax = true, .Shared = false, .Min = 1, .Max = 1});
        static WasmEdge_String memName =
            WasmEdge_StringCreateByCString("memory");
        static WasmEdge_String cbakFunctionName =
            WasmEdge_StringCreateByCString("cbak");
        static WasmEdge_String hookFunctionName =
            WasmEdge_StringCreateByCString("hook");

        // Bridge data must not reallocate during registration
        std::vector<BridgeData> bridgeData;
        bridgeData.reserve(imports.size());

        auto* importObj = WasmEdge_ModuleInstanceCreate(exportName);

        for (auto const& decl : imports)
        {
            // featureGate: if non-null and not zero, check rules
            if (decl.featureGate && !(*decl.featureGate).isZero() &&
                !rules.enabled(*decl.featureGate))
                continue;

            bridgeData.push_back({&decl, &ctx});

            std::vector<WasmEdge_ValType> paramTypes;
            paramTypes.reserve(decl.params.size());
            for (auto k : decl.params)
                paramTypes.push_back(
                    k == WasmValue::Kind::I32 ? WasmEdge_ValType_I32
                                              : WasmEdge_ValType_I64);

            WasmEdge_ValType resultType = decl.result == WasmValue::Kind::I32
                ? WasmEdge_ValType_I32
                : WasmEdge_ValType_I64;

            auto* fnType = WasmEdge_FunctionTypeCreate(
                paramTypes.data(), paramTypes.size(), &resultType, 1);
            auto* fn = WasmEdge_FunctionInstanceCreate(
                fnType, bridgeFn, &bridgeData.back(), 0);
            WasmEdge_FunctionTypeDelete(fnType);

            auto nameStr = WasmEdge_StringCreateByCString(decl.name);
            WasmEdge_ModuleInstanceAddFunction(importObj, nameStr, fn);
            WasmEdge_StringDelete(nameStr);
        }

        WasmEdge_ModuleInstanceAddTable(
            importObj, tableName, WasmEdge_TableInstanceCreate(tableType));
        WasmEdge_ModuleInstanceAddMemory(
            importObj, memName, WasmEdge_MemoryInstanceCreate(memType));

        JLOG(j.trace()) << "HookInfo[" << ctx.result.account << "-"
                        << ctx.result.otxnAccount
                        << "]: creating wasm instance";

        WasmEdge_LogOff();

        WasmEdgeVM vm;
        if (!vm.sane())
        {
            WasmEdge_ModuleInstanceDelete(importObj);
            JLOG(j.warn()) << "HookError[" << ctx.result.account << "-"
                           << ctx.result.otxnAccount
                           << "]: Could not create WASMEDGE instance.";
            return {false, 0, "Could not create WASMEDGE instance"};
        }

        WasmEdge_Result res =
            WasmEdge_VMRegisterModuleFromImport(vm.ctx, importObj);
        if (auto err = getWasmError("Import phase failed", res))
        {
            JLOG(j.trace()) << "HookError[" << ctx.result.account << "-"
                            << ctx.result.otxnAccount << "]: " << *err;
            WasmEdge_ModuleInstanceDelete(importObj);
            return {false, 0, *err};
        }

        WasmEdge_Value params[1] = {WasmEdge_ValueGenI32((int64_t)wasmParam)};
        WasmEdge_Value returns[1];

        res = WasmEdge_VMRunWasmFromBuffer(
            vm.ctx,
            reinterpret_cast<uint8_t const*>(wasm),
            len,
            isCallback ? cbakFunctionName : hookFunctionName,
            params,
            1,
            returns,
            1);

        ExecutionResult result;
        if (auto err = getWasmError("WASM VM error", res))
        {
            result.ok = false;
            result.instructionCount = 0;
            result.error = *err;
        }
        else
        {
            result.ok = true;
            auto* statsCtx = WasmEdge_VMGetStatisticsContext(vm.ctx);
            result.instructionCount =
                WasmEdge_StatisticsGetInstrCount(statsCtx);
        }

        WasmEdge_ModuleInstanceDelete(importObj);
        return result;
    }
};

}  // anonymous namespace

// ── hookHostFunctionDecls registry ─────────────────────────────────────────
// Expand hook_api.macro with custom HOOK_API_DEFINITION to build the list.
// Each entry stores the WasmFunction##F pointer and parameter kinds.
// For featureGate: uint256{} means "always available" (stored as nullptr),
// named features are stored as their address.

#pragma push_macro("HOOK_API_DEFINITION")
#pragma push_macro("HOOK_WRAP_PARAMS")
#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS

// Helper to convert feature gate argument: uint256{} → nullptr,
// named feature variable → address of that variable.
// We use an inline helper function template to do this.
namespace {
template <typename T>
static inline uint256 const*
featureGatePtr(T const& gate)
{
    // If gate is zero (default uint256{}), return nullptr (no gate).
    if (gate.isZero())
        return nullptr;
    return &gate;
}
}  // namespace

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(R, F, P, GATE) \
    {                                      \
        #F,                                \
        &hook_api::WasmFunction##F,        \
        buildKinds<HOOK_WRAP_PARAMS P>(),  \
        kindOf<R>(),                       \
        featureGatePtr(GATE),              \
    },

std::vector<HostFunctionDecl> const&
hookHostFunctionDecls()
{
    static std::vector<HostFunctionDecl> const decls = {
#include <xrpl/hook/hook_api.macro>
    };
    return decls;
}

#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS
#pragma pop_macro("HOOK_WRAP_PARAMS")
#pragma pop_macro("HOOK_API_DEFINITION")

// ── Factory ─────────────────────────────────────────────────────────────────

std::unique_ptr<IWasmEngine>
makeWasmEdgeEngine()
{
    return std::make_unique<WasmEdgeEngineImpl>();
}

}  // namespace hook
