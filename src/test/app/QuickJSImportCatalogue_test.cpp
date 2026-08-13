#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSLinker.h>
#include <xrpl/beast/unit_test/suite.h>
#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <string_view>

namespace ripple::test {
namespace {

std::string_view
wasmKindName(wasm_valkind_t kind)
{
    if (kind == WASM_I32)
        return "i32";
    if (kind == WASM_I64)
        return "i64";
    return "unsupported";
}

template <class Kinds, class Name>
std::string
joinKinds(Kinds const& kinds, std::size_t count, Name&& name)
{
    std::string result;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!result.empty())
            result += ',';
        result += name(kinds[index]);
    }
    return result;
}

ripple::uint256
amendmentFromFrozenName(std::string_view name)
{
    if (name.empty())
        return {};
    if (name == "featureHooksUpdate2")
        return ripple::featureHooksUpdate2;
    return {};
}

}  // namespace

class QuickJSImportCatalogue_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        using namespace hook::quickjs;
        namespace generated = hook::artifact::generated;

        testcase("Current macro catalogue classification");
        auto const catalogue = importCatalogue();
        BEAST_EXPECT(catalogue.size() == 75);
        BEAST_EXPECT(catalogue.size() == quickJSImportCount);
        BEAST_EXPECT(generated::nativeABICatalogueCount == 75);
        std::set<std::string_view> currentNames;
        std::array<std::size_t, 12> categoryCounts{};
        for (std::size_t index = 0; index < catalogue.size(); ++index)
        {
            auto const& descriptor = catalogue[index];
            BEAST_EXPECT(static_cast<std::size_t>(descriptor.id) == index);
            BEAST_EXPECT(descriptor.category != ImportCategory::unknown);
            BEAST_EXPECT(currentNames.emplace(descriptor.name).second);
            BEAST_EXPECT(descriptor.parameterCount <= maxImportParameters);
            ++categoryCounts[static_cast<std::size_t>(descriptor.category)];
        }
        constexpr std::array<std::size_t, 12> expectedCategoryCounts{
            3, 5, 5, 8, 16, 6, 7, 11, 4, 3, 7, 0};
        BEAST_EXPECT(categoryCounts == expectedCategoryCounts);

        auto const guard = std::find_if(
            catalogue.begin(), catalogue.end(), [](auto const& descriptor) {
                return descriptor.id == QuickJSImportId::_g;
            });
        BEAST_EXPECT(guard != catalogue.end());
        if (guard != catalogue.end())
        {
            BEAST_EXPECT(guard->resultKind == WASM_I32);
            BEAST_EXPECT(guard->parameterCount == 2);
        }

        testcase("Frozen v1 policy is complete and independent");
        auto const snapshot = quickJSHostPolicyV1Snapshot();
        BEAST_EXPECT(snapshot.size() == 13);
        BEAST_EXPECT(snapshot.size() == quickJSV1ImportCount);
        auto const* policy =
            findHostAdapterPolicy(generated::hostAdapterPolicy);
        BEAST_EXPECT(policy != nullptr);
        BEAST_EXPECT(findHostAdapterPolicy("unknown-policy") == nullptr);
        if (!policy)
            return;
        BEAST_EXPECT(policy->id == "xahau-raw-hook-host-v1");
        BEAST_EXPECT(policy->hostWorkMeter == generated::hostWorkMeter);
        BEAST_EXPECT(
            policy->chargeOrder == HostChargeOrder::amendmentBeforeCharge);
        BEAST_EXPECT(
            policy->debitBehavior ==
            HostDebitBehavior::saturatedBasePlusAddressedBytes);
        BEAST_EXPECT(policy->complete());
        BEAST_EXPECT(policy->imports.size() == snapshot.size());
        BEAST_EXPECT(quickJSHostPolicyV1Snapshot().data() == snapshot.data());
        BEAST_EXPECT(policy == findHostAdapterPolicy(policy->id));

        std::set<std::string_view> v1Names;
        for (std::size_t index = 0; index < snapshot.size(); ++index)
        {
            auto const& descriptor = snapshot[index];
            auto const& resolved = policy->imports[index];
            BEAST_EXPECT(static_cast<std::size_t>(descriptor.id) == index);
            BEAST_EXPECT(descriptor.module == "env");
            BEAST_EXPECT(v1Names.emplace(descriptor.name).second);
            BEAST_EXPECT(currentNames.contains(descriptor.name));
            BEAST_EXPECT(resolved.descriptor == &descriptor);
            BEAST_EXPECT(resolved.binding != nullptr);
            if (resolved.binding)
            {
                BEAST_EXPECT(resolved.binding->id == descriptor.id);
                BEAST_EXPECT(resolved.binding->category == descriptor.category);
                BEAST_EXPECT(
                    resolved.binding->measureKind == descriptor.measure);
                BEAST_EXPECT(resolved.binding->terminal == descriptor.terminal);
                BEAST_EXPECT(
                    resolved.binding->rawOperationVersion ==
                    descriptor.rawOperationVersion);
                BEAST_EXPECT(resolved.binding->measure != nullptr);
            }
            BEAST_EXPECT(descriptor.rawOperationVersion == 1);
            BEAST_EXPECT(
                descriptor.terminal == TerminalBehavior::hookTerminal
                    ? descriptor.name == "accept" ||
                        descriptor.name == "rollback"
                    : descriptor.name != "accept" &&
                        descriptor.name != "rollback");
        }
        BEAST_EXPECT(currentNames.size() - v1Names.size() == 62);
        std::size_t outsideV1 = 0;
        for (auto const name : currentNames)
            if (!v1Names.contains(name))
                ++outsideV1;
        BEAST_EXPECT(outsideV1 == 62);

        testcase("Provider manifest has exact v1 Wasm signatures");
        BEAST_EXPECT(generated::providerImportSignatures.size() == 13);
        std::set<std::string_view> providerNames;
        for (auto const& expected : generated::providerImportSignatures)
        {
            BEAST_EXPECT(providerNames.emplace(expected.name).second);
            auto const descriptor = std::find_if(
                snapshot.begin(), snapshot.end(), [&](auto const& item) {
                    return item.name == expected.name;
                });
            BEAST_EXPECT(descriptor != snapshot.end());
            if (descriptor == snapshot.end())
                continue;
            BEAST_EXPECT(descriptor->module == expected.module);
            BEAST_EXPECT(
                joinKinds(
                    descriptor->parameterKinds,
                    descriptor->parameterCount,
                    wasmKindName) == expected.parameters);
            BEAST_EXPECT(
                wasmKindName(descriptor->resultKind) == expected.results);
        }
        BEAST_EXPECT(providerNames == v1Names);

        testcase("Pinned native ABI matches frozen and current projections");
        BEAST_EXPECT(generated::nativeImportSignatures.size() == 13);
        BEAST_EXPECT(
            generated::nativeABISourceRepository ==
            "https://github.com/Xahau/xahaud");
        BEAST_EXPECT(
            generated::nativeABISourceCommit ==
            "bb244ef7729503a0317bcff0f8fdaa93ca5cb7d2");
        BEAST_EXPECT(
            generated::nativeABISourcePath ==
            "include/xrpl/hook/hook_api.macro");
        std::set<std::string_view> nativeNames;
        for (auto const& expected : generated::nativeImportSignatures)
        {
            BEAST_EXPECT(nativeNames.emplace(expected.name).second);
            auto const frozen = std::find_if(
                snapshot.begin(), snapshot.end(), [&](auto const& item) {
                    return item.name == expected.name;
                });
            auto const current = std::find_if(
                catalogue.begin(), catalogue.end(), [&](auto const& item) {
                    return item.name == expected.name;
                });
            BEAST_EXPECT(frozen != snapshot.end());
            BEAST_EXPECT(current != catalogue.end());
            if (frozen == snapshot.end() || current == catalogue.end())
                continue;

            auto const expectedAmendment =
                amendmentFromFrozenName(expected.amendment);
            BEAST_EXPECT(
                expected.amendment.empty() ||
                expected.amendment == "featureHooksUpdate2");
            BEAST_EXPECT(frozen->amendment == expectedAmendment);
            BEAST_EXPECT(current->amendment == expectedAmendment);
            BEAST_EXPECT(
                nativeScalarName(frozen->nativeResult) == expected.result);
            BEAST_EXPECT(
                nativeScalarName(current->nativeResult) == expected.result);
            BEAST_EXPECT(
                joinKinds(
                    frozen->nativeParameters,
                    frozen->parameterCount,
                    nativeScalarName) == expected.parameters);
            BEAST_EXPECT(
                joinKinds(
                    current->nativeParameters,
                    current->parameterCount,
                    nativeScalarName) == expected.parameters);
        }
        BEAST_EXPECT(nativeNames == v1Names);

        testcase("Typed category bindings are unique");
        std::array const categoryBindings = {
            controlBindings(),
            emissionBindings(),
            hookContextBindings(),
            ledgerBindings(),
            originatingTransactionBindings(),
            stateBindings(),
            traceBindings()};
        std::set<QuickJSV1ImportId> declaredBindings;
        for (auto const bindings : categoryBindings)
        {
            for (auto const& binding : bindings)
            {
                auto const index = static_cast<std::size_t>(binding.id);
                BEAST_EXPECT(index < snapshot.size());
                if (index >= snapshot.size())
                    continue;
                BEAST_EXPECT(binding.category == snapshot[index].category);
                BEAST_EXPECT(declaredBindings.emplace(binding.id).second);
            }
        }
        BEAST_EXPECT(declaredBindings.size() == snapshot.size());

        testcase("Frozen host-work measures and saturated debit");
        for (auto const& resolved : policy->imports)
        {
            auto const& descriptor = *resolved.descriptor;
            std::array<wasmtime_val_t, maxImportParameters> arguments{};
            for (std::size_t index = 0; index < descriptor.parameterCount;
                 ++index)
            {
                arguments[index].kind =
                    descriptor.parameterKinds[index] == WASM_I32 ? WASMTIME_I32
                                                                 : WASMTIME_I64;
                if (arguments[index].kind == WASMTIME_I32)
                    arguments[index].of.i32 =
                        index == 1 ? 7 : (index == 3 ? 11 : 3);
                else
                    arguments[index].of.i64 = 3;
            }
            auto const expected = [&] {
                switch (descriptor.measure)
                {
                    case HostWorkMeasureKind::zeroV1:
                        return std::uint64_t{0};
                    case HostWorkMeasureKind::argument1V1:
                        return std::uint64_t{7};
                    case HostWorkMeasureKind::arguments1And3SaturatedV1:
                        return std::uint64_t{18};
                }
                return std::uint64_t{0};
            }();
            BEAST_EXPECT(
                resolved.binding->measure(std::span{
                    arguments.data(), descriptor.parameterCount}) == expected);
        }
        auto profile = hook::currentQuickJSRuntimeProfile();
        BEAST_EXPECT(quickJSHostWorkCost(profile, 0) == 1);
        BEAST_EXPECT(quickJSHostWorkCost(profile, 18) == 19);
        profile.hostWorkBasePerCall =
            std::numeric_limits<std::uint64_t>::max() - 1;
        profile.hostWorkPerAddressedByte = 2;
        BEAST_EXPECT(
            quickJSHostWorkCost(profile, 2) ==
            std::numeric_limits<std::uint64_t>::max());

        testcase("Missing required v1 binding fails closed");
        auto brokenImports =
            std::array<ResolvedJSImport, quickJSV1ImportCount>{};
        std::copy(
            policy->imports.begin(),
            policy->imports.end(),
            brokenImports.begin());
        brokenImports.front().binding = nullptr;
        auto broken = *policy;
        broken.imports = brokenImports;
        BEAST_EXPECT(!broken.complete());
        std::string error;
        BEAST_EXPECT(!defineImports(nullptr, broken, error));
        BEAST_EXPECT(!error.empty());
    }
};

BEAST_DEFINE_TESTSUITE(QuickJSImportCatalogue, app, ripple);

}  // namespace ripple::test
