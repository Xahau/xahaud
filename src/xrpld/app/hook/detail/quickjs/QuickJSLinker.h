#ifndef XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSLINKER_H_INCLUDED
#define XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSLINKER_H_INCLUDED

#include <xrpld/app/hook/detail/quickjs/QuickJSHostCall.h>
#include <span>
#include <string>
#include <string_view>
#include <wasmtime.h>

namespace hook::quickjs {

struct QuickJSHostAdapterPolicy
{
    std::string_view id;
    std::span<ResolvedJSImport const> imports;
};

QuickJSHostAdapterPolicy const*
findHostAdapterPolicy(std::string_view id) noexcept;

bool
defineImports(
    wasmtime_linker_t* linker,
    QuickJSHostAdapterPolicy const& policy,
    std::string& error);

}  // namespace hook::quickjs

#endif  // XRPLD_APP_HOOK_DETAIL_QUICKJS_QUICKJSLINKER_H_INCLUDED
