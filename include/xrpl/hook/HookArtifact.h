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
    'X', 'Q', 'J', 'S'};
inline constexpr std::uint8_t quickJSEnvelopeVersion = 1;
inline constexpr std::uint8_t quickJSBytecodeKind = 1;

using Identity = std::array<std::uint8_t, identitySize>;

enum class Kind : std::uint8_t {
    legacyWasm,
    quickJSBytecode,
};

enum class Error : std::uint8_t {
    tooLarge,
    tooShort,
    unknownMagic,
    truncatedQuickJSHeader,
    unsupportedEnvelopeVersion,
    unsupportedArtifactKind,
    nonCanonicalHeaderSize,
    nonZeroReserved,
    emptyPayload,
    lengthMismatch,
    zeroBytecodeABI,
    zeroRuntimeProfile,
};

struct View
{
    Kind kind;
    std::uint16_t hookApiVersion;
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

// The current transaction test fixture is deliberately not a production
// runtime profile. Keeping one exact identity makes unsupported-profile tests
// real while the Wasmtime/no-WASI profile is being built.
inline constexpr Identity prototypeBytecodeABI = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
inline constexpr Identity prototypeRuntimeProfile = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};

inline bool
isPrototypeQuickJS(View const& artifact) noexcept
{
    return artifact.kind == Kind::quickJSBytecode &&
        artifact.bytecodeABI == prototypeBytecodeABI &&
        artifact.runtimeProfile == prototypeRuntimeProfile;
}

}  // namespace hook::artifact

#endif  // XRPL_HOOK_HOOKARTIFACT_H_INCLUDED
