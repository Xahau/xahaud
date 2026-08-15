#ifndef XRPLD_APP_HOOK_QUICKJSHOOKRUNTIME_H_INCLUDED
#define XRPLD_APP_HOOK_QUICKJSHOOKRUNTIME_H_INCLUDED

#include <xrpl/basics/Blob.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/hook/HookArtifact.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace hook {

struct HookContext;
class QuickJSProviderRuntime;
using QuickJSRuntimeHandle = std::shared_ptr<QuickJSProviderRuntime const>;

/** Consensus-relevant data needed to retain and execute one runtime profile.

    The profile identity keys the registry, while the bytecode ABI and API
    version prevent an artifact from resolving through a merely compatible
    but historically different provider.
*/
struct QuickJSRuntimeProfile
{
    artifact::Identity bytecodeABI;
    artifact::Identity runtimeProfile;
    artifact::Identity providerSHA256;
    std::size_t providerSize;
    std::uint16_t hookApiVersion;
    std::uint64_t initializationFuel;
    std::uint64_t invocationFuel;
    std::uint64_t hostWorkBudget;
    std::uint64_t hostWorkBasePerCall;
    std::uint64_t hostWorkPerAddressedByte;
    std::string hostWorkMeter;
    std::string hostAdapterPolicy;
    std::uint32_t heapBytes;
    std::uint32_t stackBytes;

    bool
    operator==(QuickJSRuntimeProfile const&) const = default;
};

QuickJSRuntimeProfile
currentQuickJSRuntimeProfile();

/** Verify and compile one provider, retaining it by profile/ABI/API key.

    Provider bytes are hashed only here. Stores, instances, guest memory, and
    HookContext remain callback-scoped and are never retained in the registry.
*/
std::optional<std::string>
registerQuickJSRuntime(
    QuickJSRuntimeProfile profile,
    std::span<std::uint8_t const> provider);

/** Begin verifying/compiling one provider on a background thread.

    Cranelift compilation of the whole provider is CPU-heavy; this moves it
    off the caller's thread so process startup is not serialized behind it.
    The compiled module lives in memory for the life of the process and is
    thrown away with it — no serialization, no on-disk cache, nothing new is
    consensus-visible (the input is still the SHA-verified provider bytes).

    Resolution through findQuickJSRuntime WAITS for a pending registration
    of the same identity, so an artifact arriving before the compile
    finishes blocks briefly instead of failing to resolve. A failed
    registration resolves to an empty handle; its error is retained per
    identity and readable via quickJSRuntimeRegistrationError.
*/
void
launchQuickJSRuntimeRegistration(
    QuickJSRuntimeProfile profile,
    ripple::Blob provider);

/** The retained error from a launched registration, if it failed. */
std::optional<std::string>
quickJSRuntimeRegistrationError(QuickJSRuntimeProfile const& profile);

/** Resolve the exact historical runtime selected by a deployment envelope.

    Waits for a launched registration of the same identity to settle before
    answering, so background compilation is invisible to resolution except
    as startup latency. */
QuickJSRuntimeHandle
findQuickJSRuntime(artifact::View const& artifact);

/** Validate payload bytecode in the exact retained provider.

    Module initialization runs in a bounded, disposable Store with no Hook
    invocation context, so it must be synchronous and host-free. The entry
    points are not invoked. On success, reports whether initialized cbak is
    callable.
*/
std::optional<std::string>
validateQuickJSBytecode(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool& hasCallback);

/** Execute payload-only bytecode with a previously registered runtime. */
void
executeQuickJSBytecode(
    HookContext& hookCtx,
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode,
    bool callback,
    std::uint32_t reserved,
    beast::Journal const& journal);

#ifdef ENABLE_TESTS
struct QuickJSValidationForTests
{
    std::optional<std::string> error;
    bool hasCallback = false;
    std::uint64_t invocationFuelConsumed = 0;
};

/** Validate through the production path while exposing its fuel delta. */
QuickJSValidationForTests
validateQuickJSBytecodeForTests(
    QuickJSRuntimeHandle const& runtime,
    std::span<std::uint8_t const> bytecode);

/** Register the currently generated profile using a test-supplied provider. */
std::optional<std::string>
setQuickJSProviderForTests(ripple::Blob provider);
#endif

}  // namespace hook

#endif  // XRPLD_APP_HOOK_QUICKJSHOOKRUNTIME_H_INCLUDED
