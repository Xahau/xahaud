#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTCALL_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTCALL_H_INCLUDED

#include <xrpld/app/hook/HookGuestMemory.h>
#include <xrpld/app/hook/HookHostOperationResult.h>
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSImportCatalogue.h>
#include <xrpl/beast/utility/Journal.h>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wasmtime.h>

namespace hook {
struct HookContext;

namespace quickjs {

struct QuickJSInvocation
{
    HookContext& hookCtx;
    beast::Journal const& journal;
    QuickJSRuntimeProfile const& profile;
    std::uint64_t hostWorkRemaining;
    bool terminal = false;

    QuickJSInvocation(
        HookContext& hookCtx,
        beast::Journal const& journal,
        QuickJSRuntimeProfile const& profile) noexcept;
};

class QuickJSHostCall
{
public:
    QuickJSHostCall(
        QuickJSInvocation& invocation,
        wasmtime_caller_t* caller) noexcept;

    HookContext&
    hookContext() noexcept;

    beast::Journal const&
    journal() const noexcept;

    std::optional<HookGuestMemory>
    memory() const noexcept;

    bool
    charge(std::uint64_t declaredBytes) noexcept;

    char const*
    fault() const noexcept;

private:
    QuickJSInvocation& invocation_;
    wasmtime_caller_t* caller_;
    char const* fault_ = nullptr;
};

using ErasedJSImportHandler =
    raw::Result (*)(QuickJSHostCall&, std::span<wasmtime_val_t const>);

struct QuickJSImportBinding
{
    QuickJSImportId id;
    ImportCategory category;
    ErasedJSImportHandler invoke;
};

template <class Tuple>
struct BindingSignatures;

template <class... Args>
struct BindingSignatures<std::tuple<Args...>>
{
    using Handler = raw::Result (*)(QuickJSHostCall&, Args...);
    using Measure = std::uint64_t (*)(Args...) noexcept;
};

template <class T>
T
decodeArgument(wasmtime_val_t const& value) noexcept
{
    if constexpr (std::is_same_v<T, std::uint32_t>)
        return static_cast<std::uint32_t>(value.of.i32);
    else if constexpr (std::is_same_v<T, std::int32_t>)
        return value.of.i32;
    else if constexpr (std::is_same_v<T, std::uint64_t>)
        return static_cast<std::uint64_t>(value.of.i64);
    else
    {
        static_assert(std::is_same_v<T, std::int64_t>);
        return value.of.i64;
    }
}

template <class Tuple, std::size_t... Indices>
Tuple
decodeArguments(
    std::span<wasmtime_val_t const> values,
    std::index_sequence<Indices...>) noexcept
{
    return Tuple{decodeArgument<std::tuple_element_t<Indices, Tuple>>(
        values[Indices])...};
}

template <QuickJSImportId Id, auto Handler, auto Measure>
raw::Result
invokeBinding(QuickJSHostCall& call, std::span<wasmtime_val_t const> values)
{
    using Parameters = typename ImportTraits<Id>::Parameters;
    constexpr auto count = std::tuple_size_v<Parameters>;
    auto const arguments =
        decodeArguments<Parameters>(values, std::make_index_sequence<count>{});
    auto const declaredBytes = std::apply(Measure, arguments);
    if (!call.charge(declaredBytes))
        return hook_api::hook_return_code::INTERNAL_ERROR;
    return std::apply(
        [&](auto... args) { return Handler(call, args...); }, arguments);
}

template <QuickJSImportId Id, auto Handler, auto Measure>
constexpr QuickJSImportBinding
makeBinding() noexcept
{
    using Signatures = BindingSignatures<typename ImportTraits<Id>::Parameters>;
    static_assert(
        std::is_same_v<decltype(Handler), typename Signatures::Handler>);
    static_assert(
        std::is_same_v<decltype(Measure), typename Signatures::Measure>);
    return {
        .id = Id,
        .category = ImportTraits<Id>::category,
        .invoke = &invokeBinding<Id, Handler, Measure>};
}

std::span<QuickJSImportBinding const>
controlBindings() noexcept;
std::span<QuickJSImportBinding const>
emissionBindings() noexcept;
std::span<QuickJSImportBinding const>
hookContextBindings() noexcept;
std::span<QuickJSImportBinding const>
ledgerBindings() noexcept;
std::span<QuickJSImportBinding const>
originatingTransactionBindings() noexcept;
std::span<QuickJSImportBinding const>
stateBindings() noexcept;
std::span<QuickJSImportBinding const>
traceBindings() noexcept;

QuickJSImportBinding const*
findBinding(QuickJSImportId id, ImportCategory category) noexcept;

struct ResolvedJSImport
{
    QuickJSImportDescriptor const* descriptor;
    QuickJSImportBinding const* binding;
};

wasm_trap_t*
rawHookCallback(
    void* environment,
    wasmtime_caller_t* caller,
    wasmtime_val_t const* args,
    std::size_t argumentCount,
    wasmtime_val_t* results,
    std::size_t resultCount) noexcept;

}  // namespace quickjs
}  // namespace hook

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSHOSTCALL_H_INCLUDED
