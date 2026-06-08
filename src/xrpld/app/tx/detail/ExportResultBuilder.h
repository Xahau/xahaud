#ifndef RIPPLE_TX_EXPORTRESULTBUILDER_H_INCLUDED
#define RIPPLE_TX_EXPORTRESULTBUILDER_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstddef>
#include <map>

namespace ripple {
namespace ExportResultBuilder {

using SignatureSnapshot = std::map<PublicKey, Buffer>;

struct AssembledExportResult
{
    STObject metadata;
    uint256 signedTxHash;
    std::size_t signerCount = 0;
};

Buffer
signExportedTxn(
    STTx const& innerTx,
    PublicKey const& publicKey,
    SecretKey const& secretKey);

STObject
buildMultiSignedExportedTxn(
    STTx const& innerTx,
    SignatureSnapshot const& signatures);

AssembledExportResult
assemble(
    STTx const& innerTx,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq,
    uint256 const& exportTxHash);

}  // namespace ExportResultBuilder
}  // namespace ripple

#endif
