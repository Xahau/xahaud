#ifndef RIPPLE_APP_HOOK_DETAIL_WASMENGINE_H_INCLUDED
#define RIPPLE_APP_HOOK_DETAIL_WASMENGINE_H_INCLUDED

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/hook/WasmTypes.h>
#include <xrpl/protocol/Rules.h>
#include <optional>
#include <string>
#include <vector>

namespace hook {

/// Initial fuel budget per Hook execution (consensus-fixed; changing
/// this value requires a separate Amendment).
constexpr uint64_t kWasmtimeInitialFuel = 10'000'000'000ULL;

struct HookContext;

struct ExecutionResult
{
    bool ok;
    uint64_t instructionCount;
    std::optional<std::string> error;
};

class IWasmEngine
{
public:
    virtual ~IWasmEngine() = default;

    virtual std::optional<std::string>
    validate(void const* wasm, size_t len) = 0;

    virtual ExecutionResult
    execute(
        void const* wasm,
        size_t len,
        bool isCallback,
        uint32_t wasmParam,
        HookContext& ctx,
        std::vector<HostFunctionDecl> const& imports,
        ripple::Rules const& rules,
        beast::Journal const& j) = 0;
};

std::vector<HostFunctionDecl> const&
hookHostFunctionDecls();

}  // namespace hook

#endif  // RIPPLE_APP_HOOK_DETAIL_WASMENGINE_H_INCLUDED
