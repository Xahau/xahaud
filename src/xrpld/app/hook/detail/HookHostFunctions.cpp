#include <xrpld/app/hook/HookHostFunction.h>
#include <xrpld/app/hook/applyHook.h>
#include <type_traits>

namespace hook {
namespace {

template <class T>
constexpr HookHostValueKind
kindOf() noexcept
{
    if constexpr (std::is_same_v<T, std::int32_t>)
        return HookHostValueKind::i32;
    else if constexpr (std::is_same_v<T, std::uint32_t>)
        return HookHostValueKind::u32;
    else if constexpr (std::is_same_v<T, std::int64_t>)
        return HookHostValueKind::i64;
    else
    {
        static_assert(std::is_same_v<T, std::uint64_t>);
        return HookHostValueKind::u64;
    }
}

template <class... Ts>
std::vector<HookHostValueKind>
kindsOf()
{
    return {kindOf<Ts>()...};
}

}  // namespace

std::span<HookHostFunctionDescriptor const>
hookHostFunctionCatalogue() noexcept
{
#pragma push_macro("HOOK_API_DEFINITION")
#pragma push_macro("HOOK_WRAP_PARAMS")
#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS

#define HOOK_WRAP_PARAMS(...) __VA_ARGS__
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, GATE) \
    {                                                                       \
        #FUNCTION_NAME,                                                     \
        &hook_api::HostFunction##FUNCTION_NAME,                             \
        kindsOf<HOOK_WRAP_PARAMS PARAMS_TUPLE>(),                           \
        kindOf<RETURN_TYPE>(),                                              \
        ripple::GATE,                                                       \
        1,                                                                  \
    },

    static std::vector<HookHostFunctionDescriptor> const catalogue = {
#include <xrpl/hook/hook_api.macro>
    };

#undef HOOK_API_DEFINITION
#undef HOOK_WRAP_PARAMS
#pragma pop_macro("HOOK_WRAP_PARAMS")
#pragma pop_macro("HOOK_API_DEFINITION")

    return catalogue;
}

HookHostFunctionDescriptor const*
findHookHostFunction(std::string_view name) noexcept
{
    for (auto const& descriptor : hookHostFunctionCatalogue())
        if (descriptor.name == name)
            return &descriptor;
    return nullptr;
}

HookHostFunctionDescriptor const*
findHookHostFunction(
    std::string_view name,
    std::uint16_t implementationVersion) noexcept
{
    for (auto const& descriptor : hookHostFunctionCatalogue())
        if (descriptor.name == name &&
            descriptor.implementationVersion == implementationVersion)
            return &descriptor;
    return nullptr;
}

}  // namespace hook
