#ifndef RIPPLE_APP_HOOK_DETAIL_WASMTIMEENGINE_H_INCLUDED
#define RIPPLE_APP_HOOK_DETAIL_WASMTIMEENGINE_H_INCLUDED

#include <xrpld/app/hook/detail/WasmEngine.h>
#include <memory>

namespace hook {

std::unique_ptr<IWasmEngine>
makeWasmtimeEngine();

}  // namespace hook

#endif  // RIPPLE_APP_HOOK_DETAIL_WASMTIMEENGINE_H_INCLUDED
