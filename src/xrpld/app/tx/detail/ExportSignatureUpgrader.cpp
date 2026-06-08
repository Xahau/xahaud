#include <xrpld/app/tx/detail/ExportSignatureUpgrader.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Sign.h>

namespace ripple {
namespace ExportSignatureUpgrader {

UpgradeStats
upgradeUnverifiedSignatures(
    ExportSigCollector& collector,
    STTx const& innerTx,
    uint256 const& exportTxHash,
    LedgerIndex currentSeq,
    IsActiveSigner isActiveSigner,
    beast::Journal j)
{
    UpgradeStats stats;
    auto const unverified = collector.unverifiedSignatures(exportTxHash);
    for (auto const& [valPK, sigBuf] : unverified)
    {
        ++stats.inspected;

        if (!isActiveSigner(valPK))
        {
            ++stats.inactiveSkipped;
            continue;
        }

        auto const signerAcctID = calcAccountID(valPK);
        auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
        if (verify(valPK, sigData.slice(), Slice(sigBuf.data(), sigBuf.size())))
        {
            collector.upgradeSignature(exportTxHash, valPK, sigBuf, currentSeq);
            ++stats.upgraded;
        }
        else
        {
            JLOG(j.warn()) << "Export: upgrade verify failed"
                           << " txHash=" << exportTxHash
                           << " signer=" << calcNodeID(valPK)
                           << " ledgerSeq=" << currentSeq
                           << " action=remove-invalid-sig";
            if (collector.removeSignature(exportTxHash, valPK, sigBuf))
                ++stats.removedInvalid;
        }
    }

    return stats;
}

}  // namespace ExportSignatureUpgrader
}  // namespace ripple
