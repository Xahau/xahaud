// WasmtimeEngine.cpp — Wasmtime backend for the IWasmEngine abstraction.
//
// WHY THIS FILE IS LARGER THAN WasmEdgeEngine.cpp
// ─────────────────────────────────────────────────
// WasmEdge provides WasmEdge_CallingFrameGetMemoryInstance(frame, index),
// which retrieves a module's linear memory by index regardless of whether it
// is exported.  Wasmtime has no equivalent: the only way to access memory
// from inside a host-function callback is wasmtime_caller_export_get(), which
// only works for *exported* memories.
//
// Hook WASM modules compiled by wasmcc define their own linear memory
// (WebAssembly section 5) but do not export it.  Without an export the memory
// pointer is unavailable inside host callbacks, causing every Hook API call
// that touches guest memory to see mem.base == nullptr and return
// INTERNAL_ERROR, ultimately crashing the execution.
//
// The ensureMemoryExported() function (≈200 lines) works around this by
// scanning the binary before instantiation and injecting a "memory" export
// entry into section 7 when the module owns but does not export its memory.
// The patched binary is semantically identical to the original.
//
// A potential alternative — storing the wasmtime_memory_t handle directly in
// BridgeData — requires knowing at setup time which memory the module will
// actually use (its own vs. an import from "env"), which itself requires
// binary inspection.  The export-injection approach is therefore the simplest
// correct solution.
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/hook/detail/WasmEngine.h>
#include <xrpld/app/hook/detail/WasmtimeEngine.h>
#include <xrpl/protocol/Feature.h>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
// wasmtime.h must come LAST to avoid int128_t pollution
#include <wasmtime.h>

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

// ── Error helpers ────────────────────────────────────────────────────────────

