#ifndef RIPPLE_APP_HOOK_DETAIL_WASMEDGEENGINE_H_INCLUDED
#define RIPPLE_APP_HOOK_DETAIL_WASMEDGEENGINE_H_INCLUDED

#include <xrpld/app/hook/detail/WasmEngine.h>
#include <memory>

namespace hook {

std::unique_ptr<IWasmEngine>
makeWasmEdgeEngine();

}  // namespace hook

#endif  // RIPPLE_APP_HOOK_DETAIL_WASMEDGEENGINE_H_INCLUDED
