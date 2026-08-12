#include <xrpld/app/hook/HookHostTypes.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSImportCatalogue.h>
#include <xrpl/beast/unit_test/suite.h>
#include <set>
#include <string_view>

namespace ripple::test {

class HookHostFunctionCatalogue_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        testcase("Complete macro-derived raw host catalogue");

        auto const& raw = hook::hookHostFunctionCatalogue();
        auto const projected = hook::quickjs::importCatalogue();
        BEAST_EXPECT(raw.size() == 75);
        BEAST_EXPECT(raw.size() == projected.size());

        std::set<std::string_view> names;
        for (std::size_t index = 0; index < raw.size(); ++index)
        {
            auto const& descriptor = raw[index];
            BEAST_EXPECT(descriptor.function != nullptr);
            BEAST_EXPECT(names.emplace(descriptor.name).second);

            if (index >= projected.size())
                continue;
            auto const& expected = projected[index];
            BEAST_EXPECT(descriptor.name == expected.name);
            BEAST_EXPECT(descriptor.amendment == expected.amendment);
            BEAST_EXPECT(
                descriptor.parameters.size() == expected.parameterCount);
            BEAST_EXPECT(
                (descriptor.result == hook::HookHostValueKind::i32
                     ? WASM_I32
                     : WASM_I64) == expected.resultKind);
            for (std::size_t parameter = 0;
                 parameter < descriptor.parameters.size() &&
                 parameter < expected.parameterCount;
                 ++parameter)
            {
                BEAST_EXPECT(
                    (descriptor.parameters[parameter] ==
                             hook::HookHostValueKind::i32
                         ? WASM_I32
                         : WASM_I64) == expected.parameterKinds[parameter]);
            }
        }
    }
};

BEAST_DEFINE_TESTSUITE(HookHostFunctionCatalogue, app, ripple);

}  // namespace ripple::test
