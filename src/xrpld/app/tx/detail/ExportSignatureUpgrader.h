#ifndef RIPPLE_TX_EXPORTSIGNATUREUPGRADER_H_INCLUDED
#define RIPPLE_TX_EXPORTSIGNATUREUPGRADER_H_INCLUDED

#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ripple {
namespace ExportSignatureUpgrader {

using IsActiveSigner = std::function<bool(PublicKey const&)>;

struct UpgradeStats
{
    std::size_t inspected = 0;
    std::size_t inactiveSkipped = 0;
    std::size_t upgraded = 0;
    std::size_t removedInvalid = 0;
};

UpgradeStats
upgradeUnverifiedSignatures(
    ExportSigCollector& collector,
    STTx const& innerTx,
    uint256 const& exportTxHash,
    LedgerIndex currentSeq,
    IsActiveSigner isActiveSigner,
    beast::Journal j);

}  // namespace ExportSignatureUpgrader
}  // namespace ripple

#endif
