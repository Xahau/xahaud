#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSPROVIDERSESSION_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSPROVIDERSESSION_H_INCLUDED

#include <xrpld/app/hook/detail/quickjs/QuickJSRuntimeInternal.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <wasmtime.h>

namespace hook::quickjs {

enum class ProviderStage : std::uint8_t {
    none,
    storeCreation,
    initializationFuel,
    importRegistration,
    instantiation,
    memoryExport,
    reactorInitialization,
    quickJSInitialization,
    memoryLimit,
    stackLimit,
    invocationFuel,
    bytecodeInput,
    bytecodeAllocation,
    bytecodeCopy
};

std::string_view
providerStageName(ProviderStage stage) noexcept;

/** One callback-scoped instance of a retained QuickJS provider.

    The session owns its Store and Linker. It may retain the Wasmtime memory
    handle, but it never retains a native data pointer, size, span, or
    HookGuestMemory view across a Wasm call.
*/
class ProviderSession
{
public:
    using LinkerPtr =
        std::unique_ptr<wasmtime_linker_t, decltype(&wasmtime_linker_delete)>;
    using StorePtr =
        std::unique_ptr<wasmtime_store_t, decltype(&wasmtime_store_delete)>;

    static std::unique_ptr<ProviderSession>
    create(
        QuickJSProviderRuntime const& runtime,
        void* storeData,
        ProviderStage& stage,
        std::string& detail);

    ProviderSession(ProviderSession const&) = delete;
    ProviderSession&
    operator=(ProviderSession const&) = delete;

    bool
    initialize(ProviderStage& stage, std::string& detail);

    bool
    resetInvocationFuel(ProviderStage& stage, std::string& detail);

    std::optional<std::uint32_t>
    allocateAndCopy(
        std::span<std::uint8_t const> bytes,
        ProviderStage& stage,
        std::string& detail);

    bool
    callExport(
        char const* name,
        wasmtime_val_t const* arguments,
        std::size_t argumentCount,
        wasmtime_val_t* results,
        std::size_t resultCount,
        std::string& detail);

    std::string
    readDiagnostic() noexcept;

    std::optional<std::uint64_t>
    invocationFuelConsumed() const noexcept;

private:
    ProviderSession(
        QuickJSProviderRuntime const& runtime,
        StorePtr&& store,
        LinkerPtr&& linker,
        wasmtime_instance_t instance,
        wasmtime_memory_t memory) noexcept;

    QuickJSProviderRuntime const& runtime_;
    StorePtr store_;
    LinkerPtr linker_;
    wasmtime_context_t* context_;
    wasmtime_instance_t instance_;
    wasmtime_memory_t memory_;
};

}  // namespace hook::quickjs

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSPROVIDERSESSION_H_INCLUDED
