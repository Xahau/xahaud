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

extern std::span<std::string_view const> const providerImports;
extern std::span<ProviderImportSignature const> const providerImportSignatures;
extern std::span<NativeImportSignature const> const nativeImportSignatures;
extern std::string_view const nativeABISourceRepository;
extern std::string_view const nativeABISourceCommit;
extern std::string_view const nativeABISourcePath;
extern std::string_view const nativeABISHA256;
extern std::size_t const nativeABICatalogueCount;

}  // namespace hook::artifact::generated

#endif