static std::optional<std::string>
wasmtimeError(wasmtime_error_t* err)
{
    if (!err)
        return {};
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    std::string s(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
    return s;
}

static std::optional<std::string>
wasmtimeTrap(wasm_trap_t* trap)
{
    if (!trap)
        return {};
    wasm_byte_vec_t msg;
    wasm_trap_message(trap, &msg);
    std::string s(msg.data, msg.size);
    wasm_byte_vec_delete(&msg);
    wasm_trap_delete(trap);
    return s;
}

// ── Process-global engine (thread-safe; created once) ────────────────────────

static wasm_engine_t*
getGlobalEngine()
{
    static wasm_engine_t* gEngine = []() -> wasm_engine_t* {
        wasm_config_t* cfg = wasm_config_new();
        if (!cfg)
            return nullptr;

        // Consensus-fixed configuration – never change without an Amendment
        wasmtime_config_consume_fuel_set(cfg, true);
        wasmtime_config_wasm_simd_set(cfg, true);
        wasmtime_config_wasm_relaxed_simd_set(cfg, false);
        wasmtime_config_wasm_reference_types_set(cfg, true);
        wasmtime_config_wasm_bulk_memory_set(cfg, true);
        wasmtime_config_wasm_multi_value_set(cfg, false);
        wasmtime_config_cranelift_nan_canonicalization_set(cfg, true);

        // wasm_threads is behind a feature flag; only set if available.
        // The compile-time feature guard in config.h controls whether
        // wasmtime_config_wasm_threads_set exists.
#ifdef WASMTIME_FEATURE_THREADS
        wasmtime_config_wasm_threads_set(cfg, false);
#endif

        // wasm_engine_new_with_config takes ownership of cfg
        return wasm_engine_new_with_config(cfg);
    }();
    return gEngine;
}

// ── WASM binary normalisation
// ─────────────────────────────────────────────────
//
// Hook WASM modules typically have their own memory (WebAssembly Section 5)
// but do NOT export it.  Wasmtime's wasmtime_caller_export_get() only works
// for *exported* memories, so without an export the memory pointer is
// unavailable inside host callbacks.
//
// This function inspects the binary and, when the module owns a memory but
// does not already export it as "memory", injects a new export entry for
// memory index 0.  The resulting binary is semantically identical to the
// original but now exposes its memory to the host.
//
// Modules that already export "memory" (or that import memory from "env") are
// returned unchanged.

// LEB128 helpers (unsigned, forward-only)
static uint32_t
readUleb128(uint8_t const* p, size_t& pos, size_t limit)
{
    uint32_t result = 0;
    uint32_t shift = 0;
    while (pos < limit)
    {
        uint8_t b = p[pos++];
        result |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
    }
    return result;
}

// Encode a value as unsigned LEB128 (appends to out)
static void
writeUleb128(std::vector<uint8_t>& out, uint32_t val)
{
    do
    {
        uint8_t b = val & 0x7F;
        val >>= 7;
        if (val)
            b |= 0x80;
        out.push_back(b);
    } while (val);
}

// Ensure the WASM binary exports its first memory as "memory".
// Returns the original bytes unchanged if no patching is needed,
// otherwise returns a patched copy.
static std::vector<uint8_t>
ensureMemoryExported(void const* wasm, size_t len)
{
    uint8_t const* p = reinterpret_cast<uint8_t const*>(wasm);

    // Validate magic + version.
    // NOTE: the magic bytes are  0x00 0x61 0x73 0x6D 0x01 0x00 0x00 0x00
    // Written as a string literal, "\x00asm…" is ambiguous because \x hex
    // escapes are greedy: "\x00a" == "\x0a" (newline).  Use a byte array.
    static constexpr uint8_t kWasmMagic[8] = {
        0x00,
        0x61,
        0x73,
        0x6D,  // \0asm
        0x01,
        0x00,
        0x00,
        0x00  // version 1
    };
    if (len < 8 || std::memcmp(p, kWasmMagic, 8) != 0)
        return {p, p + len};  // not a valid WASM binary, return as-is

    bool hasOwnMemory = false;
    bool exportsMemory = false;

    // offset to memory section (for the insert point) and export section
    size_t exportSectionOffset = 0;  // byte offset of existing export section
    size_t exportSectionPayloadOff = 0;  // offset of payload start
    size_t exportSectionPayloadLen = 0;  // payload length

    size_t pos = 8;
    while (pos + 1 < len)
    {
        uint8_t sectionId = p[pos++];
        size_t secLenPos = pos;
        uint32_t secLen = readUleb128(p, pos, len);
        size_t payloadStart = pos;

        if (sectionId == 5)  // memory section
        {
            // At least one memory entry means the module owns a memory
            size_t tmp = pos;
            uint32_t count = readUleb128(p, tmp, len);
            if (count > 0)
                hasOwnMemory = true;
        }
        else if (sectionId == 7)  // export section
        {
            exportSectionOffset = secLenPos - 1;  // start of section id byte
            exportSectionPayloadOff = payloadStart;
            exportSectionPayloadLen = secLen;

            size_t tmp = pos;
            uint32_t count = readUleb128(p, tmp, len);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t nameLen = readUleb128(p, tmp, len);
                if (tmp + nameLen > len)
                    break;
                bool isMemoryName =
                    (nameLen == 6 && std::memcmp(p + tmp, "memory", 6) == 0);
                tmp += nameLen;
                if (tmp >= len)
                    break;
                uint8_t kind = p[tmp++];           // export kind byte
                readUleb128(p, tmp, len);          // export index
                if (isMemoryName && kind == 0x02)  // kind 2 == memory
                {
                    exportsMemory = true;
                    break;
                }
            }
        }

        pos = payloadStart + secLen;
    }

    // No patching needed if:
    // - module does not own a memory (it may import one; caller_export_get
    // works)
    // - module already exports its memory
    if (!hasOwnMemory || exportsMemory)
        return {p, p + len};

    // Build a new memory export entry:  \x06 m e m o r y \x02 \x00
    // (name_len=6, "memory", kind=2 (memory), index=0)
    std::vector<uint8_t> newExportEntry;
    writeUleb128(newExportEntry, 6);  // name length
    for (char c : std::string("memory"))
        newExportEntry.push_back((uint8_t)c);
    newExportEntry.push_back(0x02);   // export kind: memory
    writeUleb128(newExportEntry, 0);  // memory index 0

    if (exportSectionOffset == 0)
    {
        // No export section exists at all – create one from scratch.
        // New section: id=7, payload = count(1) + entry
        std::vector<uint8_t> newSection;
        newSection.push_back(0x07);  // export section id
        std::vector<uint8_t> payload;
        writeUleb128(payload, 1);  // 1 export
        payload.insert(
            payload.end(), newExportEntry.begin(), newExportEntry.end());
        writeUleb128(newSection, (uint32_t)payload.size());
        newSection.insert(newSection.end(), payload.begin(), payload.end());

        // Insert the new section before the code section (id=10) or at end.
        // For correctness we append after the last known section.
        std::vector<uint8_t> result(p, p + len);
        result.insert(result.end(), newSection.begin(), newSection.end());
        return result;
    }
    else
    {
        // Patch the existing export section: increment count, append entry.

        // Read existing export count (as a LEB128)
        size_t countPos = exportSectionPayloadOff;
        uint32_t existingCount = readUleb128(p, countPos, len);
        uint32_t newCount = existingCount + 1;
        std::vector<uint8_t> newCountBytes;
        writeUleb128(newCountBytes, newCount);

        // New payload = new count + existing entries (skip old count bytes) +
        // new entry
        std::vector<uint8_t> newPayload;
        newPayload.insert(
            newPayload.end(), newCountBytes.begin(), newCountBytes.end());
        // Existing entries start at countPos, end at exportSectionPayloadOff +
        // exportSectionPayloadLen
        size_t entriesStart = countPos;
        size_t entriesEnd = exportSectionPayloadOff + exportSectionPayloadLen;
        if (entriesEnd > len)
            entriesEnd = len;
        newPayload.insert(newPayload.end(), p + entriesStart, p + entriesEnd);
        newPayload.insert(
            newPayload.end(), newExportEntry.begin(), newExportEntry.end());

        // Encode new section length as LEB128
        std::vector<uint8_t> newSecLen;
        writeUleb128(newSecLen, (uint32_t)newPayload.size());

        // Reconstruct the full binary:
        //   bytes before export section + id(0x07) + new_len + new_payload +
        //   bytes after
        std::vector<uint8_t> result;
        result.reserve(len + newExportEntry.size() + 4);
        // Part 1: everything before the export section
        result.insert(result.end(), p, p + exportSectionOffset);
        // Part 2: section id
        result.push_back(0x07);
        // Part 3: new length
        result.insert(result.end(), newSecLen.begin(), newSecLen.end());
        // Part 4: new payload
        result.insert(result.end(), newPayload.begin(), newPayload.end());
        // Part 5: everything after the export section
        size_t afterExport = exportSectionPayloadOff + exportSectionPayloadLen;
        if (afterExport < len)
            result.insert(result.end(), p + afterExport, p + len);
        return result;
    }
}

