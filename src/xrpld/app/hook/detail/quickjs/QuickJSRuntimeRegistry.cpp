#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSRuntimeInternal.h>
#include <xrpl/hook/HookArtifact.h>
#include <xrpl/protocol/digest.h>
#include <chrono>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

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

// A launched (background) registration, keyed like the registry. The future
// settles when the compile thread has finished registering or failed; the
// launched profile lets a mismatched relaunch be refused, and the retained
// error string keeps a failed identity diagnosable for the life of the
// process. Entries are never erased: launch is at most once per identity.
struct PendingRegistration
{
    QuickJSRuntimeProfile profile;
    std::shared_future<void> settled;
    std::shared_ptr<std::optional<std::string>> error;
};

// One static holds the mutex, the registry, and the pending map so their
// lifetimes do not depend on which accessor a process happens to touch
// first. Nothing here joins a compile thread: the thread is detached, its
// future is promise-backed, and the process must await settlement before
// shutdown (see the header contract).
struct RegistryState
{
    std::mutex mutex;
    std::map<RuntimeKey, QuickJSRuntimeHandle> runtimes;
    std::map<RuntimeKey, PendingRegistration> pending;
};

RegistryState&
registryState()
{
    static RegistryState state;
    return state;
}

// Copy a pending entry under the registry lock, or nothing.
std::optional<PendingRegistration>
pendingFor(RuntimeKey const& key)
{
    auto& state = registryState();
    std::lock_guard lock{state.mutex};
    auto const position = state.pending.find(key);
    if (position == state.pending.end())
        return std::nullopt;
    return position->second;
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
        profile.heapBytes == 0 || profile.stackBytes == 0 ||
        profile.serializedObjectMaxBytes == 0 ||
        profile.serializedObjectMaxFields == 0 ||
        profile.serializedObjectMaxScopes == 0 ||
        profile.serializedObjectMaxDepth == 0 ||
        profile.providerMemoryMinimumPages == 0 ||
        profile.providerMemoryMaximumPages < profile.providerMemoryMinimumPages)
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
        .stackBytes = artifact::quickJSStackBytes,
        .serializedObjectMaxBytes = artifact::quickJSSerializedObjectMaxBytes,
        .serializedObjectMaxFields = artifact::quickJSSerializedObjectMaxFields,
        .serializedObjectMaxScopes = artifact::quickJSSerializedObjectMaxScopes,
        .serializedObjectMaxDepth = artifact::quickJSSerializedObjectMaxDepth,
        .providerMemoryMinimumPages =
            artifact::quickJSProviderMemoryMinimumPages,
        .providerMemoryMaximumPages =
            artifact::quickJSProviderMemoryMaximumPages};
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

    auto& state = registryState();
    std::lock_guard lock{state.mutex};
    auto const [position, inserted] =
        state.runtimes.emplace(keyFor(profile), std::move(candidate));
    if (!inserted && position->second->profile != profile)
        return "runtime profile identity is already registered with different "
               "consensus limits or host policy";
    return std::nullopt;
}

std::optional<std::string>
launchQuickJSRuntimeRegistration(
    QuickJSRuntimeProfile profile,
    ripple::Blob provider)
{
    auto const key = keyFor(profile);
    auto error = std::make_shared<std::optional<std::string>>();
    auto settled = std::make_shared<std::promise<void>>();
    {
        auto& state = registryState();
        std::lock_guard lock{state.mutex};
        if (auto const registered = state.runtimes.find(key);
            registered != state.runtimes.end())
        {
            if (registered->second->profile == profile)
                return std::nullopt;
            return "runtime profile identity is already registered with "
                   "different consensus limits or host policy";
        }
        if (auto const launched = state.pending.find(key);
            launched != state.pending.end())
        {
            if (launched->second.profile != profile)
                return "runtime profile identity was already launched with "
                       "different consensus limits or host policy";
            // Same identity, same limits: still compiling, already
            // registered, or terminally failed. Only the failure is loud.
            if (launched->second.settled.wait_for(std::chrono::seconds{0}) ==
                std::future_status::ready)
                return *launched->second.error;
            return std::nullopt;
        }
        // Reserve the entry before any thread exists, so nothing below can
        // wait on the compile while holding the registry lock, and so a
        // find/await that observes the entry always has a valid future.
        state.pending.emplace(
            key,
            PendingRegistration{profile, settled->get_future().share(), error});
    }

    try
    {
        std::thread{[profile = std::move(profile),
                     provider = std::move(provider),
                     error,
                     settled]() mutable {
            try
            {
                *error = registerQuickJSRuntime(profile, provider);
            }
            catch (std::exception const& e)
            {
                *error =
                    std::string{"provider registration threw: "} + e.what();
            }
            catch (...)
            {
                *error = "provider registration threw an unknown exception";
            }
            settled->set_value();
        }}.detach();
    }
    catch (std::exception const& e)
    {
        // No thread runs the lambda when its constructor throws; settle the
        // reserved entry so waiters see the failure instead of blocking.
        *error = std::string{"could not start the provider compile thread: "} +
            e.what();
        settled->set_value();
        return *error;
    }
    return std::nullopt;
}

std::optional<std::string>
awaitQuickJSRuntimeRegistration(QuickJSRuntimeProfile const& profile)
{
    auto const key = keyFor(profile);
    // Wait outside the lock: the compile thread needs it to publish.
    if (auto pending = pendingFor(key))
        pending->settled.wait();

    auto& state = registryState();
    std::lock_guard lock{state.mutex};
    auto const registered = state.runtimes.find(key);
    if (registered != state.runtimes.end() &&
        registered->second->profile == profile)
        return std::nullopt;
    if (auto const launched = state.pending.find(key);
        launched != state.pending.end() && *launched->second.error)
        return *launched->second.error;
    if (registered != state.runtimes.end())
        return "runtime profile identity is registered with different "
               "consensus limits or host policy";
    return "no runtime was registered or launched for this profile identity";
}

QuickJSRuntimeHandle
findQuickJSRuntime(artifact::View const& artifact)
{
    if (artifact.kind != artifact::Kind::quickJSBytecode)
        return {};
    auto const key = keyFor(artifact);
    auto& state = registryState();
    {
        std::lock_guard lock{state.mutex};
        auto const position = state.runtimes.find(key);
        if (position != state.runtimes.end())
            return position->second;
    }
    // Not registered yet: a launched compile for this identity may still be
    // running. Wait outside the lock (the compile thread needs the lock to
    // publish), then answer from the settled registry state.
    if (auto pending = pendingFor(key))
        pending->settled.wait();
    std::lock_guard lock{state.mutex};
    auto const position = state.runtimes.find(key);
    return position == state.runtimes.end() ? QuickJSRuntimeHandle{}
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
