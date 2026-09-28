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
#include <cstdint>
#include <map>
#include <optional>

namespace ripple {
namespace ExportResultBuilder {

using SignatureSnapshot = std::map<PublicKey, Buffer>;

struct PositionedSignature
{
    PublicKey signingKey;
    Buffer signature;
};

using PositionedSignatureSnapshot =
    std::map<std::uint16_t, PositionedSignature>;

Buffer
signExportedTxn(
    STTx const& innerTx,
    PublicKey const& publicKey,
    SecretKey const& secretKey);

uint256
exportIntentHash(STTx const& innerTx);

STObject
buildMultiSignedExportedTxn(
    STTx const& innerTx,
    SignatureSnapshot const& signatures);

STTx
buildSignatureWitness(
    uint256 const& exportTxHash,
    STTx const& exportSigningPayload,
    PositionedSignatureSnapshot const& signatures,
    std::size_t committeeSize,
    LedgerIndex currentSeq);

std::optional<PositionedSignatureSnapshot>
signaturesFromWitness(STTx const& witness);

}  // namespace ExportResultBuilder
}  // namespace ripple

#endif
