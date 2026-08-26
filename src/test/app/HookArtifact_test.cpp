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
#include <string_view>
#include <vector>

namespace ripple::test {
namespace {

struct ExpectedProviderExport
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

constexpr ExpectedProviderExport expectedProviderExports[] = {
    {"function", "_initialize", "", "", 0, 0, false, false},
    {"function", "free", "i32", "", 0, 0, false, false},
    {"function", "malloc", "i32", "i32", 0, 0, false, false},
    {"memory", "memory", "", "", 7, 512, false, false},
    {"function", "qjs_cbak", "i32,i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_compile", "i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_compile_module", "i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_destroy", "", "", 0, 0, false, false},
    {"function", "qjs_enable_coverage", "i32", "", 0, 0, false, false},
    {"function", "qjs_eval", "i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_eval_bytecode", "i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_eval_module", "i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_get_bytecode_len", "", "i32", 0, 0, false, false},
    {"function", "qjs_get_bytecode_ptr", "", "i32", 0, 0, false, false},
    {"function", "qjs_get_result_len", "", "i32", 0, 0, false, false},
    {"function", "qjs_get_result_ptr", "", "i32", 0, 0, false, false},
    {"function", "qjs_hook", "i32,i32,i32", "i32", 0, 0, false, false},
    {"function", "qjs_init", "", "", 0, 0, false, false},
    {"function", "qjs_set_max_stack_size", "i32", "", 0, 0, false, false},
    {"function", "qjs_set_memory_limit", "i32", "", 0, 0, false, false},
    {"function", "qjs_set_seed", "i32", "", 0, 0, false, false},
    {"function",
     "qjs_validate_hook_module",
     "i32,i32",
     "i32",
     0,
     0,
     false,
     false},
};

std::vector<std::uint8_t>
quickJSArtifact(
    std::vector<std::uint8_t> const& payload = {'a', 'b', 'c'},
    hook::artifact::XFLArithmeticProfile profile =
        hook::artifact::XFLArithmeticProfile::none,
    std::uint8_t envelopeVersion =
        hook::artifact::quickJSCurrentEnvelopeVersion)
{
    std::vector<std::uint8_t> result(
        hook::artifact::quickJSHeaderSize + payload.size(), 0);
    std::copy(
        hook::artifact::quickJSMagic.begin(),
        hook::artifact::quickJSMagic.end(),
        result.begin());
    result[4] = envelopeVersion;
    result[5] = hook::artifact::quickJSBytecodeKind;
    result[6] = 0;
    result[7] = hook::artifact::quickJSHeaderSize;
    result[8] = 0;
    result[9] = 1;
    auto const profileCode = static_cast<std::uint16_t>(profile);
    result[10] = static_cast<std::uint8_t>(profileCode >> 8);
    result[11] = static_cast<std::uint8_t>(profileCode);
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

        testcase("QuickJS v1 and v2 wire contract");
        auto encoded = quickJSArtifact();
        auto parsed = hook::artifact::parse(makeSlice(encoded));
        BEAST_EXPECT(parsed);
        if (parsed)
        {
            BEAST_EXPECT(parsed->kind == hook::artifact::Kind::quickJSBytecode);
            BEAST_EXPECT(
                parsed->envelopeVersion ==
                hook::artifact::quickJSCurrentEnvelopeVersion);
            BEAST_EXPECT(parsed->hookApiVersion == 1);
            BEAST_EXPECT(
                parsed->xflArithmeticProfile ==
                hook::artifact::XFLArithmeticProfile::none);
            BEAST_EXPECT(hook::artifact::isCurrentQuickJS(*parsed));
            BEAST_EXPECT(
                hook::artifact::quickJSSerializedObjectMaxBytes == 1'048'576);
            BEAST_EXPECT(
                hook::artifact::quickJSSerializedObjectMaxFields == 32'768);
            BEAST_EXPECT(
                hook::artifact::quickJSSerializedObjectMaxScopes == 32'769);
            BEAST_EXPECT(hook::artifact::quickJSSerializedObjectMaxDepth == 10);
            BEAST_EXPECT(hook::artifact::quickJSProviderSize == 1'122'108);
            BEAST_EXPECT(
                hook::artifact::generated::providerManifestSHA256 ==
                "08d288f2e0ccc1e232f5dacdbed02ec92b45e1425b82b39fef4214bdd11e7"
                "217");
            BEAST_EXPECT(
                hook::artifact::generated::nativeABISHA256 ==
                "328ec938dcad875f3bdf25b8d779dd77f3571589818ca9271cb98c64c7018f"
                "99");
            BEAST_EXPECT(
                hook::artifact::generated::providerImports.size() == 13);
            BEAST_EXPECT(
                hook::artifact::generated::providerExports.size() == 22);
            BEAST_EXPECT(
                hook::artifact::generated::providerExportSignatures.size() ==
                22);
            BEAST_EXPECT(
                hook::artifact::quickJSProviderMemoryMinimumPages == 7);
            BEAST_EXPECT(
                hook::artifact::quickJSProviderMemoryMaximumPages == 512);
            BEAST_EXPECT(!hook::artifact::generated::providerMemory64);
            BEAST_EXPECT(!hook::artifact::generated::providerMemoryShared);
            BEAST_EXPECT(
                hook::artifact::quickJSProviderWasmStackBytes == 131'072);
            BEAST_EXPECT(hook::artifact::generated::wasmStackBytes == 131'072);
            constexpr auto exportCount = sizeof(expectedProviderExports) /
                sizeof(expectedProviderExports[0]);
            BEAST_EXPECT(
                hook::artifact::generated::providerExportSignatures.size() ==
                exportCount);
            if (hook::artifact::generated::providerExportSignatures.size() ==
                exportCount)
            {
                for (std::size_t index = 0; index < exportCount; ++index)
                {
                    auto const& actual = hook::artifact::generated::
                        providerExportSignatures[index];
                    auto const& expected = expectedProviderExports[index];
                    BEAST_EXPECT(actual.kind == expected.kind);
                    BEAST_EXPECT(actual.name == expected.name);
                    BEAST_EXPECT(actual.parameters == expected.parameters);
                    BEAST_EXPECT(actual.results == expected.results);
                    BEAST_EXPECT(actual.minimumPages == expected.minimumPages);
                    BEAST_EXPECT(actual.maximumPages == expected.maximumPages);
                    BEAST_EXPECT(actual.memory64 == expected.memory64);
                    BEAST_EXPECT(actual.shared == expected.shared);
                    BEAST_EXPECT(
                        hook::artifact::generated::providerExports[index] ==
                        expected.name);
                }
            }
            BEAST_EXPECT(
                hook::artifact::generated::javascriptBroadDeclarationSHA256 ==
                "c83351e646c85dfa14ba478bbf0074bf9fd6fcbe6c74bd95ac0a024a185f0"
                "b4d");
            BEAST_EXPECT(
                hook::artifact::generated::javascriptExactV1DeclarationSHA256 ==
                "f69fabc3912ced6e1f6f2cdf3bc9fa8882302fc28079f6019d1c0571534121"
                "1e");
            BEAST_EXPECT(
                hook::artifact::generated::javascriptSurfaceSHA256 ==
                "046b487c8e374770e4f216b5ad2c60eca05de50cf65b4bb2242e381eb6d2e"
                "331");
            BEAST_EXPECT(
                hook::artifact::generated::javascriptXFLProfileLedgerSHA256 ==
                "39263a9726085a36d584a81b4503ed01dc2266c4f43b0603f043cfc54beaf0"
                "52");
            BEAST_EXPECT(
                hook::artifact::generated::
                    javascriptAPIArtifactManifestSHA256 ==
                "b1d1c24dd711ec69c3a13198e3fe13ad8577dd27d25f2c6de2ae0c2887f0"
                "d3c9");
            BEAST_EXPECT(
                hook::artifact::generated::xqjsEnvelopeVersion == 2);
            BEAST_EXPECT(
                hook::artifact::generated::xflArithmeticProfileNone == 0);
            BEAST_EXPECT(
                hook::artifact::generated::xflArithmeticProfileXahauFloatV1 ==
                1);
            BEAST_EXPECT(
                hook::artifact::generated::
                    xflArithmeticProfileNearestEvenV1 == 2);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationLayoutVersion == 1);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationFailureSentinel ==
                -1);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationMainBit == 1);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationCallbackBit == 2);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationEntryMask == 3);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationReservedMask ==
                0x800000FCU);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationProfileMask ==
                0x00FFFF00U);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationProfileShift == 8);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationVersionMask ==
                0x7F000000U);
            BEAST_EXPECT(
                hook::artifact::generated::moduleValidationVersionShift == 24);
            BEAST_EXPECT(parsed->payload.size() == 3);
            BEAST_EXPECT(parsed->payload[0] == 'a');
        }

        auto const v1Encoded = quickJSArtifact(
            {'a', 'b', 'c'},
            hook::artifact::XFLArithmeticProfile::none,
            hook::artifact::quickJSLegacyEnvelopeVersion);
        auto const v1Parsed = hook::artifact::parse(makeSlice(v1Encoded));
        BEAST_EXPECT(v1Parsed);
        if (v1Parsed)
        {
            BEAST_EXPECT(
                v1Parsed->envelopeVersion ==
                hook::artifact::quickJSLegacyEnvelopeVersion);
            BEAST_EXPECT(
                v1Parsed->xflArithmeticProfile ==
                hook::artifact::XFLArithmeticProfile::none);
            BEAST_EXPECT(v1Parsed->payload.size() == 3);
        }

        for (auto const profile : {
                 hook::artifact::XFLArithmeticProfile::none,
                 hook::artifact::XFLArithmeticProfile::xahauFloatV1,
                 hook::artifact::XFLArithmeticProfile::nearestEvenV1})
        {
            auto const profiled = quickJSArtifact({'x'}, profile);
            auto const profiledView =
                hook::artifact::parse(makeSlice(profiled));
            BEAST_EXPECT(profiledView);
            if (profiledView)
                BEAST_EXPECT(profiledView->xflArithmeticProfile == profile);
        }

        testcase("QuickJS v1 and v2 corruption rejection");
        auto expectError = [this](
                               std::vector<std::uint8_t> bytes,
                               hook::artifact::Error expected) {
            auto result = hook::artifact::parse(makeSlice(bytes));
            BEAST_EXPECT(!result);
            if (!result)
                BEAST_EXPECT(result.error() == expected);
        };

        auto corrupted = encoded;
        corrupted[4] = 3;
        expectError(
            corrupted, hook::artifact::Error::unsupportedEnvelopeVersion);
        corrupted = encoded;
        corrupted[5] = 2;
        expectError(corrupted, hook::artifact::Error::unsupportedArtifactKind);
        corrupted = encoded;
        corrupted[7] = 81;
        expectError(corrupted, hook::artifact::Error::nonCanonicalHeaderSize);
        corrupted = v1Encoded;
        corrupted[11] = 1;
        expectError(corrupted, hook::artifact::Error::nonZeroReserved);
        corrupted = encoded;
        corrupted[11] = 3;
        expectError(
            corrupted,
            hook::artifact::Error::unsupportedXFLArithmeticProfile);
        corrupted = encoded;
        corrupted[10] = 0xff;
        corrupted[11] = 0xff;
        expectError(
            corrupted,
            hook::artifact::Error::unsupportedXFLArithmeticProfile);
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
