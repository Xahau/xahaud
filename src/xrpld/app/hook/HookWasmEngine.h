#ifndef XRPLD_APP_HOOK_HOOKWASMENGINE_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKWASMENGINE_H_INCLUDED

#include <xrpld/app/hook/HookHostTypes.h>
#include <xrpl/beast/utility/Journal.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hook {

struct HookContext;

enum class HookWasmEngineKind : std::uint8_t { wasmEdge, wasmtime };

struct HookWasmExecutionResult
{
    bool ok;
    std::uint64_t instructionCount;
    std::optional<std::string> error;
    std::optional<HookHostValue> returnValue = {};
};

struct HookWasmModuleSurface
{
    struct Import
    {
        std::string module;
        std::string name;
        std::vector<HookHostValueKind> parameters;
        std::optional<HookHostValueKind> result;
    };

    std::vector<Import> imports;
    std::vector<std::string> exports;
};

class HookWasmEngine
{
public:
    virtual ~HookWasmEngine() = default;

    virtual std::optional<std::string>
    validate(void const* wasm, std::size_t length) = 0;

    virtual HookWasmExecutionResult
    execute(
        void const* wasm,
        std::size_t length,
        bool callback,
        std::uint32_t wasmParameter,
        HookContext& hookContext,
        beast::Journal const& journal) = 0;
};

std::unique_ptr<HookWasmEngine>
makeWasmEdgeHookEngine();

std::unique_ptr<HookWasmEngine>
makeWasmtimeHookEngine();

std::optional<std::string>
inspectWasmtimeHookModule(
    std::span<std::uint8_t const> wasm,
    HookWasmModuleSurface& surface);

/** Invoke one named () -> i64 export through the all-enabled raw test policy.

    This is test evidence for the independent 75-wrapper fixture. Production C
    Hook execution remains the `hook`/`cbak` path on WasmEdge.
 */
HookWasmExecutionResult
invokeWasmtimeHookTestExport(
    std::span<std::uint8_t const> wasm,
    std::string_view exportName,
    HookContext& hookContext,
    beast::Journal const& journal);

/** Select the C-Hook engine at the outer execution/admission boundary.

    Production always returns WasmEdge. Tests may install a scoped,
    thread-local override to run the same Env flow through Wasmtime without
    putting an engine, Store, Instance, or SDK object in HookContext.
 */
std::unique_ptr<HookWasmEngine>
makeHookWasmEngine();

#ifdef ENABLE_TESTS
class ScopedHookWasmEngineForTests
{
public:
    explicit ScopedHookWasmEngineForTests(HookWasmEngineKind kind) noexcept;
    ~ScopedHookWasmEngineForTests();

    ScopedHookWasmEngineForTests(ScopedHookWasmEngineForTests const&) = delete;
    ScopedHookWasmEngineForTests&
    operator=(ScopedHookWasmEngineForTests const&) = delete;

private:
    std::optional<HookWasmEngineKind> previous_;
};
#endif

}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKWASMENGINE_H_INCLUDED
