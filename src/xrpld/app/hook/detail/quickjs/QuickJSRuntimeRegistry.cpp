#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSRuntimeInternal.h>
#include <xrpl/hook/HookArtifact.h>
#include <xrpl/protocol/digest.h>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace hook {
namespace {

using RuntimeKey =
    std::tuple<std::uint16_t, artifact::Identity, artifact::Identity>;

RuntimeKey
keyFor(QuickJSRuntimeProfile const& profile)
{
    return {
        profile.hookApiVersion, profile.bytecodeABI, profile.runtimeProfile};
}

RuntimeKey
keyFor(artifact::View const& artifact)
{
    return {
        artifact.hookApiVersion, artifact.bytecodeABI, artifact.runtimeProfile};
}

std::mutex&
runtimeRegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<RuntimeKey, QuickJSRuntimeHandle>&
runtimeRegistry()
{
    static std::map<RuntimeKey, QuickJSRuntimeHandle> runtimes;
    return runtimes;
}

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

QuickJSRuntimeHandle
compileRuntime(
    QuickJSRuntimeProfile profile,
    std::span<std::uint8_t const> provider,
    std::string& error)
{
    if (profile.initializationFuel == 0 || profile.invocationFuel == 0 ||
        profile.hostWorkBudget == 0 || profile.hostWorkPerAddressedByte == 0 ||
        profile.heapBytes == 0 || profile.stackBytes == 0)
    {
        error = "runtime profile has an invalid zero execution limit";
        return {};
    }
    auto const* hostPolicy =
        quickjs::findHostAdapterPolicy(profile.hostAdapterPolicy);
    if (!hostPolicy)
    {
        error = "runtime profile names an unknown host-adapter policy";
        return {};
    }
    if (!hostPolicy->complete())
    {
        error = "runtime profile host policy has a missing required binding";
        return {};
    }
    if (profile.hostWorkMeter != hostPolicy->hostWorkMeter)
    {
        error =
            "runtime profile names the wrong host-work meter for its "
            "host policy";
        return {};
    }
    if (provider.size() != profile.providerSize)
    {
        error = "provider size does not match its runtime profile";
        return {};
    }
    ripple::sha256_hasher hasher;
    hasher(provider.data(), provider.size());
    if (static_cast<ripple::sha256_hasher::result_type>(hasher) !=
        profile.providerSHA256)
    {
        error = "provider SHA-256 does not match its runtime profile";
        return {};
    }

    auto* config = wasm_config_new();
    if (!config)
    {
        error = "could not allocate Wasmtime configuration";
        return {};
    }
    wasmtime_config_consume_fuel_set(config, true);
    wasmtime_config_cranelift_nan_canonicalization_set(config, true);
    wasmtime_config_wasm_threads_set(config, false);
    wasmtime_config_wasm_relaxed_simd_set(config, false);
    wasmtime_config_wasm_memory64_set(config, false);
    wasmtime_config_wasm_multi_memory_set(config, false);
    wasmtime_config_wasm_tail_call_set(config, false);

    QuickJSEnginePtr engine{
        wasm_engine_new_with_config(config), wasm_engine_delete};
    if (!engine)
    {
        error = "could not create the pinned Wasmtime engine";
        return {};
    }

    wasmtime_module_t* module = nullptr;
    if (auto* compileError = wasmtime_module_new(
            engine.get(), provider.data(), provider.size(), &module))
    {
        error = takeError(compileError);
        return {};
    }

    return std::make_shared<QuickJSProviderRuntime>(
        std::move(profile),
        *hostPolicy,
        std::move(engine),
        QuickJSModulePtr{module, wasmtime_module_delete});
}

}  // namespace

QuickJSRuntimeProfile
currentQuickJSRuntimeProfile()
{
    return {
        .bytecodeABI = artifact::quickJSBytecodeABI,
        .runtimeProfile = artifact::quickJSRuntimeProfile,
        .providerSHA256 = artifact::quickJSProviderSHA256,
        .providerSize = artifact::quickJSProviderSize,
        .hookApiVersion = artifact::quickJSHookApiVersion,
        .initializationFuel = artifact::quickJSInitializationFuel,
        .invocationFuel = artifact::quickJSInvocationFuel,
        .hostWorkBudget = artifact::quickJSHostWorkBudget,
        .hostWorkBasePerCall = artifact::quickJSHostWorkBasePerCall,
        .hostWorkPerAddressedByte = artifact::quickJSHostWorkPerAddressedByte,
        .hostWorkMeter = std::string{artifact::quickJSHostWorkMeter},
        .hostAdapterPolicy = std::string{artifact::quickJSHostAdapterPolicy},
        .heapBytes = artifact::quickJSHeapBytes,
        .stackBytes = artifact::quickJSStackBytes};
}

std::optional<std::string>
registerQuickJSRuntime(
    QuickJSRuntimeProfile profile,
    std::span<std::uint8_t const> provider)
{
    std::string error;
    auto candidate = compileRuntime(profile, provider, error);
    if (!candidate)
        return error;

    std::lock_guard lock{runtimeRegistryMutex()};
    auto const [position, inserted] =
        runtimeRegistry().emplace(keyFor(profile), std::move(candidate));
    if (!inserted && position->second->profile != profile)
        return "runtime profile identity is already registered with different "
               "consensus limits or host policy";
    return std::nullopt;
}

QuickJSRuntimeHandle
findQuickJSRuntime(artifact::View const& artifact)
{
    if (artifact.kind != artifact::Kind::quickJSBytecode)
        return {};
    std::lock_guard lock{runtimeRegistryMutex()};
    auto const position = runtimeRegistry().find(keyFor(artifact));
    return position == runtimeRegistry().end() ? QuickJSRuntimeHandle{}
                                               : position->second;
}

#ifdef ENABLE_TESTS
std::optional<std::string>
setQuickJSProviderForTests(ripple::Blob provider)
{
    return registerQuickJSRuntime(currentQuickJSRuntimeProfile(), provider);
}
#endif

}  // namespace hook
