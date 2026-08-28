#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTPOLICY_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTPOLICY_H_INCLUDED

#include <xrpld/app/hook/HookHostFunction.h>
#include <xrpl/protocol/Feature.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace hook::quickjs {

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

/** Immutable import identity for the retained xahau-raw-hook-host-v1 policy.

    This list is deliberately separate from the live 75-entry Hook catalogue.
    Once the profile is activated, later catalogue growth or reordering must
    not alter the policy selected by its retained runtime profile.
*/
enum class QuickJSV1ImportId : std::uint8_t {
    accept,
    rollback,
    ledger_seq,
    ledger_last_time,
    ledger_last_hash,
    otxn_type,
    otxn_slot,
    slot_size,
    slot,
    slot_clear,
    hook_account,
    trace,
    state,
    state_set,
    prepare,
    etxn_reserve,
    emit,
    count
};

inline constexpr std::size_t quickJSV1ImportCount =
    static_cast<std::size_t>(QuickJSV1ImportId::count);

enum class TerminalBehavior : std::uint8_t { ordinaryStatus, hookTerminal };

enum class HostWorkMeasureKind : std::uint8_t {
    zeroV1,
    argument1V1,
    arguments1And3SaturatedV1
};

template <QuickJSV1ImportId>
struct V1ImportTraits;

#define QUICKJS_V1_EXPAND_PARAMS(...) __VA_ARGS__
#define QUICKJS_V1_IMPORT_TRAITS(                                             \
    ID, RETURN_TYPE, PARAMS_TUPLE, AMENDMENT, MEASURE, TERMINAL)              \
    template <>                                                               \
    struct V1ImportTraits<QuickJSV1ImportId::ID>                              \
    {                                                                         \
        using Return = RETURN_TYPE;                                           \
        using Parameters = std::tuple<QUICKJS_V1_EXPAND_PARAMS PARAMS_TUPLE>; \
        static constexpr std::string_view module = "env";                     \
        static constexpr std::string_view name = #ID;                         \
        static ripple::uint256                                                \
        amendment()                                                           \
        {                                                                     \
            return ripple::AMENDMENT;                                         \
        }                                                                     \
        static constexpr TerminalBehavior terminal =                          \
            TerminalBehavior::TERMINAL;                                       \
        static constexpr HostWorkMeasureKind measure =                        \
            HostWorkMeasureKind::MEASURE;                                     \
        static constexpr std::uint16_t rawOperationVersion = 1;               \
    }

QUICKJS_V1_IMPORT_TRAITS(
    accept,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::int64_t),
    uint256{},
    argument1V1,
    hookTerminal);
QUICKJS_V1_IMPORT_TRAITS(
    rollback,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::int64_t),
    uint256{},
    argument1V1,
    hookTerminal);
QUICKJS_V1_IMPORT_TRAITS(
    ledger_seq,
    std::int64_t,
    (),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    ledger_last_time,
    std::int64_t,
    (),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    ledger_last_hash,
    std::int64_t,
    (std::uint32_t, std::uint32_t),
    uint256{},
    argument1V1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    otxn_type,
    std::int64_t,
    (),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    otxn_slot,
    std::int64_t,
    (std::uint32_t),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    slot_size,
    std::int64_t,
    (std::uint32_t),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    slot,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::uint32_t),
    uint256{},
    argument1V1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    slot_clear,
    std::int64_t,
    (std::uint32_t),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    hook_account,
    std::int64_t,
    (std::uint32_t, std::uint32_t),
    uint256{},
    argument1V1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    trace,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t),
    uint256{},
    arguments1And3SaturatedV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    state,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t),
    uint256{},
    arguments1And3SaturatedV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    state_set,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t),
    uint256{},
    arguments1And3SaturatedV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    prepare,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t),
    featureHooksUpdate2,
    arguments1And3SaturatedV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    etxn_reserve,
    std::int64_t,
    (std::uint32_t),
    uint256{},
    zeroV1,
    ordinaryStatus);
QUICKJS_V1_IMPORT_TRAITS(
    emit,
    std::int64_t,
    (std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t),
    uint256{},
    arguments1And3SaturatedV1,
    ordinaryStatus);

#undef QUICKJS_V1_IMPORT_TRAITS
#undef QUICKJS_V1_EXPAND_PARAMS

struct QuickJSV1ImportDescriptor
{
    QuickJSV1ImportId id;
    std::string_view module;
    std::string_view name;
    ripple::uint256 amendment;
    NativeScalarKind nativeResult;
    std::array<NativeScalarKind, maxImportParameters> nativeParameters;
    std::uint8_t parameterCount;
    HostWorkMeasureKind measure;
    TerminalBehavior terminal;
    std::uint16_t rawOperationVersion;
};

std::span<QuickJSV1ImportDescriptor const>
quickJSHostPolicyV1Snapshot() noexcept;

}  // namespace hook::quickjs

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTPOLICY_H_INCLUDED
