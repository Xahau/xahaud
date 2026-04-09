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
#undef HOOK_API_DEFINITION

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    DECLARE_HOOK_FUNCTION(                                                 \
        RETURN_TYPE, FUNCTION_NAME, HOOK_WRAP_PARAMS PARAMS_TUPLE);

#include <xrpl/hook/hook_api.macro>

#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS
#pragma pop_macro("HOOK_API_DEFINITION")

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
    uint32_t wasmParam,
    uint8_t hookChainPosition,
    // result of apply() if this is weak exec
    std::shared_ptr<STObject const> const& provisionalMeta);

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
    uint32_t wasmParam = 0;
    uint32_t overrideCount = 0;
    uint8_t hookChainPosition = 0;
    bool foreignStateSetDisabled = false;
    bool executeAgainAsWeak =
        false;  // hook_again allows strong pre-apply to nominate
                // additional weak post-apply execution
    std::shared_ptr<STObject const> provisionalMeta;
};

class HookExecutor;

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
    const HookExecutor* module = 0;

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
#define ADD_HOOK_FUNCTION(F, ctx)                          \
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

#define HR_ACC() hookResult.account << "-" << hookResult.otxnAccount
#define HC_ACC() hookCtx.result.account << "-" << hookCtx.result.otxnAccount

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

/**
 * HookExecutor is effectively a two-part function:
 * The first part sets up the Hook Api inside the wasm import, ready for use
 * (this is done during object construction.)
 * The second part is actually executing webassembly instructions
 * this is done during execteWasm function.
 * The instance is single use.
 */
class HookExecutor
{
private:
    bool spent = false;  // a HookExecutor can only be used once

public:
    HookContext& hookCtx;
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
    validateWasm(const void* wasm, size_t len)
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
    executeWasm(
        const void* wasm,
        size_t len,
        bool callback,
        uint32_t wasmParam,
        beast::Journal const& j)
    {
        // HookExecutor can only execute once
        XRPL_ASSERT(
            !spent,
            "HookExecutor::executeWasm : HookExecutor can only execute once");

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

        WasmEdge_Value params[1] = {WasmEdge_ValueGenI32((int64_t)wasmParam)};
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

    HookExecutor(HookContext& ctx)
        : hookCtx(ctx), importObj(WasmEdge_ModuleInstanceCreate(exportName))
    {
        ctx.module = this;

        WasmEdge_LogSetDebugLevel();

#pragma push_macro("HOOK_API_DEFINITION")
#undef HOOK_API_DEFINITION

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    ADD_HOOK_FUNCTION(FUNCTION_NAME, ctx);

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

    ~HookExecutor()
    {
        WasmEdge_ModuleInstanceDelete(importObj);
    };
};

}  // namespace hook

#endif
