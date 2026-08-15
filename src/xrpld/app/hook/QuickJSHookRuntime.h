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

    Returns immediately. nullopt means the identity is registered, still
    compiling, or was just started; otherwise the reason the launch was
    refused: the identity is already registered or launched with a
    different profile, an earlier launch of it failed (the retained error;
    failure is terminal, there is no relaunch), or no thread could be
    started. A same-profile relaunch is a no-op.

    Contract for callers:
    - launch must return before any findQuickJSRuntime or await for the
      identity; a find that races the launch answers as if nothing was
      launched.
    - the compile thread is detached and nothing joins it at exit, so the
      process must awaitQuickJSRuntimeRegistration before it shuts down;
      the intended shape is: Application setup launches early, awaits
      before start, and treats an error as fatal — a node that cannot
      compile the sealed provider must not stay up, or it will disagree
      with its peers on every QuickJS artifact.
*/
std::optional<std::string>
launchQuickJSRuntimeRegistration(
    QuickJSRuntimeProfile profile,
    ripple::Blob provider);

/** Wait for a launched registration of this identity to settle.

    Returns nullopt once the identity is registered with this profile —
    by the launched compile or by a synchronous registerQuickJSRuntime —
    otherwise why it is not: the retained compile failure, a registration
    of the same identity with a different profile, or, when nothing was
    launched or registered, a message saying so. A startup that forgets to
    launch therefore does not look healthy.
*/
std::optional<std::string>
awaitQuickJSRuntimeRegistration(QuickJSRuntimeProfile const& profile);

/** Resolve the exact historical runtime selected by a deployment envelope.

    Waits for a launched registration of the same identity to settle before
    answering, so background compilation is invisible to resolution except
    as latency; the startup barrier is awaitQuickJSRuntimeRegistration, and
    this wait is only the safety net for a caller that races a
    still-compiling process. */
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
