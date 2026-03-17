#ifndef RIPPLE_TX_EXPORT_H_INCLUDED
#define RIPPLE_TX_EXPORT_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>

namespace ripple {

/// User-submittable export transaction.
/// Creates an ltEXPORTED_TXN entry for validator signing.
/// This is the transaction-based entry point for non-hook users;
/// hooks use the xport() API which creates the same ledger state inline.
class Export : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit Export(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace ripple

#endif