// ── Per-execution state shared between the host callback and caller ──────────

struct ExecState
{
    bool terminated = false;
};

struct BridgeData
{
    HostFunctionDecl const* decl;
    HookContext* ctx;
    ExecState* state;
};

// ── wasm_functype_t builder helper ───────────────────────────────────────────

static wasm_functype_t*
buildFuncType(
    std::vector<WasmValue::Kind> const& params,
    WasmValue::Kind result)
{
    wasm_valtype_vec_t paramVec;
    wasm_valtype_vec_t resultVec;

    if (params.empty())
    {
        wasm_valtype_vec_new_empty(&paramVec);
    }
    else
    {
        std::vector<wasm_valtype_t*> ptrs;
        ptrs.reserve(params.size());
        for (auto k : params)
            ptrs.push_back(wasm_valtype_new(
                k == WasmValue::Kind::I32 ? WASM_I32 : WASM_I64));
        wasm_valtype_vec_new(&paramVec, ptrs.size(), ptrs.data());
    }

    {
        wasm_valtype_t* rs[1] = {wasm_valtype_new(
            result == WasmValue::Kind::I32 ? WASM_I32 : WASM_I64)};
        wasm_valtype_vec_new(&resultVec, 1, rs);
    }

    return wasm_functype_new(&paramVec, &resultVec);
}

// ── Bridge callback ──────────────────────────────────────────────────────────

