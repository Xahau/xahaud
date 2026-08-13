#include <xrpld/app/hook/detail/quickjs/QuickJSLinker.h>
#include <array>
#include <cstddef>

namespace hook::quickjs {
namespace {

std::string
takeError(wasmtime_error_t* error)
{
    wasm_name_t message;
    wasmtime_error_message(error, &message);
    std::string result{message.data, message.size};
    wasm_byte_vec_delete(&message);
    wasmtime_error_delete(error);
    return result;
}

std::array<ResolvedJSImport, quickJSV1ImportCount> const&
resolvedV1Imports()
{
    static std::array<ResolvedJSImport, quickJSV1ImportCount> const resolved =
        [] {
            std::array<ResolvedJSImport, quickJSV1ImportCount> result{};
            auto const snapshot = quickJSHostPolicyV1Snapshot();
            for (std::size_t index = 0; index < snapshot.size(); ++index)
            {
                auto const& descriptor = snapshot[index];
                result[index] = {
                    .descriptor = &descriptor,
                    .binding = findBinding(descriptor.id, descriptor.category)};
            }
            return result;
        }();
    return resolved;
}

QuickJSHostAdapterPolicy const&
v1Policy()
{
    static QuickJSHostAdapterPolicy const policy{
        .id = "xahau-raw-hook-host-v1",
        .hostWorkMeter = "base-plus-addressed-byte-v1",
        .chargeOrder = HostChargeOrder::amendmentBeforeCharge,
        .debitBehavior = HostDebitBehavior::saturatedBasePlusAddressedBytes,
        .imports = resolvedV1Imports()};
    return policy;
}

bool
defineImport(
    wasmtime_linker_t* linker,
    ResolvedJSImport const& resolved,
    std::string& error)
{
    auto const& descriptor = *resolved.descriptor;
    if (!resolved.binding)
    {
        error = std::string{"missing required v1 binding for "} +
            std::string{descriptor.name};
        return false;
    }
    wasm_valtype_vec_t parameters;
    wasm_valtype_vec_new_uninitialized(&parameters, descriptor.parameterCount);
    for (std::size_t index = 0; index < descriptor.parameterCount; ++index)
        parameters.data[index] =
            wasm_valtype_new(descriptor.parameterKinds[index]);

    wasm_valtype_vec_t results;
    wasm_valtype_vec_new_uninitialized(&results, 1);
    results.data[0] = wasm_valtype_new(descriptor.resultKind);
    auto* type = wasm_functype_new(&parameters, &results);
    if (!type)
    {
        error = std::string{"could not create import type for "} +
            std::string{descriptor.name};
        return false;
    }

    auto* defineError = wasmtime_linker_define_func(
        linker,
        descriptor.module.data(),
        descriptor.module.size(),
        descriptor.name.data(),
        descriptor.name.size(),
        type,
        rawHookCallback,
        const_cast<ResolvedJSImport*>(&resolved),
        nullptr);
    wasm_functype_delete(type);
    if (defineError)
    {
        error = takeError(defineError);
        return false;
    }
    return true;
}

}  // namespace

bool
QuickJSHostAdapterPolicy::complete() const noexcept
{
    if (imports.size() != quickJSV1ImportCount)
        return false;
    for (std::size_t index = 0; index < imports.size(); ++index)
    {
        auto const& resolved = imports[index];
        if (!resolved.descriptor || !resolved.binding ||
            !resolved.binding->measure || !resolved.binding->invoke ||
            static_cast<std::size_t>(resolved.descriptor->id) != index ||
            resolved.binding->id != resolved.descriptor->id ||
            resolved.binding->category != resolved.descriptor->category ||
            resolved.binding->measureKind != resolved.descriptor->measure ||
            resolved.binding->terminal != resolved.descriptor->terminal ||
            resolved.binding->rawOperationVersion !=
                resolved.descriptor->rawOperationVersion)
            return false;
    }
    return true;
}

QuickJSHostAdapterPolicy const*
findHostAdapterPolicy(std::string_view id) noexcept
{
    auto const& policy = v1Policy();
    return id == policy.id ? &policy : nullptr;
}

bool
defineImports(
    wasmtime_linker_t* linker,
    QuickJSHostAdapterPolicy const& policy,
    std::string& error)
{
    if (!policy.complete())
    {
        error = "QuickJS host policy has a missing required v1 binding";
        return false;
    }
    for (auto const& resolved : policy.imports)
        if (!defineImport(linker, resolved, error))
            return false;
    return true;
}

}  // namespace hook::quickjs
