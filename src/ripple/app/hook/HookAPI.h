// A decoupled Hook host API helper for programmatic use and testing.
// Provides selected Hook APIs (emit-related and dependencies) without any
// dependency on WASM memory. It operates directly on ripple types.

#pragma once

#include <ripple/app/tx/impl/ApplyContext.h>
#include <ripple/basics/Blob.h>
#include <ripple/basics/Expected.h>
#include <ripple/basics/Slice.h>
#include <ripple/protocol/STTx.h>

#include <ripple/app/misc/Transaction.h>
#include <cstdint>

namespace hook {

struct HookContext;  // defined in applyHook.h

class HookAPI
{
public:
    explicit HookAPI(HookContext& ctx) : hookCtx(ctx)
    {
    }

    // Emit a transaction from the running hook. On success, returns 32-byte
    // transaction ID bytes (same content written by the wasm host function).
    ripple::Expected<std::shared_ptr<ripple::Transaction>, std::int64_t>
    emit(ripple::Slice txBlob);

    // Dependencies (public so callers can compose):
    // etxn_generation == otxn_generation() + 1
    std::int64_t
    etxn_generation() const;
    std::int64_t
    etxn_burden() const;
    std::int64_t
    etxn_fee_base(ripple::Slice txBlob) const;

    std::int64_t
    otxn_generation() const;
    std::int64_t
    otxn_burden() const;

private:
    HookContext& hookCtx;
};

}  // namespace hook
