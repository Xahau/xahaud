#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJSPROVIDERPROFILE_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJSPROVIDERPROFILE_H_INCLUDED

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace hook::artifact::generated {

struct ProviderImportSignature
{
    std::string_view module;
    std::string_view name;
    std::string_view parameters;
    std::string_view results;
};

struct ProviderExportSignature
{
    std::string_view kind;
    std::string_view name;
    std::string_view parameters;
    std::string_view results;
    std::uint32_t minimumPages;
    std::uint32_t maximumPages;
    bool memory64;
    bool shared;
};

struct NativeImportSignature
{
    std::string_view name;
    std::string_view result;
    std::string_view parameters;
    std::string_view amendment;
};

extern std::array<std::uint8_t, 32> const providerSHA256;
extern std::array<std::uint8_t, 32> const bytecodeABI;
extern std::array<std::uint8_t, 32> const runtimeProfile;
extern std::string_view const providerProduct;
extern std::string_view const providerManifestSHA256;
extern std::size_t const providerSize;
extern std::uint16_t const hookApiVersion;
extern std::uint64_t const initializationFuel;
extern std::uint64_t const invocationFuel;
extern std::string_view const hostWorkMeter;
extern std::uint64_t const hostWorkBudget;
extern std::uint64_t const hostWorkBasePerCall;
extern std::uint64_t const hostWorkPerAddressedByte;
extern std::string_view const hostAdapterPolicy;
extern std::uint32_t const heapBytes;
extern std::uint32_t const stackBytes;
extern std::uint32_t const wasmStackBytes;
extern std::uint32_t const serializedObjectMaxBytes;
extern std::uint32_t const serializedObjectMaxFields;
extern std::uint32_t const serializedObjectMaxScopes;
extern std::uint32_t const serializedObjectMaxDepth;
extern std::uint32_t const providerMemoryMinimumPages;
extern std::uint32_t const providerMemoryMaximumPages;
extern bool const providerMemory64;
extern bool const providerMemoryShared;
extern std::string_view const javascriptBroadDeclarationSHA256;
extern std::string_view const javascriptExactV1DeclarationSHA256;
extern std::string_view const javascriptSurfaceSHA256;
extern std::string_view const javascriptXFLProfileLedgerSHA256;
extern std::string_view const javascriptAPIArtifactManifestSHA256;
extern std::uint8_t const xqjsEnvelopeVersion;
extern std::uint16_t const xflArithmeticProfileNone;
extern std::uint16_t const xflArithmeticProfileXahauFloatV1;
extern std::uint16_t const xflArithmeticProfileNearestEvenV1;
extern std::uint32_t const moduleValidationLayoutVersion;
extern std::int32_t const moduleValidationFailureSentinel;
extern std::uint32_t const moduleValidationMainBit;
extern std::uint32_t const moduleValidationCallbackBit;
extern std::uint32_t const moduleValidationEntryMask;
extern std::uint32_t const moduleValidationReservedMask;
extern std::uint32_t const moduleValidationProfileMask;
extern std::uint32_t const moduleValidationProfileShift;
extern std::uint32_t const moduleValidationVersionMask;
extern std::uint32_t const moduleValidationVersionShift;

extern std::span<std::string_view const> const providerImports;
extern std::span<std::string_view const> const providerExports;
extern std::span<ProviderImportSignature const> const providerImportSignatures;
extern std::span<ProviderExportSignature const> const providerExportSignatures;
extern std::span<NativeImportSignature const> const nativeImportSignatures;
extern std::string_view const nativeABISourceRepository;
extern std::string_view const nativeABISourceCommit;
extern std::string_view const nativeABISourcePath;
extern std::string_view const nativeABISHA256;
extern std::size_t const nativeABICatalogueCount;

}  // namespace hook::artifact::generated

#endif
