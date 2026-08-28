#include <xrpld/app/hook/HookHostFunction.h>
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
wasmScalarName(hook::HookHostValueKind kind)
{
    return hook::isI32(kind) ? "i32" : "i64";
}

std::string_view
nativeScalarName(hook::HookHostValueKind kind)
{
    switch (kind)
    {
        case hook::HookHostValueKind::i32:
            return "int32_t";
        case hook::HookHostValueKind::u32:
            return "uint32_t";
        case hook::HookHostValueKind::i64:
            return "int64_t";
        case hook::HookHostValueKind::u64:
            return "uint64_t";
    }
    return {};
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

class QuickJSHostPolicy_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        using namespace hook::quickjs;
        namespace generated = hook::artifact::generated;

        testcase("Current macro catalogue");
        auto const hostCatalogue = hook::hookHostFunctionCatalogue();
        BEAST_EXPECT(hostCatalogue.size() == 75);
        BEAST_EXPECT(generated::nativeABICatalogueCount == 75);
        std::set<std::string_view> currentNames;
        for (auto const& operation : hostCatalogue)
        {
            BEAST_EXPECT(operation.function != nullptr);
            BEAST_EXPECT(operation.implementationVersion == 1);
            BEAST_EXPECT(currentNames.emplace(operation.name).second);
            BEAST_EXPECT(operation.parameters.size() <= maxImportParameters);
        }

        auto const guard = std::find_if(
            hostCatalogue.begin(),
            hostCatalogue.end(),
            [](auto const& descriptor) { return descriptor.name == "_g"; });
        BEAST_EXPECT(guard != hostCatalogue.end());
        if (guard != hostCatalogue.end())
        {
            BEAST_EXPECT(guard->result == hook::HookHostValueKind::i32);
            BEAST_EXPECT(guard->parameters.size() == 2);
        }

        testcase("Frozen v1 policy is complete and independent");
        auto const snapshot = quickJSHostPolicyV1Snapshot();
        BEAST_EXPECT(snapshot.size() == 17);
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
            auto const& binding = policy->imports[index];
            BEAST_EXPECT(static_cast<std::size_t>(descriptor.id) == index);
            BEAST_EXPECT(descriptor.module == "env");
            BEAST_EXPECT(v1Names.emplace(descriptor.name).second);
            BEAST_EXPECT(currentNames.contains(descriptor.name));
            BEAST_EXPECT(binding.module == descriptor.module);
            BEAST_EXPECT(binding.name == descriptor.name);
            BEAST_EXPECT(binding.parameterCount == descriptor.parameterCount);
            BEAST_EXPECT(binding.parameters == descriptor.nativeParameters);
            BEAST_EXPECT(binding.result == descriptor.nativeResult);
            BEAST_EXPECT(
                binding.implementationVersion ==
                descriptor.rawOperationVersion);
            BEAST_EXPECT(binding.amendment == descriptor.amendment);
            BEAST_EXPECT(binding.measure == descriptor.measure);
            BEAST_EXPECT(binding.terminal == descriptor.terminal);
            BEAST_EXPECT(
                binding.charging == WasmtimeHostBinding::Charging::quickJSV1);
            BEAST_EXPECT(binding.operation != nullptr);
            if (binding.operation)
            {
                BEAST_EXPECT(
                    binding.operation->implementationVersion ==
                    descriptor.rawOperationVersion);
                BEAST_EXPECT(binding.operation->name == descriptor.name);
            }
            BEAST_EXPECT(descriptor.rawOperationVersion == 1);
            BEAST_EXPECT(
                descriptor.terminal == TerminalBehavior::hookTerminal
                    ? descriptor.name == "accept" ||
                        descriptor.name == "rollback"
                    : descriptor.name != "accept" &&
                        descriptor.name != "rollback");
        }
        BEAST_EXPECT(currentNames.size() - v1Names.size() == 58);
        std::size_t outsideV1 = 0;
        for (auto const name : currentNames)
            if (!v1Names.contains(name))
                ++outsideV1;
        BEAST_EXPECT(outsideV1 == 58);

        testcase("Provider manifest has exact v1 Wasm signatures");
        BEAST_EXPECT(generated::providerImportSignatures.size() == 17);
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
                    descriptor->nativeParameters,
                    descriptor->parameterCount,
                    wasmScalarName) == expected.parameters);
            BEAST_EXPECT(
                wasmScalarName(descriptor->nativeResult) == expected.results);
        }
        BEAST_EXPECT(providerNames == v1Names);

        testcase("Pinned native ABI matches frozen and current projections");
        BEAST_EXPECT(generated::nativeImportSignatures.size() == 17);
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
                hostCatalogue.begin(),
                hostCatalogue.end(),
                [&](auto const& item) { return item.name == expected.name; });
            BEAST_EXPECT(frozen != snapshot.end());
            BEAST_EXPECT(current != hostCatalogue.end());
            if (frozen == snapshot.end() || current == hostCatalogue.end())
                continue;

            auto const expectedAmendment =
                amendmentFromFrozenName(expected.amendment);
            BEAST_EXPECT(
                expected.amendment.empty() ||
                expected.amendment == "featureHooksUpdate2");
            BEAST_EXPECT(frozen->amendment == expectedAmendment);
            BEAST_EXPECT(current->declarationAmendment == expectedAmendment);
            BEAST_EXPECT(
                nativeScalarName(frozen->nativeResult) == expected.result);
            BEAST_EXPECT(nativeScalarName(current->result) == expected.result);
            BEAST_EXPECT(
                joinKinds(
                    frozen->nativeParameters,
                    frozen->parameterCount,
                    nativeScalarName) == expected.parameters);
            BEAST_EXPECT(
                joinKinds(
                    current->parameters,
                    current->parameters.size(),
                    nativeScalarName) == expected.parameters);
        }
        BEAST_EXPECT(nativeNames == v1Names);

        testcase("Selected imports resolve unique neutral operations");
        std::set<std::string_view> selectedOperations;
        for (auto const& binding : policy->imports)
        {
            BEAST_EXPECT(binding.operation != nullptr);
            if (!binding.operation)
                continue;
            BEAST_EXPECT(
                selectedOperations.emplace(binding.operation->name).second);
        }
        BEAST_EXPECT(selectedOperations.size() == snapshot.size());

        testcase("Frozen host-work measures and saturated debit");
        for (std::size_t bindingIndex = 0;
             bindingIndex < policy->imports.size();
             ++bindingIndex)
        {
            auto const& descriptor = snapshot[bindingIndex];
            std::array<wasmtime_val_t, maxImportParameters> arguments{};
            for (std::size_t index = 0; index < descriptor.parameterCount;
                 ++index)
            {
                arguments[index].kind =
                    hook::isI32(descriptor.nativeParameters[index])
                    ? WASMTIME_I32
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
                declaredHostWork(
                    descriptor.measure,
                    std::span{arguments.data(), descriptor.parameterCount}) ==
                expected);
        }
        auto profile = hook::currentQuickJSRuntimeProfile();
        BEAST_EXPECT(profile.hostWorkBudget == 2'097'152);
        BEAST_EXPECT(quickJSHostWorkCost(profile, 0) == 1);
        BEAST_EXPECT(quickJSHostWorkCost(profile, 18) == 19);
        auto const findMeasure = [&](std::string_view name) {
            auto const found = std::find_if(
                snapshot.begin(), snapshot.end(), [&](auto const& item) {
                    return item.name == name;
                });
            BEAST_EXPECT(found != snapshot.end());
            return found == snapshot.end() ? HostWorkMeasureKind::zeroV1
                                           : found->measure;
        };
        BEAST_EXPECT(findMeasure("otxn_slot") == HostWorkMeasureKind::zeroV1);
        BEAST_EXPECT(findMeasure("slot_size") == HostWorkMeasureKind::zeroV1);
        BEAST_EXPECT(findMeasure("slot") == HostWorkMeasureKind::argument1V1);
        BEAST_EXPECT(findMeasure("slot_clear") == HostWorkMeasureKind::zeroV1);
        auto const maximumAcquisitionCost =
            5 * quickJSHostWorkCost(profile, 0) +
            quickJSHostWorkCost(profile, profile.serializedObjectMaxBytes);
        BEAST_EXPECT(maximumAcquisitionCost == 1'048'582);
        BEAST_EXPECT(
            profile.hostWorkBudget - maximumAcquisitionCost == 1'048'570);
        profile.hostWorkBasePerCall =
            std::numeric_limits<std::uint64_t>::max() - 1;
        profile.hostWorkPerAddressedByte = 2;
        BEAST_EXPECT(
            quickJSHostWorkCost(profile, 2) ==
            std::numeric_limits<std::uint64_t>::max());

        testcase("Missing required v1 binding fails closed");
        auto brokenImports =
            std::array<WasmtimeHostBinding, quickJSV1ImportCount>{};
        std::copy(
            policy->imports.begin(),
            policy->imports.end(),
            brokenImports.begin());
        brokenImports.front().operation = nullptr;
        auto broken = *policy;
        broken.imports = brokenImports;
        BEAST_EXPECT(!broken.complete());
        std::string error;
        BEAST_EXPECT(!defineImports(nullptr, broken, error));
        BEAST_EXPECT(!error.empty());
    }
};

BEAST_DEFINE_TESTSUITE(QuickJSHostPolicy, app, ripple);

}  // namespace ripple::test
