//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
*/
//==============================================================================

#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpl/hook/HookArtifact.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace hook::artifact {

Identity const quickJSBytecodeABI = generated::bytecodeABI;
Identity const quickJSRuntimeProfile = generated::runtimeProfile;
Identity const quickJSProviderSHA256 = generated::providerSHA256;
std::size_t const quickJSProviderSize = generated::providerSize;
std::uint16_t const quickJSHookApiVersion = generated::hookApiVersion;
std::uint64_t const quickJSInitializationFuel = generated::initializationFuel;
std::uint64_t const quickJSInvocationFuel = generated::invocationFuel;
std::string_view const quickJSHostWorkMeter = generated::hostWorkMeter;
std::uint64_t const quickJSHostWorkBudget = generated::hostWorkBudget;
std::uint64_t const quickJSHostWorkBasePerCall = generated::hostWorkBasePerCall;
std::uint64_t const quickJSHostWorkPerAddressedByte =
    generated::hostWorkPerAddressedByte;
std::string_view const quickJSHostAdapterPolicy = generated::hostAdapterPolicy;
std::uint32_t const quickJSHeapBytes = generated::heapBytes;
std::uint32_t const quickJSStackBytes = generated::stackBytes;
std::uint32_t const quickJSSerializedObjectMaxBytes =
    generated::serializedObjectMaxBytes;
std::uint32_t const quickJSSerializedObjectMaxFields =
    generated::serializedObjectMaxFields;
std::uint32_t const quickJSSerializedObjectMaxScopes =
    generated::serializedObjectMaxScopes;
std::uint32_t const quickJSSerializedObjectMaxDepth =
    generated::serializedObjectMaxDepth;
std::uint32_t const quickJSProviderMemoryMinimumPages =
    generated::providerMemoryMinimumPages;
std::uint32_t const quickJSProviderMemoryMaximumPages =
    generated::providerMemoryMaximumPages;
std::uint32_t const quickJSProviderWasmStackBytes = generated::wasmStackBytes;

namespace {

constexpr std::array<std::uint8_t, 4> wasmMagic = {0x00, 0x61, 0x73, 0x6D};

bool
startsWith(ripple::Slice bytes, std::array<std::uint8_t, 4> const& prefix)
{
    return bytes.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

std::uint16_t
readU16BE(std::uint8_t const* bytes)
{
    return (static_cast<std::uint16_t>(bytes[0]) << 8) |
        static_cast<std::uint16_t>(bytes[1]);
}

std::uint32_t
readU32BE(std::uint8_t const* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) |
        static_cast<std::uint32_t>(bytes[3]);
}

bool
isZero(Identity const& identity)
{
    return std::all_of(
        identity.begin(), identity.end(), [](std::uint8_t value) {
            return value == 0;
        });
}

}  // namespace

bool
isCurrentQuickJS(View const& artifact) noexcept
{
    return artifact.kind == Kind::quickJSBytecode &&
        artifact.hookApiVersion == quickJSHookApiVersion &&
        artifact.bytecodeABI == quickJSBytecodeABI &&
        artifact.runtimeProfile == quickJSRuntimeProfile;
}

ripple::Expected<View, Error>
parse(ripple::Slice code) noexcept
{
    if (code.size() > maxCreateCodeSize)
        return ripple::Unexpected(Error::tooLarge);
    if (code.size() < 4)
        return ripple::Unexpected(Error::tooShort);

    if (startsWith(code, wasmMagic))
    {
        return View{
            .kind = Kind::legacyWasm,
            .hookApiVersion = 0,
            .bytecodeABI = {},
            .runtimeProfile = {},
            .payload = code};
    }

    if (!startsWith(code, quickJSMagic))
        return ripple::Unexpected(Error::unknownMagic);
    if (code.size() < quickJSHeaderSize)
        return ripple::Unexpected(Error::truncatedQuickJSHeader);

    auto const* bytes = code.data();
    if (bytes[4] != quickJSEnvelopeVersion)
        return ripple::Unexpected(Error::unsupportedEnvelopeVersion);
    if (bytes[5] != quickJSBytecodeKind)
        return ripple::Unexpected(Error::unsupportedArtifactKind);
    if (readU16BE(bytes + 6) != quickJSHeaderSize)
        return ripple::Unexpected(Error::nonCanonicalHeaderSize);
    if (readU16BE(bytes + 10) != 0)
        return ripple::Unexpected(Error::nonZeroReserved);

    auto const payloadSize = readU32BE(bytes + 12);
    if (payloadSize == 0)
        return ripple::Unexpected(Error::emptyPayload);
    if (payloadSize != code.size() - quickJSHeaderSize)
        return ripple::Unexpected(Error::lengthMismatch);

    Identity bytecodeABI;
    std::copy_n(bytes + 16, identitySize, bytecodeABI.begin());
    if (isZero(bytecodeABI))
        return ripple::Unexpected(Error::zeroBytecodeABI);

    Identity runtimeProfile;
    std::copy_n(bytes + 48, identitySize, runtimeProfile.begin());
    if (isZero(runtimeProfile))
        return ripple::Unexpected(Error::zeroRuntimeProfile);

    return View{
        .kind = Kind::quickJSBytecode,
        .hookApiVersion = readU16BE(bytes + 8),
        .bytecodeABI = bytecodeABI,
        .runtimeProfile = runtimeProfile,
        .payload = ripple::Slice{bytes + quickJSHeaderSize, payloadSize}};
}

std::string_view
toString(Error error) noexcept
{
    switch (error)
    {
        case Error::tooLarge:
            return "artifact exceeds the sfCreateCode limit";
        case Error::tooShort:
            return "artifact is too short to identify";
        case Error::unknownMagic:
            return "artifact has unknown magic";
        case Error::truncatedQuickJSHeader:
            return "QuickJS artifact header is truncated";
        case Error::unsupportedEnvelopeVersion:
            return "QuickJS envelope version is unsupported";
        case Error::unsupportedArtifactKind:
            return "QuickJS artifact kind is unsupported";
        case Error::nonCanonicalHeaderSize:
            return "QuickJS v1 header size is non-canonical";
        case Error::nonZeroReserved:
            return "QuickJS v1 reserved bytes are nonzero";
        case Error::emptyPayload:
            return "QuickJS artifact payload is empty";
        case Error::lengthMismatch:
            return "QuickJS payload length does not match sfCreateCode";
        case Error::zeroBytecodeABI:
            return "QuickJS bytecode ABI identity is zero";
        case Error::zeroRuntimeProfile:
            return "QuickJS runtime profile identity is zero";
    }
    return "unknown Hook artifact error";
}

}  // namespace hook::artifact
