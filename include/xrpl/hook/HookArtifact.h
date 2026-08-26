//------------------------------------------------------------------------------
/*
    Canonical Hook artifact classification and QuickJS deployment envelope.

    Legacy C Hooks remain raw WebAssembly. QuickJS bytecode is never deployed
    raw: it is carried in a fixed-width, big-endian XQJS envelope whose exact
    bytes participate in HookHash.
*/
//==============================================================================

#ifndef XRPL_HOOK_HOOKARTIFACT_H_INCLUDED
#define XRPL_HOOK_HOOKARTIFACT_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/basics/Slice.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hook::artifact {

inline constexpr std::size_t identitySize = 32;
inline constexpr std::size_t quickJSHeaderSize = 80;
inline constexpr std::size_t maxCreateCodeSize = 65'535;
inline constexpr std::array<std::uint8_t, 4> quickJSMagic = {
    'X',
    'Q',
    'J',
    'S'};
inline constexpr std::uint8_t quickJSEnvelopeVersion = 1;
inline constexpr std::uint8_t quickJSBytecodeKind = 1;

using Identity = std::array<std::uint8_t, identitySize>;

enum class Kind : std::uint8_t {
    legacyWasm,
    quickJSBytecode,
};

enum class XFLArithmeticProfile {
    none,
    xahauFloatV1,
    nearestEvenV1,
};

enum class Error : std::uint8_t {
    tooLarge,
    tooShort,
    unknownMagic,
    truncatedQuickJSHeader,
    unsupportedEnvelopeVersion,
    unsupportedArtifactKind,
    nonCanonicalHeaderSize,
    unsupportedXFLArithmeticProfile,
    emptyPayload,
    lengthMismatch,
    zeroBytecodeABI,
    zeroRuntimeProfile,
};

struct View
{
    Kind kind;
    std::uint16_t hookApiVersion;
    XFLArithmeticProfile xflArithmeticProfile;
    Identity bytecodeABI;
    Identity runtimeProfile;
    ripple::Slice payload;
};

/** Parse either a legacy raw-Wasm Hook or a canonical QuickJS artifact.

    The returned slices retain no ownership and must not outlive `code`.
*/
ripple::Expected<View, Error>
parse(ripple::Slice code) noexcept;

std::string_view
toString(Error error) noexcept;

extern Identity const quickJSBytecodeABI;
extern Identity const quickJSRuntimeProfile;
extern Identity const quickJSProviderSHA256;
extern std::size_t const quickJSProviderSize;
extern std::uint16_t const quickJSHookApiVersion;
extern std::uint64_t const quickJSInitializationFuel;
extern std::uint64_t const quickJSInvocationFuel;
extern std::string_view const quickJSHostWorkMeter;
extern std::uint64_t const quickJSHostWorkBudget;
extern std::uint64_t const quickJSHostWorkBasePerCall;
extern std::uint64_t const quickJSHostWorkPerAddressedByte;
extern std::string_view const quickJSHostAdapterPolicy;
extern std::uint32_t const quickJSHeapBytes;
extern std::uint32_t const quickJSStackBytes;
extern std::uint32_t const quickJSSerializedObjectMaxBytes;
extern std::uint32_t const quickJSSerializedObjectMaxFields;
extern std::uint32_t const quickJSSerializedObjectMaxScopes;
extern std::uint32_t const quickJSSerializedObjectMaxDepth;
extern std::uint32_t const quickJSProviderMemoryMinimumPages;
extern std::uint32_t const quickJSProviderMemoryMaximumPages;
extern std::uint32_t const quickJSProviderWasmStackBytes;

bool
isCurrentQuickJS(View const& artifact) noexcept;

}  // namespace hook::artifact

#endif  // XRPL_HOOK_HOOKARTIFACT_H_INCLUDED
