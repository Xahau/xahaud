#include <xrpld/app/hook/HookHostTypes.h>
#include <xrpld/app/hook/applyHook.h>
#include <xrpl/protocol/Feature.h>
#include <type_traits>

namespace hook {
namespace {

template <class T>
constexpr HookHostValueKind
kindOf() noexcept
{
    if constexpr (
        std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::int32_t>)
        return HookHostValueKind::i32;
    else
        return HookHostValueKind::i64;
}

template <class... Ts>
std::vector<HookHostValueKind>
kindsOf()
{
    return {kindOf<Ts>()...};
}

}  // namespace

std::vector<HookHostFunctionDescriptor> const&
hookHostFunctionCatalogue()
{
    using namespace ripple;

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
        GATE,                                                               \
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

}  // namespace hook
