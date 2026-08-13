#include <xrpld/app/hook/detail/quickjs/QuickJSImportCatalogue.h>
#include <utility>

namespace hook::quickjs {
namespace {

template <class Tuple, std::size_t... Indices>
std::array<wasm_valkind_t, maxImportParameters>
parameterKinds(std::index_sequence<Indices...>) noexcept
{
    std::array<wasm_valkind_t, maxImportParameters> result{};
    ((result[Indices] = wasmKind<std::tuple_element_t<Indices, Tuple>>), ...);
    return result;
}

template <class Tuple, std::size_t... Indices>
std::array<NativeScalarKind, maxImportParameters>
nativeParameterKinds(std::index_sequence<Indices...>) noexcept
{
    std::array<NativeScalarKind, maxImportParameters> result{};
    ((result[Indices] = nativeScalarKind<std::tuple_element_t<Indices, Tuple>>),
     ...);
    return result;
}

template <QuickJSImportId Id>
QuickJSImportDescriptor
makeDescriptor()
{
    using Traits = ImportTraits<Id>;
    constexpr auto parameterCount =
        std::tuple_size_v<typename Traits::Parameters>;
    static_assert(parameterCount <= maxImportParameters);
    return {
        .id = Id,
        .name = Traits::name,
        .category = Traits::category,
        .amendment = Traits::amendment(),
        .nativeResult = nativeScalarKind<typename Traits::Return>,
        .nativeParameters = nativeParameterKinds<typename Traits::Parameters>(
            std::make_index_sequence<parameterCount>{}),
        .resultKind = wasmKind<typename Traits::Return>,
        .parameterKinds = parameterKinds<typename Traits::Parameters>(
            std::make_index_sequence<parameterCount>{}),
        .parameterCount = static_cast<std::uint8_t>(parameterCount)};
}

std::array<QuickJSImportDescriptor, quickJSImportCount> const&
catalogueStorage()
{
    static std::array<QuickJSImportDescriptor, quickJSImportCount> const value =
        {
#pragma push_macro("HOOK_API_DEFINITION")
#undef HOOK_API_DEFINITION
#define HOOK_API_DEFINITION(RETURN_TYPE, FUNCTION_NAME, PARAMS_TUPLE, ...) \
    makeDescriptor<QuickJSImportId::FUNCTION_NAME>(),
#include <xrpl/hook/hook_api.macro>
#undef HOOK_API_DEFINITION
#pragma pop_macro("HOOK_API_DEFINITION")
        };
    return value;
}

}  // namespace

std::span<QuickJSImportDescriptor const>
importCatalogue() noexcept
{
    return catalogueStorage();
}

std::string_view
categoryName(ImportCategory category) noexcept
{
    switch (category)
    {
        case ImportCategory::control:
            return "control";
        case ImportCategory::util:
            return "util";
        case ImportCategory::serializedObject:
            return "serialized-object";
        case ImportCategory::emission:
            return "emission";
        case ImportCategory::floatingPoint:
            return "float";
        case ImportCategory::ledger:
            return "ledger";
        case ImportCategory::hookContext:
            return "hook-context";
        case ImportCategory::slot:
            return "slot";
        case ImportCategory::state:
            return "state";
        case ImportCategory::trace:
            return "trace";
        case ImportCategory::originatingTransaction:
            return "originating-transaction";
        case ImportCategory::unknown:
            return "unknown";
    }
    return "unknown";
}

std::string_view
nativeScalarName(NativeScalarKind kind) noexcept
{
    switch (kind)
    {
        case NativeScalarKind::i32:
            return "int32_t";
        case NativeScalarKind::u32:
            return "uint32_t";
        case NativeScalarKind::i64:
            return "int64_t";
        case NativeScalarKind::u64:
            return "uint64_t";
    }
    return {};
}

}  // namespace hook::quickjs
