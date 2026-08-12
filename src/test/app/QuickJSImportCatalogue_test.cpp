#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSLinker.h>
#include <xrpl/beast/unit_test/suite.h>
#include <algorithm>
#include <array>
#include <set>
#include <string_view>

namespace ripple::test {

class QuickJSImportCatalogue_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        using namespace hook::quickjs;

        testcase("Macro catalogue classification");
        auto const catalogue = importCatalogue();
        BEAST_EXPECT(catalogue.size() == quickJSImportCount);
        std::set<std::string_view> names;
        std::array<std::size_t, 12> categoryCounts{};
        for (std::size_t index = 0; index < catalogue.size(); ++index)
        {
            auto const& descriptor = catalogue[index];
            BEAST_EXPECT(static_cast<std::size_t>(descriptor.id) == index);
            BEAST_EXPECT(descriptor.category != ImportCategory::unknown);
            BEAST_EXPECT(names.emplace(descriptor.name).second);
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

        testcase("Resolved policy and provider-required bindings");
        auto const* policy =
            findHostAdapterPolicy(hook::artifact::generated::hostAdapterPolicy);
        BEAST_EXPECT(policy != nullptr);
        BEAST_EXPECT(findHostAdapterPolicy("unknown-policy") == nullptr);
        if (!policy)
            return;
        BEAST_EXPECT(policy->imports.size() == catalogue.size());

        testcase("Category bindings are unique and correctly classified");
        std::array const categoryBindings = {
            controlBindings(),
            emissionBindings(),
            hookContextBindings(),
            ledgerBindings(),
            originatingTransactionBindings(),
            stateBindings(),
            traceBindings()};
        std::set<QuickJSImportId> declaredBindings;
        for (auto const bindings : categoryBindings)
        {
            for (auto const& binding : bindings)
            {
                auto const index = static_cast<std::size_t>(binding.id);
                BEAST_EXPECT(index < catalogue.size());
                if (index >= catalogue.size())
                    continue;
                BEAST_EXPECT(binding.category == catalogue[index].category);
                BEAST_EXPECT(declaredBindings.emplace(binding.id).second);
            }
        }

        std::set<QuickJSImportId> bound;
        for (std::size_t index = 0; index < policy->imports.size(); ++index)
        {
            auto const& resolved = policy->imports[index];
            BEAST_EXPECT(resolved.descriptor == &catalogue[index]);
            if (!resolved.binding)
                continue;
            BEAST_EXPECT(resolved.binding->id == resolved.descriptor->id);
            BEAST_EXPECT(
                resolved.binding->category == resolved.descriptor->category);
            BEAST_EXPECT(bound.emplace(resolved.binding->id).second);
        }

        for (auto const required : hook::artifact::generated::providerImports)
        {
            auto const descriptor = std::find_if(
                catalogue.begin(), catalogue.end(), [&](auto const& item) {
                    return item.name == required;
                });
            BEAST_EXPECT(descriptor != catalogue.end());
            if (descriptor != catalogue.end())
                BEAST_EXPECT(
                    findBinding(descriptor->id, descriptor->category) !=
                    nullptr);
        }
    }
};

BEAST_DEFINE_TESTSUITE(QuickJSImportCatalogue, app, ripple);

}  // namespace ripple::test
