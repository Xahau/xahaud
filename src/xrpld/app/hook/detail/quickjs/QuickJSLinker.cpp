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

std::array<WasmtimeHostBinding, quickJSV1ImportCount> const&
v1Bindings()
{
    static std::array<WasmtimeHostBinding, quickJSV1ImportCount> const
        bindings = [] {
            std::array<WasmtimeHostBinding, quickJSV1ImportCount> result{};
            auto const snapshot = quickJSHostPolicyV1Snapshot();
            for (std::size_t index = 0; index < snapshot.size(); ++index)
            {
                auto const& descriptor = snapshot[index];
                result[index] = {
                    .module = descriptor.module,
                    .name = descriptor.name,
                    .parameters = descriptor.nativeParameters,
                    .parameterCount = descriptor.parameterCount,
                    .result = descriptor.nativeResult,
                    .implementationVersion = descriptor.rawOperationVersion,
                    .operation = findHookHostFunction(
                        descriptor.name, descriptor.rawOperationVersion),
                    .amendment = descriptor.amendment,
                    .measure = descriptor.measure,
                    .terminal = descriptor.terminal,
                    .charging = WasmtimeHostBinding::Charging::quickJSV1};
            }
            return result;
        }();
    return bindings;
}

QuickJSHostAdapterPolicy const&
v1Policy()
{
    static QuickJSHostAdapterPolicy const policy{
        .id = "xahau-raw-hook-host-v1",
        .hostWorkMeter = "base-plus-addressed-byte-v1",
        .chargeOrder = HostChargeOrder::amendmentBeforeCharge,
        .debitBehavior = HostDebitBehavior::saturatedBasePlusAddressedBytes,
        .imports = v1Bindings()};
    return policy;
}

bool
defineImport(
    wasmtime_linker_t* linker,
    WasmtimeHostBinding const& binding,
    std::string& error)
{
    if (!binding.operation)
    {
        error = std::string{"missing required host binding for "} +
            std::string{binding.name};
        return false;
    }
    wasm_valtype_vec_t parameters;
    wasm_valtype_vec_new_uninitialized(&parameters, binding.parameterCount);
    for (std::size_t index = 0; index < binding.parameterCount; ++index)
        parameters.data[index] = wasm_valtype_new(
            isI32(binding.parameters[index]) ? WASM_I32 : WASM_I64);

    wasm_valtype_vec_t results;
    wasm_valtype_vec_new_uninitialized(&results, 1);
    results.data[0] =
        wasm_valtype_new(isI32(binding.result) ? WASM_I32 : WASM_I64);
    auto* type = wasm_functype_new(&parameters, &results);
    if (!type)
    {
        error = std::string{"could not create import type for "} +
            std::string{binding.name};
        return false;
    }

    auto* defineError = wasmtime_linker_define_func(
        linker,
        binding.module.data(),
        binding.module.size(),
        binding.name.data(),
        binding.name.size(),
        type,
        rawHookCallback,
        const_cast<WasmtimeHostBinding*>(&binding),
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
    if (imports.empty())
        return false;
    for (std::size_t index = 0; index < imports.size(); ++index)
    {
        auto const& binding = imports[index];
        if (!validWasmtimeHostBinding(binding))
            return false;
        for (std::size_t prior = 0; prior < index; ++prior)
            if (imports[prior].module == binding.module &&
                imports[prior].name == binding.name)
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
        error = "QuickJS host policy has an invalid host binding";
        return false;
    }
    for (auto const& binding : policy.imports)
        if (!defineImport(linker, binding, error))
            return false;
    return true;
}

}  // namespace hook::quickjs
