#include <xrpld/app/hook/HookHostOperations/OriginatingTransaction.h>
#include <xrpld/app/hook/applyHook.h>

namespace hook::raw::v1 {

Result
otxnType(HookContext& hookCtx)
{
    return hookCtx.api().otxn_type();
}

}  // namespace hook::raw::v1
