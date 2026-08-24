//------------------------------------------------------------------------------
/*
    Tests for the consensus-visible Hook deployment envelope.
*/
//==============================================================================

#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/hook/HookArtifact.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ripple::test {
namespace {

std::vector<std::uint8_t>
quickJSArtifact(std::vector<std::uint8_t> const& payload = {'a', 'b', 'c'})
{
    std::vector<std::uint8_t> result(
        hook::artifact::quickJSHeaderSize + payload.size(), 0);
    std::copy(
        hook::artifact::quickJSMagic.begin(),
        hook::artifact::quickJSMagic.end(),
        result.begin());
    result[4] = hook::artifact::quickJSEnvelopeVersion;
    result[5] = hook::artifact::quickJSBytecodeKind;
    result[6] = 0;
    result[7] = hook::artifact::quickJSHeaderSize;
    result[8] = 0;
    result[9] = 1;
    auto const length = static_cast<std::uint32_t>(payload.size());
    result[12] = static_cast<std::uint8_t>(length >> 24);
    result[13] = static_cast<std::uint8_t>(length >> 16);
    result[14] = static_cast<std::uint8_t>(length >> 8);
    result[15] = static_cast<std::uint8_t>(length);
    std::copy(
        hook::artifact::quickJSBytecodeABI.begin(),
        hook::artifact::quickJSBytecodeABI.end(),
        result.begin() + 16);
    std::copy(
        hook::artifact::quickJSRuntimeProfile.begin(),
        hook::artifact::quickJSRuntimeProfile.end(),
        result.begin() + 48);
    std::copy(payload.begin(), payload.end(), result.begin() + 80);
    return result;
}

}  // namespace

class HookArtifact_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        testcase("Legacy Wasm classification");
        std::array<std::uint8_t, 8> const wasm = {
            0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
        auto legacy = hook::artifact::parse(makeSlice(wasm));
        BEAST_EXPECT(legacy);
        if (legacy)
        {
            BEAST_EXPECT(legacy->kind == hook::artifact::Kind::legacyWasm);
            BEAST_EXPECT(legacy->hookApiVersion == 0);
            BEAST_EXPECT(legacy->payload == makeSlice(wasm));
        }

        testcase("QuickJS v1 wire contract");
        auto encoded = quickJSArtifact();
        auto parsed = hook::artifact::parse(makeSlice(encoded));
        BEAST_EXPECT(parsed);
        if (parsed)
        {
            BEAST_EXPECT(parsed->kind == hook::artifact::Kind::quickJSBytecode);
            BEAST_EXPECT(parsed->hookApiVersion == 1);
            BEAST_EXPECT(hook::artifact::isCurrentQuickJS(*parsed));
            BEAST_EXPECT(
                hook::artifact::quickJSSerializedObjectMaxBytes == 1'048'576);
            BEAST_EXPECT(
                hook::artifact::quickJSSerializedObjectMaxFields == 32'768);
            BEAST_EXPECT(
                hook::artifact::quickJSSerializedObjectMaxScopes == 32'769);
            BEAST_EXPECT(hook::artifact::quickJSSerializedObjectMaxDepth == 10);
            BEAST_EXPECT(hook::artifact::quickJSProviderSize == 1'101'461);
            BEAST_EXPECT(
                hook::artifact::generated::nativeABISHA256 ==
                "328ec938dcad875f3bdf25b8d779dd77f3571589818ca9271cb98c64c7018f"
                "99");
            BEAST_EXPECT(
                hook::artifact::generated::providerImports.size() == 13);
            BEAST_EXPECT(
                hook::artifact::generated::providerExports.size() == 22);
            BEAST_EXPECT(
                hook::artifact::quickJSProviderMemoryMinimumPages == 6);
            BEAST_EXPECT(
                hook::artifact::quickJSProviderMemoryMaximumPages == 512);
            BEAST_EXPECT(
                !hook::artifact::generated::providerMemory64);
            BEAST_EXPECT(
                !hook::artifact::generated::providerMemoryShared);
            BEAST_EXPECT(
                hook::artifact::generated::javascriptSurfaceDeclarationSHA256 ==
                "56b4b2974b8a63a550721abd60350e392990762e76666f050b1a7c810e5849"
                "57");
            BEAST_EXPECT(
                hook::artifact::generated::javascriptSurfaceSHA256 ==
                "b112346b95da74d04930ba86bd597e56a428e41e11581804ccc49907ae206e"
                "b0");
            BEAST_EXPECT(parsed->payload.size() == 3);
            BEAST_EXPECT(parsed->payload[0] == 'a');
        }

        testcase("QuickJS v1 corruption rejection");
        auto expectError = [this](
                               std::vector<std::uint8_t> bytes,
                               hook::artifact::Error expected) {
            auto result = hook::artifact::parse(makeSlice(bytes));
            BEAST_EXPECT(!result);
            if (!result)
                BEAST_EXPECT(result.error() == expected);
        };

        auto corrupted = encoded;
        corrupted[4] = 2;
        expectError(
            corrupted, hook::artifact::Error::unsupportedEnvelopeVersion);
        corrupted = encoded;
        corrupted[5] = 2;
        expectError(corrupted, hook::artifact::Error::unsupportedArtifactKind);
        corrupted = encoded;
        corrupted[7] = 81;
        expectError(corrupted, hook::artifact::Error::nonCanonicalHeaderSize);
        corrupted = encoded;
        corrupted[11] = 1;
        expectError(corrupted, hook::artifact::Error::nonZeroReserved);
        corrupted = encoded;
        corrupted[15] = 4;
        expectError(corrupted, hook::artifact::Error::lengthMismatch);
        corrupted = encoded;
        std::fill(corrupted.begin() + 16, corrupted.begin() + 48, 0);
        expectError(corrupted, hook::artifact::Error::zeroBytecodeABI);
        corrupted = encoded;
        std::fill(corrupted.begin() + 48, corrupted.begin() + 80, 0);
        expectError(corrupted, hook::artifact::Error::zeroRuntimeProfile);

        testcase("Raw QuickJS bytecode is not deployable");
        std::vector<std::uint8_t> rawBytecode = {0x05, 0x0D, 0x14, 0x3C};
        auto raw = hook::artifact::parse(makeSlice(rawBytecode));
        BEAST_EXPECT(!raw);
        if (!raw)
            BEAST_EXPECT(raw.error() == hook::artifact::Error::unknownMagic);
    }
};

BEAST_DEFINE_TESTSUITE(HookArtifact, app, ripple);

}  // namespace ripple::test
