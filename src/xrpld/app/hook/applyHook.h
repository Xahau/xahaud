#ifndef APPLY_HOOK_INCLUDED
#define APPLY_HOOK_INCLUDED 1
#include <xrpld/app/hook/HookAPI.h>
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/app/tx/detail/ApplyContext.h>
#include <xrpl/basics/Blob.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/hook/Macro.h>
#include <xrpl/hook/Misc.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/digest.h>
#include <any>
#include <memory>
#include <optional>
#include <queue>
#include <vector>
#include <wasmedge/wasmedge.h>

#include "quickjs-atom.h"
#include "quickjs-libc.h"
#include "quickjs.h"

extern "C" {
int
js_code_init_textdecoder(JSContext*, JSModuleDef* m);
}

namespace hook {
struct HookContext;
struct HookResult;
bool
isEmittedTxn(ripple::STTx const& tx);

// This map type acts as both a read and write cache for hook execution
// and is preserved across the execution of the set of hook chains
// being executed in the current transaction. It is committed to lgr
// only upon tesSuccess for the otxn.
class HookStateMap : public std::map<
                         ripple::AccountID,  // account that owns the state
                         std::tuple<
                             int64_t,   // remaining available ownercount
                             int64_t,   // total namespace count
                             uint16_t,  // hook state scale
                             std::map<
                                 ripple::uint256,  // namespace
                                 std::map<
                                     ripple::uint256,  // key
                                     std::pair<
                                         bool,  // is modified from ledger value
                                         ripple::Blob>>>>>  // the value
{
public:
    uint32_t modified_entry_count = 0;  // track the number of total modified
};

using namespace ripple;
std::vector<std::pair<AccountID, bool>>
getTransactionalStakeHolders(STTx const& tx, ReadView const& rv);
}  // namespace hook

namespace hook_api {

// for debugging if you want a lot of output change to 1
#define HOOK_DBG 0
#define DBG_PRINTF \
    if (HOOK_DBG)  \
    printf
#define DBG_FPRINTF \
    if (HOOK_DBG)   \
    fprintf

#pragma push_macro("HOOK_API_DEFINITION")
#pragma push_macro("JSHOOK_API_DEFINITION")
#undef HOOK_API_DEFINITION
#undef JSHOOK_API_DEFINITION

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    DECLARE_WASM_FUNCTION(                                                 \
        RETURN_TYPE, FUNCTION_NAME, HOOK_WRAP_PARAMS PARAMS_TUPLE);

#define JSHOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    DECLARE_JS_FUNCTION(JSValue, FUNCTION_NAME, HOOK_WRAP_PARAMS PARAMS_TUPLE);

#include <xrpl/hook/hook_api.macro>

#undef HOOK_API_DEFINITION
#undef JSHOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS
#pragma pop_macro("HOOK_API_DEFINITION")
#pragma pop_macro("JSHOOK_API_DEFINITION")

} /* end namespace hook_api */

