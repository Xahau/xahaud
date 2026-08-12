#ifndef XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_ORIGINATINGTRANSACTION_H_INCLUDED
#define XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_ORIGINATINGTRANSACTION_H_INCLUDED

#include <xrpld/app/hook/HookHostOperationResult.h>

namespace hook {
struct HookContext;

namespace raw {
Result
otxnType(HookContext& hookCtx);
}  // namespace raw
}  // namespace hook

#endif  // XRPLD_APP_HOOK_HOOKHOSTOPERATIONS_ORIGINATINGTRANSACTION_H_INCLUDED
