#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSIMPORTCATALOGUE_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSIMPORTCATALOGUE_H_INCLUDED

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

enum class ImportCategory : std::uint8_t {
    control,
    util,
    serializedObject,
    emission,
    floatingPoint,
    ledger,
    hookContext,
    slot,
    state,
    trace,
    originatingTransaction,
    unknown
};

struct CategorySelection
{
    ImportCategory category;
    std::uint8_t matches;
};

constexpr CategorySelection
classifyImport(std::string_view name) noexcept
{
    CategorySelection result{ImportCategory::unknown, 0};
    auto const select = [&](bool matches, ImportCategory category) {
        if (!matches)
            return;
        result.category = category;
        ++result.matches;
    };
    select(
        name == "_g" || name == "accept" || name == "rollback",
        ImportCategory::control);
    select(name.starts_with("util_"), ImportCategory::util);
    select(name.starts_with("sto_"), ImportCategory::serializedObject);
    select(
        name.starts_with("etxn_") || name == "emit" || name == "prepare",
        ImportCategory::emission);
    select(name.starts_with("float_"), ImportCategory::floatingPoint);
    select(
        name == "fee_base" || name.starts_with("ledger_"),
        ImportCategory::ledger);
    select(name.starts_with("hook_"), ImportCategory::hookContext);
    select(
        name == "slot" || name.starts_with("slot_") || name == "meta_slot" ||
            name == "xpop_slot",
        ImportCategory::slot);
    select(
        name == "state" || name.starts_with("state_"), ImportCategory::state);
    select(
        name == "trace" || name.starts_with("trace_"), ImportCategory::trace);
    select(name.starts_with("otxn_"), ImportCategory::originatingTransaction);
    return result;
}

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
#define HOOK_API_DEFINITION(                                                 \
    RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, AMENDMENT)                     \
    template <>                                                              \
    struct ImportTraits<QuickJSImportId::FUNCTION_NAME>                      \
    {                                                                        \
        using Return = RETURN_TYPE;                                          \
        using Parameters = std::tuple<QUICKJS_EXPAND_PARAMS PARAMS_TUPLE>;   \
        static constexpr std::string_view name = #FUNCTION_NAME;             \
        static constexpr CategorySelection selection = classifyImport(name); \
        static_assert(                                                       \
            selection.matches == 1,                                          \
            "each Hook import must have exactly one category");              \
        static constexpr ImportCategory category = selection.category;       \
        static ripple::uint256                                               \
        amendment()                                                          \
        {                                                                    \
            return ripple::AMENDMENT;                                        \
        }                                                                    \
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

enum class NativeScalarKind : std::uint8_t { i32, u32, i64, u64 };

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
    ImportCategory category;
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
categoryName(ImportCategory category) noexcept;

std::string_view
nativeScalarName(NativeScalarKind kind) noexcept;

}  // namespace hook::quickjs

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSIMPORTCATALOGUE_H_INCLUDED