static wasm_trap_t*
bridgeFn(
    void* envPtr,
    wasmtime_caller_t* caller,
    wasmtime_val_t const* args,
    size_t nargs,
    wasmtime_val_t* results,
    size_t nresults)
{
    auto* bridge = static_cast<BridgeData*>(envPtr);

    // The module's memory is always exported as "memory" after
    // ensureMemoryExported() normalises the binary.  For modules that import
    // memory from "env" (rare) the imported memory is also accessible via
    // the same export path once the linker resolves it.
    wasmtime_extern_t memExtern;
    GuestMemory mem{nullptr, 0};
    if (wasmtime_caller_export_get(
            caller, "memory", 6 /* strlen("memory") */, &memExtern))
    {
        if (memExtern.kind == WASMTIME_EXTERN_MEMORY)
        {
            wasmtime_context_t* ctx = wasmtime_caller_context(caller);
            mem.base = wasmtime_memory_data(ctx, &memExtern.of.memory);
            mem.size = wasmtime_memory_data_size(ctx, &memExtern.of.memory);
        }
    }

    // Convert incoming wasmtime_val_t args to WasmValue
    size_t const paramCount = bridge->decl->params.size();
    std::vector<WasmValue> inVals(paramCount);
    for (size_t i = 0; i < paramCount && i < nargs; ++i)
    {
        if (bridge->decl->params[i] == WasmValue::Kind::I32)
            inVals[i] = WasmValue::i32((uint32_t)args[i].of.i32);
        else
            inVals[i] = WasmValue::i64((uint64_t)args[i].of.i64);
    }

    WasmValue outVal;
    auto status = bridge->decl->fn(
        bridge->ctx, mem, inVals.data(), paramCount, &outVal, 1);

    if (status == HostCallStatus::Terminate || status == HostCallStatus::Trap)
    {
        // Signal termination to the outer caller
        bridge->state->terminated = true;
        // Return a trap to unwind the WASM stack
        return wasmtime_trap_new("hook terminated", 15);
    }

    // Write result
    if (nresults > 0)
    {
        if (bridge->decl->result == WasmValue::Kind::I32)
        {
            results[0].kind = WASMTIME_I32;
            results[0].of.i32 = (int32_t)outVal.asI32();
        }
        else
        {
            results[0].kind = WASMTIME_I64;
            results[0].of.i64 = (int64_t)outVal.asI64();
        }
    }

    return nullptr;  // success
}

// ── WasmtimeEngineImpl ───────────────────────────────────────────────────────

