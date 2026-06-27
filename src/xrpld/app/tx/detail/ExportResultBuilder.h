#ifndef RIPPLE_TX_EXPORTRESULTBUILDER_H_INCLUDED
#define RIPPLE_TX_EXPORTRESULTBUILDER_H_INCLUDED

#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstddef>
#include <map>
#include <optional>

namespace ripple {
namespace ExportResultBuilder {

using SignatureSnapshot = std::map<PublicKey, Buffer>;

struct SignatureWitness
{
    uint256 witnessHash;
    SignatureSnapshot signatures;
};

using SignatureWitnesses = hash_map<uint256, SignatureWitness>;

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

STTx
buildSignatureWitness(
    uint256 const& exportTxHash,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq);

std::optional<SignatureSnapshot>
signaturesFromWitness(STTx const& witness);

AssembledExportResult
assemble(
    STTx const& innerTx,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq,
    uint256 const& exportTxHash,
    std::optional<uint256> const& exportSignatureHash = std::nullopt);

}  // namespace ExportResultBuilder
}  // namespace ripple

#endif
