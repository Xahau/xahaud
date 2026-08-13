#include <xrpld/app/hook/detail/quickjs/QuickJSHostPolicy.h>
#include <utility>

namespace hook::quickjs {
namespace {

template <class Tuple, std::size_t... Indices>
std::array<NativeScalarKind, maxImportParameters>
nativeParameterKinds(std::index_sequence<Indices...>) noexcept
{
    std::array<NativeScalarKind, maxImportParameters> result{};
    ((result[Indices] = nativeScalarKind<std::tuple_element_t<Indices, Tuple>>),
     ...);
    return result;
}

template <class Tuple, std::size_t... Indices>
std::array<wasm_valkind_t, maxImportParameters>
wasmParameterKinds(std::index_sequence<Indices...>) noexcept
{
    std::array<wasm_valkind_t, maxImportParameters> result{};
    ((result[Indices] = wasmKind<std::tuple_element_t<Indices, Tuple>>), ...);
    return result;
}

template <QuickJSV1ImportId Id>
QuickJSV1ImportDescriptor
makeV1Descriptor()
{
    using Traits = V1ImportTraits<Id>;
    constexpr auto parameterCount =
        std::tuple_size_v<typename Traits::Parameters>;
    static_assert(parameterCount <= maxImportParameters);
    return {
        .id = Id,
        .module = Traits::module,
        .name = Traits::name,
        .category = Traits::category,
        .amendment = Traits::amendment(),
        .nativeResult = nativeScalarKind<typename Traits::Return>,
        .nativeParameters = nativeParameterKinds<typename Traits::Parameters>(
            std::make_index_sequence<parameterCount>{}),
        .resultKind = wasmKind<typename Traits::Return>,
        .parameterKinds = wasmParameterKinds<typename Traits::Parameters>(
            std::make_index_sequence<parameterCount>{}),
        .parameterCount = static_cast<std::uint8_t>(parameterCount),
        .measure = Traits::measure,
        .terminal = Traits::terminal,
        .rawOperationVersion = Traits::rawOperationVersion};
}

std::array<QuickJSV1ImportDescriptor, quickJSV1ImportCount> const&
v1SnapshotStorage()
{
    static std::array<QuickJSV1ImportDescriptor, quickJSV1ImportCount> const
        value{
            makeV1Descriptor<QuickJSV1ImportId::accept>(),
            makeV1Descriptor<QuickJSV1ImportId::rollback>(),
            makeV1Descriptor<QuickJSV1ImportId::ledger_seq>(),
            makeV1Descriptor<QuickJSV1ImportId::ledger_last_time>(),
            makeV1Descriptor<QuickJSV1ImportId::ledger_last_hash>(),
            makeV1Descriptor<QuickJSV1ImportId::otxn_type>(),
            makeV1Descriptor<QuickJSV1ImportId::hook_account>(),
            makeV1Descriptor<QuickJSV1ImportId::trace>(),
            makeV1Descriptor<QuickJSV1ImportId::state>(),
            makeV1Descriptor<QuickJSV1ImportId::state_set>(),
            makeV1Descriptor<QuickJSV1ImportId::prepare>(),
            makeV1Descriptor<QuickJSV1ImportId::etxn_reserve>(),
            makeV1Descriptor<QuickJSV1ImportId::emit>()};
    return value;
}

}  // namespace

std::span<QuickJSV1ImportDescriptor const>
quickJSHostPolicyV1Snapshot() noexcept
{
    return v1SnapshotStorage();
}

}  // namespace hook::quickjs