namespace hook {

bool
canHook(ripple::TxType txType, ripple::uint256 hookOn);

bool
canEmit(ripple::TxType txType, ripple::uint256 hookCanEmit);

ripple::uint256
getHookCanEmit(ripple::STObject const& hookObj, SLE::pointer const& hookDef);

ripple::uint256
getHookOn(
    STObject const& obj,
    std::shared_ptr<SLE const> const& def,
    SField const& field);

struct HookResult;

HookResult
apply(
    ripple::uint256 const& hookSetTxnID, /* this is the txid of the sethook,
                                            used for caching (one day) */
    ripple::uint256 const&
        hookHash, /* hash of the actual hook byte code, used for metadata */
    uint16_t hookApiVersion,
    ripple::uint256 const& hookCanEmit,
    ripple::uint256 const& hookNamespace,
    ripple::Blob const& wasm,
    std::map<
        std::vector<uint8_t>, /* param name  */
        std::vector<uint8_t>  /* param value */
        > const& hookParams,
    std::map<
        ripple::uint256, /* hook hash */
        std::map<std::vector<uint8_t>, std::vector<uint8_t>>> const&
        hookParamOverrides,
    HookStateMap& stateMap,
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& account, /* the account the hook is INSTALLED ON
                                         not always the otxn account */
    bool hasCallback,
    bool isCallback,
    bool isStrongTSH,
    uint32_t hookArgument,
    uint8_t hookChainPosition,
    // result of apply() if this is weak exec
    std::shared_ptr<STObject const> const& provisionalMeta,
    uint32_t instructionLimit);

struct HookContext;

int64_t
computeExecutionFee(uint64_t instructionCount);
int64_t
computeCreationFee(uint64_t byteCount);

struct HookResult
{
    ripple::uint256 const hookSetTxnID;
    ripple::uint256 const hookHash;
    ripple::uint256 const hookCanEmit;
    ripple::Keylet const accountKeylet;
    ripple::Keylet const hookKeylet;
    ripple::AccountID const account;
    ripple::AccountID const otxnAccount;
    ripple::uint256 const hookNamespace;

    std::queue<std::shared_ptr<ripple::Transaction>>
        emittedTxn{};  // etx stored here until accept/rollback
    HookStateMap& stateMap;
    uint16_t changedStateCount = 0;
    std::map<
        ripple::uint256,  // hook hash
        std::map<
            std::vector<uint8_t>,  // hook param name
            std::vector<uint8_t>   // hook param value
            >>
        hookParamOverrides;

    std::map<std::vector<uint8_t>, std::vector<uint8_t>> const& hookParams;
    std::set<ripple::uint256> hookSkips;
    hook_api::ExitType exitType = hook_api::ExitType::ROLLBACK;
    std::string exitReason{""};
    int64_t exitCode{-1};
    uint64_t instructionCount{0};
    bool hasCallback = false;  // true iff this hook wasm has a cbak function
    bool isCallback =
        false;  // true iff this hook execution is a callback in action
    bool isStrong = false;
    uint32_t hookArgument = 0;
    uint32_t overrideCount = 0;
    uint8_t hookChainPosition = 0;
    bool foreignStateSetDisabled = false;
    bool executeAgainAsWeak =
        false;  // hook_again allows strong pre-apply to nominate
                // additional weak post-apply execution
    std::shared_ptr<STObject const> provisionalMeta;
};

class HookExecutorBase;
// class HookExecutorWasm;
// class HookExecutorJs;

struct SlotEntry
{
    std::shared_ptr<const ripple::STObject> storage;
    const ripple::STBase* entry;  // raw pointer into the storage, that can be
                                  // freely pointed around inside
};

struct HookContext
{
    ripple::ApplyContext& applyCtx;
    // slots are used up by requesting objects from inside the hook
    // the map stores pairs consisting of a memory view and whatever shared or
    // unique ptr is required to keep the underlying object alive for the
    // duration of the hook's execution slot number -> { keylet or hash, {
    // pointer to current object, storage for that object } }
    std::map<uint32_t, SlotEntry> slot{};
    std::queue<uint32_t> slot_free{};
    uint32_t slot_counter{0};  // uint16 to avoid accidental overflow and to
                               // allow more slots in future
    uint16_t emit_nonce_counter{
        0};  // incremented whenever nonce is called to ensure unique nonces
    uint16_t ledger_nonce_counter{0};
    int64_t expected_etxn_count{-1};  // make this a 64bit int so the uint32
                                      // from the hookapi cant overflow it
    std::map<ripple::uint256, bool> nonce_used{};
    uint32_t generation =
        0;  // used for caching, only generated when txn_generation is called
    uint64_t burden =
        0;  // used for caching, only generated when txn_burden is called
    std::map<uint32_t, uint32_t>
        guard_map{};  // iteration guard map <id -> upto_iteration>
    HookResult result;
    std::optional<ripple::STObject>
        emitFailure;  // if this is a callback from a failed
                      // emitted txn then this optional becomes
                      // populated with the SLE
    const HookExecutorBase* module = 0;

