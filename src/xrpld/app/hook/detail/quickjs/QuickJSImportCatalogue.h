#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSIMPORTCATALOGUE_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSIMPORTCATALOGUE_H_INCLUDED

#include <xrpld/app/hook/HookHostFunction.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/Feature.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <wasmtime.h>

namespace hook::quickjs {

enum class QuickJSImportId : std::uint16_t {
#pragma push_macro("HOOK_API_DEFINITION")
#undef HOOK_API_DEFINITION
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    FUNCTION_NAME,
#include <xrpl/hook/hook_api.macro>
#undef HOOK_API_DEFINITION
#pragma pop_macro("HOOK_API_DEFINITION")
    count
};

template <QuickJSImportId>
struct ImportTraits;

#define QUICKJS_EXPAND_PARAMS(...) __VA_ARGS__
#pragma push_macro("HOOK_API_DEFINITION")
#undef HOOK_API_DEFINITION
#define HOOK_API_DEFINITION(                                               \
    RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, AMENDMENT)                   \
    template <>                                                            \
    struct ImportTraits<QuickJSImportId::FUNCTION_NAME>                    \
    {                                                                      \
        using Return = RETURN_TYPE;                                        \
        using Parameters = std::tuple<QUICKJS_EXPAND_PARAMS PARAMS_TUPLE>; \
        static constexpr std::string_view name = #FUNCTION_NAME;           \
        static ripple::uint256                                             \
        amendment()                                                        \
        {                                                                  \
            return ripple::AMENDMENT;                                      \
        }                                                                  \
    };
#include <xrpl/hook/hook_api.macro>
#undef HOOK_API_DEFINITION
#pragma pop_macro("HOOK_API_DEFINITION")
#undef QUICKJS_EXPAND_PARAMS

template <class T>
inline constexpr wasm_valkind_t wasmKind = [] {
    static_assert(
        std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::int32_t> ||
            std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::int64_t>,
        "unsupported Hook import scalar type");
    if constexpr (
        std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::int32_t>)
        return WASM_I32;
    else
        return WASM_I64;
}();

using NativeScalarKind = HookHostValueKind;

template <class T>
inline constexpr NativeScalarKind nativeScalarKind = [] {
    if constexpr (std::is_same_v<T, std::int32_t>)
        return NativeScalarKind::i32;
    else if constexpr (std::is_same_v<T, std::uint32_t>)
        return NativeScalarKind::u32;
    else if constexpr (std::is_same_v<T, std::int64_t>)
        return NativeScalarKind::i64;
    else
    {
        static_assert(std::is_same_v<T, std::uint64_t>);
        return NativeScalarKind::u64;
    }
}();

inline constexpr std::size_t maxImportParameters = 9;

struct QuickJSImportDescriptor
{
    QuickJSImportId id;
    std::string_view name;
    ripple::uint256 amendment;
    NativeScalarKind nativeResult;
    std::array<NativeScalarKind, maxImportParameters> nativeParameters;
    wasm_valkind_t resultKind;
    std::array<wasm_valkind_t, maxImportParameters> parameterKinds;
    std::uint8_t parameterCount;
};

inline constexpr std::size_t quickJSImportCount =
    static_cast<std::size_t>(QuickJSImportId::count);

std::span<QuickJSImportDescriptor const>
importCatalogue() noexcept;

std::string_view
nativeScalarName(NativeScalarKind kind) noexcept;

}  // namespace hook::quickjs

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSIMPORTCATALOGUE_H_INCLUDED
