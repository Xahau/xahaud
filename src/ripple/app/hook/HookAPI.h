#include <ripple/app/hook/Enum.h>
#include <ripple/app/tx/impl/ApplyContext.h>
#include <ripple/basics/Blob.h>
#include <ripple/basics/Expected.h>
#include <ripple/basics/Slice.h>
#include <ripple/protocol/STTx.h>

#include <ripple/app/misc/Transaction.h>
#include <cstdint>

namespace hook {
using namespace ripple;
using HookReturnCode = hook_api::hook_return_code;

struct HookContext;  // defined in applyHook.h

class HookAPI
{
public:
    explicit HookAPI(HookContext& ctx) : hookCtx(ctx)
    {
    }

    // Emit a transaction from the running hook. On success, returns 32-byte
    // transaction ID bytes (same content written by the wasm host function).
    Expected<std::shared_ptr<Transaction>, HookReturnCode>
    emit(Slice txBlob);

    // Dependencies (public so callers can compose):
    // etxn_generation == otxn_generation() + 1
    uint32_t
    etxn_generation() const;
    Expected<uint64_t, HookReturnCode>
    etxn_burden() const;
    Expected<uint64_t, HookReturnCode>
    etxn_fee_base(Slice txBlob) const;

    uint32_t
    otxn_generation() const;
    uint64_t
    otxn_burden() const;

private:
    HookContext& hookCtx;
};

}  // namespace hook