    // Lazy-initialized HookAPI member
    mutable std::unique_ptr<HookAPI> api_;

    // Access the HookAPI instance (lazy initialization)
    HookAPI&
    api() const
    {
        if (!api_)
            api_ = std::make_unique<HookAPI>(const_cast<HookContext&>(*this));
        return *api_;
    }
};

bool
addHookNamespaceEntry(ripple::SLE& sleAccount, ripple::uint256 ns);

bool
removeHookNamespaceEntry(ripple::SLE& sleAccount, ripple::uint256 ns);

ripple::TER
setHookState(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key,
    ripple::Slice const& data);

// write hook execution metadata and remove emitted transaction ledger entries
ripple::TER
finalizeHookResult(
    hook::HookResult& hookResult,
    ripple::ApplyContext&,
    bool doEmit);

// write state map to ledger
ripple::TER
finalizeHookState(
    HookStateMap const&,
    ripple::ApplyContext&,
    ripple::uint256 const&);

// if the txn being executed was an emitted txn then this removes it from the
// emission directory
ripple::TER
removeEmissionEntry(ripple::ApplyContext& applyCtx);

bool /* retval of true means an error */
gatherHookParameters(
    std::shared_ptr<ripple::STLedgerEntry> const& hookDef,
    ripple::STObject const& hookObj,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& parameters,
    beast::Journal const& j_);

// RH TODO: call destruct for these on rippled shutdown
#define ADD_WASM_FUNCTION(F, ctx)                          \
    {                                                      \
        WasmEdge_FunctionInstanceContext* hf =             \
            WasmEdge_FunctionInstanceCreate(               \
                hook_api::WasmFunctionType##F,             \
                hook_api::WasmFunction##F,                 \
                (void*)(&ctx),                             \
                0);                                        \
        WasmEdge_ModuleInstanceAddFunction(                \
            importObj, hook_api::WasmFunctionName##F, hf); \
    }

#define ADD_JS_FUNCTION(F, ctx)                       \
    {                                                 \
        JSValue global_obj = JS_GetGlobalObject(ctx); \
        JS_SetPropertyStr(                            \
            ctx,                                      \
            global_obj,                               \
            #F,                                       \
            JS_NewCFunction(                          \
                ctx,                                  \
                hook_api::JSFunction##F,              \
                #F,                                   \
                hook_api::JSFunctionParamCount##F));  \
        JS_FreeValue(ctx, global_obj);                \
    }

#define HR_ACC() hookResult.account << "-" << hookResult.otxnAccount
#define HC_ACC() hookCtx.result.account << "-" << hookCtx.result.otxnAccount

class HookExecutorBase
{
protected:
    bool spent = false;  // an HookExecutor can only be used once

public:
    HookContext& hookCtx;

    HookExecutorBase(HookContext& ctx) : hookCtx(ctx)
    {
    }

    virtual void
    execute(
        const void* code,
        size_t len,
        bool callback,
        uint32_t param,
        uint32_t instructionLimit,
        beast::Journal const& j) = 0;

    static std::optional<std::string>
    validate(const void* code, size_t len)
    {
        // Base class doesn't implement validation
        return "validate() illegally called on HookExecutorBase class";
    }

    virtual ~HookExecutorBase() = default;
};

/**
 * HookExecutorWasm is effectively a two-part function:
 * The first part sets up the Hook Api inside the wasm import, ready for use
 * (this is done during object construction.)
 * The second part is actually executing webassembly instructions
 * this is done during execteWasm function.
 * The instance is single use.
 */

// create these once at boot and keep them
static WasmEdge_String exportName = WasmEdge_StringCreateByCString("env");
static WasmEdge_String tableName = WasmEdge_StringCreateByCString("table");
static auto* tableType = WasmEdge_TableTypeCreate(
    WasmEdge_RefType_FuncRef,
    {.HasMax = true, .Shared = false, .Min = 10, .Max = 20});
static auto* memType = WasmEdge_MemoryTypeCreate(
    {.HasMax = true, .Shared = false, .Min = 1, .Max = 1});
static WasmEdge_String memName = WasmEdge_StringCreateByCString("memory");
static WasmEdge_String cbakFunctionName =
    WasmEdge_StringCreateByCString("cbak");
static WasmEdge_String hookFunctionName =
    WasmEdge_StringCreateByCString("hook");

// see: lib/system/allocator.cpp
#define WasmEdge_kPageSize 65536ULL

class HookExecutorWasm : public HookExecutorBase
{
public:
    WasmEdge_ModuleInstanceContext* importObj;

    class WasmEdgeVM
    {
    public:
        WasmEdge_ConfigureContext* conf = NULL;
        WasmEdge_VMContext* ctx = NULL;

        WasmEdgeVM()
        {
            conf = WasmEdge_ConfigureCreate();
            if (!conf)
                return;
            WasmEdge_ConfigureStatisticsSetInstructionCounting(conf, true);
            ctx = WasmEdge_VMCreate(conf, NULL);
        }

        bool
        sane()
        {
            return ctx && conf;
        }

        ~WasmEdgeVM()
        {
            if (conf)
                WasmEdge_ConfigureDelete(conf);
            if (ctx)
                WasmEdge_VMDelete(ctx);
        }
    };

    // if an error occured return a string prefixed with `prefix` followed by
    // the error description
    static std::optional<std::string>
    getWasmError(std::string prefix, WasmEdge_Result& res)
    {
        if (WasmEdge_ResultOK(res))
            return {};

        const char* msg = WasmEdge_ResultGetMessage(res);
        return prefix + ": " + (msg ? msg : "unknown error");
    }

    /**
     * Validate that a web assembly blob can be loaded by wasmedge
     */
    static std::optional<std::string>
    validate(const void* wasm, size_t len)
    {
        WasmEdgeVM vm;

        if (!vm.sane())
            return "Could not create WASMEDGE instance";

        WasmEdge_Result res = WasmEdge_VMLoadWasmFromBuffer(
            vm.ctx, reinterpret_cast<const uint8_t*>(wasm), len);

        if (auto err = getWasmError("VMLoadWasmFromBuffer failed", res); err)
            return *err;

        res = WasmEdge_VMValidate(vm.ctx);

        if (auto err = getWasmError("VMValidate failed", res); err)
            return *err;

        return {};
    }

    /**
     * Execute web assembly byte code against the constructed Hook Context
     * Once execution has occured the exector is spent and cannot be used again
     * and should be destructed Information about the execution is populated
     * into hookCtx
     */
    void
    execute(
        const void* wasm,
        size_t len,
        bool callback,
        uint32_t hookArgument,
        uint32_t instructionLimit, /* this is unused in wasm, set to 0 */
        beast::Journal const& j) override
    {
        // HookExecutorWasm can only execute once
        XRPL_ASSERT(
            !spent,
            "HookExecutorWasm::execute : HookExecutorWasm can only execute "
            "once");

        spent = true;

        JLOG(j.trace()) << "HookInfo[" << HC_ACC()
                        << "]: creating wasm instance";

        WasmEdge_LogOff();

        WasmEdgeVM vm;

        if (!vm.sane())
        {
            JLOG(j.warn()) << "HookError[" << HC_ACC()
                           << "]: Could not create WASMEDGE instance.";

            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            return;
        }

        WasmEdge_Result res =
            WasmEdge_VMRegisterModuleFromImport(vm.ctx, this->importObj);

        if (auto err = getWasmError("Import phase failed", res); err)
        {
            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            JLOG(j.trace()) << "HookError[" << HC_ACC() << "]: " << *err;
            return;
        }

        WasmEdge_Value params[1] = {
            WasmEdge_ValueGenI32((int64_t)hookArgument)};
        WasmEdge_Value returns[1];

        res = WasmEdge_VMRunWasmFromBuffer(
            vm.ctx,
            reinterpret_cast<const uint8_t*>(wasm),
            len,
            callback ? cbakFunctionName : hookFunctionName,
            params,
            1,
            returns,
            1);

        if (auto err = getWasmError("WASM VM error", res); err)
        {
            JLOG(j.warn()) << "HookError[" << HC_ACC() << "]: " << *err;
            hookCtx.result.exitType = hook_api::ExitType::WASM_ERROR;
            return;
        }

        auto* statsCtx = WasmEdge_VMGetStatisticsContext(vm.ctx);
        hookCtx.result.instructionCount =
            WasmEdge_StatisticsGetInstrCount(statsCtx);

        // RH NOTE: stack unwind will clean up WasmEdgeVM
    }

    HookExecutorWasm(HookContext& ctx)
        : HookExecutorBase(ctx)
        , importObj(WasmEdge_ModuleInstanceCreate(exportName))
    {
        ctx.module = this;

        WasmEdge_LogSetDebugLevel();

#pragma push_macro("HOOK_API_DEFINITION")
#undef HOOK_API_DEFINITION

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    ADD_WASM_FUNCTION(FUNCTION_NAME, ctx);

#include <xrpl/hook/hook_api.macro>

#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS
#pragma pop_macro("HOOK_API_DEFINITION")

        WasmEdge_TableInstanceContext* hostTable =
            WasmEdge_TableInstanceCreate(tableType);
        WasmEdge_ModuleInstanceAddTable(importObj, tableName, hostTable);
        WasmEdge_MemoryInstanceContext* hostMem =
            WasmEdge_MemoryInstanceCreate(memType);
        WasmEdge_ModuleInstanceAddMemory(importObj, memName, hostMem);
    }

    virtual ~HookExecutorWasm()
    {
        WasmEdge_ModuleInstanceDelete(importObj);
    };
};

class HookExecutorJS : public HookExecutorBase
{
public:
    class QuickJSVM
    {
    public:
        JSRuntime* rt = NULL;
        JSContext* ctx = NULL;

        QuickJSVM(void* hookCtx, uint32_t instructionLimit, uint64_t dateNow)
        {
            rt = JS_NewRuntime(instructionLimit, dateNow);
            ctx = JS_NewContextRaw(rt);
            JS_AddIntrinsicBaseObjects(ctx);
            JS_AddIntrinsicDate(ctx);
            JS_AddIntrinsicEval(ctx);
            JS_AddIntrinsicStringNormalize(ctx);
            JS_AddIntrinsicRegExp(ctx);
            JS_AddIntrinsicJSON(ctx);
            JS_AddIntrinsicMapSet(ctx);
            JS_AddIntrinsicTypedArrays(ctx);
            JS_AddIntrinsicBigInt(ctx);
            //::js_init_module_textdecoder(ctx, "textdecoder");
            ::js_code_init_textdecoder(ctx, 0);

            JS_SetMaxStackSize(rt, 65535);
            JS_SetMemoryLimit(rt, 16 * 1024 * 1024);

            JS_SetRuntimeOpaque(rt, hookCtx);

#pragma push_macro("JSHOOK_API_DEFINITION")
#undef JSHOOK_API_DEFINITION

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    {                                                                      \
    }
#define JSHOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    ADD_JS_FUNCTION(FUNCTION_NAME, ctx);

#include <xrpl/hook/hook_api.macro>

#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS
#pragma pop_macro("JSHOOK_API_DEFINITION")
        }

        bool
        sane()
        {
            return ctx && rt;
        }

        ~QuickJSVM()
        {
            if (ctx)
                JS_FreeContext(ctx);
            if (rt)
                JS_FreeRuntime(rt);
            ctx = NULL;
            rt = NULL;
        }
    };

    /**
     * Helper function to handle a JS exception by extracting its message
     * and freeing any additional JSValues
     */
    template <typename... JSValues>
    static std::string
    handleException(
        JSContext* ctx,
        const char* defaultMsg,
        JSValues... valuesToFree)
    {
        // Get the actual exception object
        JSValue exception = JS_GetException(ctx);

        // Extract the error message property
        JSValue msgProp = JS_GetPropertyStr(ctx, exception, "message");
        const char* str = JS_ToCString(ctx, msgProp);

        std::string result = (str != nullptr) ? str : defaultMsg;

        if (str != nullptr)
            JS_FreeCString(ctx, str);

        JS_FreeValue(ctx, msgProp);
        JS_FreeValue(ctx, exception);

        // Free all additional values passed
        (JS_FreeValue(ctx, valuesToFree), ...);

        return result;
    }

    /**
     * Validate that a js blob can be loaded by quickjs
     */
    static std::optional<std::string>
    validate(const void* buf, size_t buf_len)
    {
        if (buf_len < 5)
            return "Could not create QUICKJS instance, bytecode too short.";

        std::optional<std::string> retval;

        // RHNOTE: hard instrction (internal program counter loop) limit of 1MM,
        // like wasm.
        QuickJSVM vm{NULL, 1000000, 0};
        JSContext* ctx = vm.ctx;

        if (!vm.sane())
            return "Could not create QUICKJS instance";

        JSValue obj = JS_ReadObject(
            ctx, (uint8_t const*)buf, buf_len, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(obj) || JS_IsUndefined(obj))
        {
            return handleException(ctx, "invalid bytecode");
        }

        JSValue val = JS_EvalFunction(ctx, obj);
        if (JS_IsException(val))
        {
            return handleException(ctx, "bytecode eval failure", obj);
        }

        JS_FreeValue(ctx, val);

        const char* testCalls =
            "if (typeof(Hook) != \"function\" || "
            "(typeof(Callback) != \"function\" && typeof(Callback) != "
            "\"undefined\")) "
            "throw Error(\"Hook/Callback function required\")";

        val = JS_Eval(vm.ctx, testCalls, strlen(testCalls), "<qjsvm>", 0);

        if (JS_IsException(val))
        {
            std::string errMsg =
                handleException(ctx, "Hook/Callback validation failure", obj);
            return errMsg;
        }

        JS_FreeValue(ctx, val);
        // We don't manually free the bytecode object (obj) here because
        // JS_EvalFunction internally transforms it into a closure and takes
        // ownership of its internal structures.
        // JS_FreeValue(ctx, obj);

        return retval;
    }

    /**
     * Execute QuickJS bytecode against hook context.
     * Once execution has occured the exector is spent and cannot be used again
     * and should be destructed Information about the execution is populated
     * into hookCtx
     */
    void
    execute(
        const void* buf,
        size_t buf_len,
        bool callback,
        uint32_t hookArgument,
        uint32_t instructionLimit,
        beast::Journal const& j) override
    {
        // HookExecutorWasm can only execute once
        assert(!spent);
        spent = true;

        JLOG(j.trace()) << "HookInfo[" << HC_ACC()
                        << "]: creating quickjs instance";

        uint64_t dateNow =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                hookCtx.applyCtx.view()
                    .info()
                    .parentCloseTime.time_since_epoch())
                .count() +
            946684800000;
        QuickJSVM vm{
            reinterpret_cast<void*>(&hookCtx), instructionLimit, dateNow};
        JSContext* ctx = vm.ctx;

        if (!vm.sane())
        {
            JLOG(j.warn()) << "HookError[" << HC_ACC()
                           << "]: Could not create QUICKJS instance.";

            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        JSValue obj = JS_ReadObject(
            ctx, (uint8_t const*)buf, buf_len, JS_READ_OBJ_BYTECODE);

        if (JS_IsException(obj))
        {
            JS_FreeValue(ctx, obj);
            JLOG(j.warn())
                << "HookError[" << HC_ACC()
                << "]: Could not create QUICKJS instance (invalid bytecode).";
            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        JSValue val = JS_EvalFunction(ctx, obj);

        if (JS_IsException(val))
        {
            JS_FreeValue(ctx, val);

            JLOG(j.warn()) << "HookError[" << HC_ACC()
                           << "]: Could not create QUICKJS instance (bytecode "
                              "eval failure).";
            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        JS_FreeValue(ctx, val);

        char expr[256];

        int expr_len = snprintf(
            expr, 256, "%s(%d)", callback ? "Callback" : "Hook", hookArgument);

        if (expr_len < 7 || expr_len == 256)
        {
            JLOG(j.warn())
                << "HookError[" << HC_ACC()
                << "]: Could not create QUICKJS instance (expr string).";

            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            return;
        }

        val = JS_Eval(vm.ctx, expr, expr_len, "<qjsvm>", 0);

        int normal_exit = 0;

        JSValue exception_val = JS_GetException(ctx);
        if (!JS_IsUndefined(exception_val) && JS_IsError(ctx, exception_val))
        {
            int printed_something = 0;

            // Most exceptions should have a message field, try to fetch and
            // print that
            JSValue msg = JS_GetPropertyStr(ctx, exception_val, "message");
            if (!JS_IsUndefined(msg))
            {
                if (const char* str = JS_ToCString(ctx, msg); str)
                {
                    std::string m(str);
                    JS_FreeCString(ctx, str);

                    if ((m == "HookExit Accept" || m == "HookExit Rollback") &&
                        (hookCtx.result.exitType ==
                             hook_api::ExitType::ACCEPT ||
                         hookCtx.result.exitType ==
                             hook_api::ExitType::ROLLBACK))
                    {
                        normal_exit = 1;
                    }
                    else if (m == "Instruction limit reached")
                    {
                        normal_exit = 1;
                        hookCtx.result.exitType =
                            hook_api::ExitType::INSTRUCTION_LIMIT_REACHED;
                    }
                    else
                    {
                        JLOG(j.warn()) << "HookError[" << HC_ACC()
                                       << "]: JSException " << m;
                        printed_something++;
                    }
                }
            }
            JS_FreeValue(ctx, msg);

            // Accept/rollback are handled via an uncatchable exception
            // internally in quickjs so only print a backtrace if it isn't a
            // normal exit.
            if (!normal_exit)
            {
                if (!printed_something)
                {
                    JLOG(j.warn())
                        << "HookError[" << HC_ACC() << "]: [unknown exception]";
                }

                JSValue bt = JS_GetPropertyStr(ctx, exception_val, "stack");
                if (!normal_exit && !JS_IsUndefined(bt))
                {
                    if (const char* str = JS_ToCString(ctx, bt); str)
                    {
                        JLOG(j.warn())
                            << "HookError[" << HC_ACC() << "]: " << str;
                        JS_FreeCString(ctx, str);
                    }
                }

                JS_FreeValue(ctx, bt);
            }
        }
        JS_FreeValue(ctx, exception_val);

        if (normal_exit)
        {
            if (hookCtx.result.exitType == hook_api::ExitType::ACCEPT)
            {
                JLOG(j.warn())
                    << "HookInfo[" << HC_ACC() << "]: JSVM Exited with ACCEPT";
            }
            else
            {
                JLOG(j.warn()) << "HookInfo[" << HC_ACC()
                               << "]: JSVM Exited with ROLLBACK";
            }
            JLOG(j.warn()) << "HookInfo[" << HC_ACC()
                           << "]: Instruction Count: "
                           << JS_GetInstructionCount(ctx);
            hookCtx.result.instructionCount = JS_GetInstructionCount(ctx);
        }
        /*
            // RHTODO: place jsvm_error exit type logic appropriately
            hookCtx.result.exitType = hook_api::ExitType::JSVM_ERROR;
            hookCtx.result.instructionCount = 0; //?
        */
        JS_FreeValue(ctx, val);
    }

    HookExecutorJS(HookContext& ctx) : HookExecutorBase(ctx)
    {
        ctx.module = this;
    }

    virtual ~HookExecutorJS() {};
};

}  // namespace hook

#endif
