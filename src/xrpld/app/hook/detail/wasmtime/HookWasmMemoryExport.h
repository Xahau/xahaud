#ifndef XRPLD_APP_HOOK_DETAIL_WASMTIME_HOOKWASMMEMORYEXPORT_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_WASMTIME_HOOKWASMMEMORYEXPORT_H_INCLUDED

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hook::wasmtime {

struct MemoryExportResult
{
    std::vector<std::uint8_t> bytes;
    std::optional<std::string> error;
};

/** Return an ephemeral module image that exports its sole memory as `memory`.

    C Hook identity, storage, hashing, and guard validation continue to use the
    original bytes. This view exists only because Wasmtime host callbacks can
    recover caller memory through an export, unlike WasmEdge's indexed calling
    frame API.
 */
MemoryExportResult
ensureHookMemoryExport(std::span<std::uint8_t const> wasm);

}  // namespace hook::wasmtime

#endif
