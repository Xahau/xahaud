#include <xrpld/app/hook/HookWasmEngine.h>

namespace hook {
namespace {

#ifdef ENABLE_TESTS
thread_local std::optional<HookWasmEngineKind> testEngineOverride;
#endif

}  // namespace

std::unique_ptr<HookWasmEngine>
makeHookWasmEngine()
{
#ifdef ENABLE_TESTS
    if (testEngineOverride == HookWasmEngineKind::wasmtime)
        return makeWasmtimeHookEngine();
#endif
    return makeWasmEdgeHookEngine();
}

#ifdef ENABLE_TESTS
ScopedHookWasmEngineForTests::ScopedHookWasmEngineForTests(
    HookWasmEngineKind kind) noexcept
    : previous_(testEngineOverride)
{
    testEngineOverride = kind;
}

ScopedHookWasmEngineForTests::~ScopedHookWasmEngineForTests()
{
    testEngineOverride = previous_;
}
#endif

}  // namespace hook