class WasmtimeEngineImpl : public IWasmEngine
{
public:
    std::optional<std::string>
    validate(void const* wasm, size_t len) override
    {
        wasm_engine_t* engine = getGlobalEngine();
        if (!engine)
            return "Could not create Wasmtime engine";

        // Normalise the binary so validation sees the same form as execution.
        auto patched = ensureMemoryExported(wasm, len);

        wasmtime_module_t* mod = nullptr;
        wasmtime_error_t* err =
            wasmtime_module_new(engine, patched.data(), patched.size(), &mod);

        if (auto msg = wasmtimeError(err))
            return "Wasmtime validate failed: " + *msg;

        wasmtime_module_delete(mod);
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
        wasm_engine_t* engine = getGlobalEngine();
        if (!engine)
            return {false, 0, "Could not obtain Wasmtime engine"};

        // Ensure the module's memory is exported so that bridgeFn can access it
        // via wasmtime_caller_export_get("memory").  Hook modules often define
        // their own memory section without a corresponding export entry.
        auto patched = ensureMemoryExported(wasm, len);

        // ── Compile module ─────────────────────────────────────────────────
        wasmtime_module_t* mod = nullptr;
        {
            wasmtime_error_t* err = wasmtime_module_new(
                engine, patched.data(), patched.size(), &mod);
            if (auto msg = wasmtimeError(err))
                return {false, 0, "Wasmtime compile failed: " + *msg};
        }

        // ── Create store + context ─────────────────────────────────────────
        wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
        if (!store)
        {
            wasmtime_module_delete(mod);
            return {false, 0, "Could not create Wasmtime store"};
        }
        wasmtime_context_t* storeCtx = wasmtime_store_context(store);

        // Set initial fuel (consensus-fixed)
        {
            wasmtime_error_t* err =
                wasmtime_context_set_fuel(storeCtx, kWasmtimeInitialFuel);
            if (auto msg = wasmtimeError(err))
            {
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Could not set fuel: " + *msg};
            }
        }

        // ── Per-execution termination state ───────────────────────────────
        ExecState execState;

        // ── Build bridge data (must stay stable in memory) ─────────────────
        std::vector<BridgeData> bridgeData;
        bridgeData.reserve(imports.size());

        // ── Create linker ──────────────────────────────────────────────────
        wasmtime_linker_t* linker = wasmtime_linker_new(engine);

        static const char kEnvModule[] = "env";
        static const size_t kEnvModuleLen = 3;  // strlen("env")

        for (auto const& decl : imports)
        {
            // Feature gate: skip if this API is behind a disabled Amendment
            if (decl.featureGate && !(*decl.featureGate).isZero() &&
                !rules.enabled(*decl.featureGate))
                continue;

            bridgeData.push_back({&decl, &ctx, &execState});

            wasm_functype_t* ft = buildFuncType(decl.params, decl.result);

            // NOTE: wasmtime_linker_define_func takes a raw `data` pointer
            // but does NOT call the finalizer per-call (only when the linker
            // is deleted or shadowed). Since bridgeData is alive for the
            // duration of the execution, this is safe.
            wasmtime_error_t* err = wasmtime_linker_define_func(
                linker,
                kEnvModule,
                kEnvModuleLen,
                decl.name,
                std::strlen(decl.name),
                ft,
                bridgeFn,
                &bridgeData.back(),
                nullptr);

            wasm_functype_delete(ft);

            if (auto msg = wasmtimeError(err))
            {
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Linker define_func failed: " + *msg};
            }
        }

        // ── Define the "table" import (funcref, min=10, max=20) ────────────
        {
            wasm_limits_t tableLimits = {10, 20};
            wasm_tabletype_t* tt = wasm_tabletype_new(
                wasm_valtype_new(WASM_FUNCREF), &tableLimits);

            wasmtime_table_t tbl;
            wasmtime_val_t initVal;
            initVal.kind = WASMTIME_FUNCREF;
            wasmtime_funcref_set_null(&initVal.of.funcref);

            wasmtime_error_t* err =
                wasmtime_table_new(storeCtx, tt, &initVal, &tbl);
            wasm_tabletype_delete(tt);

            if (auto msg = wasmtimeError(err))
            {
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Table creation failed: " + *msg};
            }

            wasmtime_extern_t ext;
            ext.kind = WASMTIME_EXTERN_TABLE;
            ext.of.table = tbl;

            err = wasmtime_linker_define(
                linker, storeCtx, kEnvModule, kEnvModuleLen, "table", 5, &ext);

            if (auto msg = wasmtimeError(err))
            {
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Linker define table failed: " + *msg};
            }
        }

        // ── Define the "memory" import (min=1, max=1 pages) ───────────────
        // This is provided for modules that import their memory from "env".
        // Modules with own memory sections (Section 5) ignore this definition
        // and use their own memory instead (which ensureMemoryExported() will
        // have made accessible via a "memory" export).
        {
            wasm_memorytype_t* mt;
            {
                wasmtime_error_t* err = wasmtime_memorytype_new(
                    1,      // min pages
                    true,   // max_present
                    1,      // max pages
                    false,  // is_64
                    false,  // shared
                    0,      // page_size_log2 (0 = default 64KiB)
                    &mt);
                if (auto msg = wasmtimeError(err))
                {
                    wasmtime_linker_delete(linker);
                    wasmtime_store_delete(store);
                    wasmtime_module_delete(mod);
                    return {false, 0, "Memory type creation failed: " + *msg};
                }
            }

            wasmtime_memory_t mem;
            wasmtime_error_t* err = wasmtime_memory_new(storeCtx, mt, &mem);
            wasm_memorytype_delete(mt);

            if (auto msg = wasmtimeError(err))
            {
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Memory creation failed: " + *msg};
            }

            wasmtime_extern_t ext;
            ext.kind = WASMTIME_EXTERN_MEMORY;
            ext.of.memory = mem;

            err = wasmtime_linker_define(
                linker, storeCtx, kEnvModule, kEnvModuleLen, "memory", 6, &ext);

            if (auto msg = wasmtimeError(err))
            {
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Linker define memory failed: " + *msg};
            }
        }

        JLOG(j.trace()) << "HookInfo[" << ctx.result.account << "-"
                        << ctx.result.otxnAccount
                        << "]: creating wasmtime instance";

        // ── Instantiate ───────────────────────────────────────────────────
        wasmtime_instance_t instance;
        {
            wasm_trap_t* trap = nullptr;
            wasmtime_error_t* err = wasmtime_linker_instantiate(
                linker, storeCtx, mod, &instance, &trap);

            if (auto msg = wasmtimeError(err))
            {
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {false, 0, "Instantiation error: " + *msg};
            }
            if (trap)
            {
                auto msg = wasmtimeTrap(trap);
                wasmtime_linker_delete(linker);
                wasmtime_store_delete(store);
                wasmtime_module_delete(mod);
                return {
                    false,
                    0,
                    "Instantiation trap: " + (msg ? *msg : "unknown")};
            }
        }

        // ── Look up "hook" or "cbak" export ───────────────────────────────
        const char* funcName = isCallback ? "cbak" : "hook";
        size_t funcNameLen = isCallback ? 4 : 4;

        wasmtime_extern_t funcExtern;
        if (!wasmtime_instance_export_get(
                storeCtx, &instance, funcName, funcNameLen, &funcExtern) ||
            funcExtern.kind != WASMTIME_EXTERN_FUNC)
        {
            wasmtime_linker_delete(linker);
            wasmtime_store_delete(store);
            wasmtime_module_delete(mod);
            return {
                false,
                0,
                std::string("WASM export '") + funcName + "' not found"};
        }

        // ── Call hook/cbak with wasmParam ──────────────────────────────────
        wasmtime_val_t callArgs[1];
        callArgs[0].kind = WASMTIME_I32;
        callArgs[0].of.i32 = (int32_t)wasmParam;

        wasmtime_val_t callResults[1];
        wasm_trap_t* callTrap = nullptr;

        wasmtime_error_t* callErr = wasmtime_func_call(
            storeCtx,
            &funcExtern.of.func,
            callArgs,
            1,
            callResults,
            1,
            &callTrap);

        // Capture fuel consumed before cleaning up
        uint64_t fuelRemaining = 0;
        wasmtime_context_get_fuel(storeCtx, &fuelRemaining);
        uint64_t instructionCount = 0;
        if (kWasmtimeInitialFuel >= fuelRemaining)
            instructionCount = kWasmtimeInitialFuel - fuelRemaining;

        wasmtime_linker_delete(linker);
        wasmtime_store_delete(store);
        wasmtime_module_delete(mod);

        // ── Interpret result ───────────────────────────────────────────────
        //
        // When a host callback returns a wasm_trap_t* to signal accept() or
        // rollback(), Wasmtime propagates that as a wasmtime_error_t (with the
        // trap embedded as the cause) rather than via the wasm_trap_t** output.
        // We therefore check execState.terminated FIRST — before inspecting
        // callErr or callTrap — so that clean hook terminations
        // (accept/rollback) are always reported as ok=true regardless of which
        // output pointer Wasmtime chose to use.

        if (execState.terminated)
        {
            // Hook called accept() or rollback(): clean termination.
            // Free any error/trap resources Wasmtime may have allocated.
            if (callErr)
                wasmtime_error_delete(callErr);
            if (callTrap)
                wasm_trap_delete(callTrap);
            return {true, instructionCount, {}};
        }

        // Programmer error (wrong arg types, mismatched store, etc.)
        if (callErr)
        {
            auto msg = wasmtimeError(callErr);
            return {
                false,
                instructionCount,
                "WASM call error: " + (msg ? *msg : "")};
        }

        if (callTrap)
        {
            // Check for out-of-fuel trap
            wasmtime_trap_code_t code = 0;
            if (wasmtime_trap_code(callTrap, &code) &&
                code == WASMTIME_TRAP_CODE_OUT_OF_FUEL)
            {
                wasm_trap_delete(callTrap);
                return {false, instructionCount, "WASM out of fuel"};
            }

            auto msg = wasmtimeTrap(callTrap);
            return {
                false,
                instructionCount,
                "WASM trap: " + (msg ? *msg : "unknown trap")};
        }

        return {true, instructionCount, {}};
    }
};

}  // anonymous namespace

// ── Factory ──────────────────────────────────────────────────────────────────

std::unique_ptr<IWasmEngine>
makeWasmtimeEngine()
{
    return std::make_unique<WasmtimeEngineImpl>();
}

}  // namespace hook
