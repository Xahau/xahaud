#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSRUNTIMEINTERNAL_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSRUNTIMEINTERNAL_H_INCLUDED

#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/quickjs/QuickJSLinker.h>
#include <memory>
#include <utility>
#include <wasmtime.h>

namespace hook {

using QuickJSEnginePtr =
    std::unique_ptr<wasm_engine_t, decltype(&wasm_engine_delete)>;
using QuickJSModulePtr =
    std::unique_ptr<wasmtime_module_t, decltype(&wasmtime_module_delete)>;

class QuickJSProviderRuntime
{
public:
    QuickJSRuntimeProfile const profile;
    quickjs::QuickJSHostAdapterPolicy const& hostPolicy;
    QuickJSEnginePtr engine;
    QuickJSModulePtr module;

    QuickJSProviderRuntime(
        QuickJSRuntimeProfile profile,
        quickjs::QuickJSHostAdapterPolicy const& hostPolicy,
        QuickJSEnginePtr&& engine,
        QuickJSModulePtr&& module)
        : profile(std::move(profile))
        , hostPolicy(hostPolicy)
        , engine(std::move(engine))
        , module(std::move(module))
    {
    }
};

}  // namespace hook

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSRUNTIMEINTERNAL_H_INCLUDED
