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

std::array<ResolvedJSImport, quickJSImportCount> const&
resolvedV1Imports()
{
    static std::array<ResolvedJSImport, quickJSImportCount> const resolved =
        [] {
            std::array<ResolvedJSImport, quickJSImportCount> result{};
            auto const catalogue = importCatalogue();
            for (std::size_t index = 0; index < catalogue.size(); ++index)
            {
                auto const& descriptor = catalogue[index];
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
        .id = "xahau-raw-hook-host-v1", .imports = resolvedV1Imports()};
    return policy;
}

bool
defineImport(
    wasmtime_linker_t* linker,
    ResolvedJSImport const& resolved,
    std::string& error)
{
    auto const& descriptor = *resolved.descriptor;
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
        "env",
        3,
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
    for (auto const& resolved : policy.imports)
        if (!defineImport(linker, resolved, error))
            return false;
    return true;
}

}  // namespace hook::quickjs
