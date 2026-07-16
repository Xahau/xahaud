#ifndef RIPPLE_TX_EXPORT_H_INCLUDED
#define RIPPLE_TX_EXPORT_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>

namespace ripple {

/// Admit an Export intent or cancel an Export latch.
///
/// A successful intent creates an origin-keyed pending latch. Validators sign
/// only after its ledger validates; a later ttEXPORT_SIGNATURES pseudo records
/// the canonical target-chain assembly and marks the latch witnessed. The outer
/// LastLedgerSequence bounds admission, not signature publication.
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
