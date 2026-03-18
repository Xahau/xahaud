#ifndef RIPPLE_TX_EXPORT_H_INCLUDED
#define RIPPLE_TX_EXPORT_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>

namespace ripple {

/// Retriable export transaction.
/// On open ledger: returns tesSUCCESS (provisional, consumes sequence/fee).
/// On closed ledger: checks ExportSigCollector for validator quorum.
///   - Quorum met → tesSUCCESS with sfExportResult in metadata.
///   - Not enough sigs → terRETRY_EXPORT (retained for next ledger).
///   - LLS expired → tecEXPORT_EXPIRED (sequence consumed, export failed).
/// Also supports shadow ticket cancellation via sfCancelTicketSequence.
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
