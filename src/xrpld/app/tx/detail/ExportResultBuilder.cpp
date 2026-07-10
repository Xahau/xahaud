#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>

#include <algorithm>

namespace ripple {
namespace ExportResultBuilder {
namespace {

constexpr std::size_t maxTxnSignatureBytes =
    ExportLimits::maxExportSignatureBytes - 32 - 33;

STArray
buildSigners(SignatureSnapshot const& signatures, bool capForTargetChain)
{
    STArray signers(sfSigners);
    for (auto const& [valPK, sigBuf] : signatures)
    {
        if (sigBuf.empty())
            continue;

        STObject signer(sfSigner);
        signer.setAccountID(sfAccount, calcAccountID(valPK));
        signer.setFieldVL(sfSigningPubKey, valPK.slice());
        signer.setFieldVL(sfTxnSignature, Slice(sigBuf.data(), sigBuf.size()));
        signers.push_back(std::move(signer));
    }

    std::sort(
        signers.begin(),
        signers.end(),
        [](STObject const& a, STObject const& b) {
            return a.getAccountID(sfAccount) < b.getAccountID(sfAccount);
        });

    // XRPL validates the Signers array size before checking signer weights.
    // Export quorum is decided earlier from the agreed sidecar snapshot; this
    // cap only materializes a protocol-size-valid canonical prefix. The
    // destination account's SignerList/quorum remains an operator contract.
    if (capForTargetChain)
    {
        auto const maxSigners = STTx::maxMultiSigners();
        if (signers.size() > maxSigners)
            signers.erase(signers.begin() + maxSigners, signers.end());
    }

    return signers;
}

STObject
normalizeAuthorizationEnvelope(STTx const& innerTx)
{
    STObject normalized(sfExportedTxn);
    {
        Serializer inner;
        innerTx.add(inner);
        SerialIter sit(inner.slice());
        normalized.set(sit);
    }
    normalized.delField(sfTxnSignature);
    normalized.delField(sfSigners);
    normalized.setFieldVL(sfSigningPubKey, Slice{});
    return normalized;
}

}  // namespace

Buffer
signExportedTxn(
    STTx const& innerTx,
    PublicKey const& publicKey,
    SecretKey const& secretKey)
{
    auto const signerAcctID = calcAccountID(publicKey);
    auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
    return ripple::sign(publicKey, secretKey, sigData.slice());
}

uint256
exportIntentHash(STTx const& innerTx)
{
    // Normalize every authorization envelope, then use the protocol's existing
    // signing hash. The identity is stable across valid signer subsets.
    return STTx{normalizeAuthorizationEnvelope(innerTx)}.getSigningHash();
}

STObject
buildMultiSignedExportedTxn(
    STTx const& innerTx,
    SignatureSnapshot const& signatures)
{
    auto signers = buildSigners(signatures, true);
    auto multiSigned = normalizeAuthorizationEnvelope(innerTx);

    if (!signers.empty())
        multiSigned.setFieldArray(sfSigners, signers);

    return multiSigned;
}

STTx
buildSignatureWitness(
    uint256 const& exportTxHash,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq)
{
    return STTx(ttEXPORT_SIGNATURES, [&](auto& obj) {
        obj.setFieldU32(sfLedgerSequence, currentSeq);
        obj.setAccountID(sfAccount, AccountID{});
        obj.setFieldU32(sfSequence, 0);
        obj.setFieldAmount(sfFee, STAmount{});
        obj.setFieldH256(sfTransactionHash, exportTxHash);
        obj.setFieldArray(sfSigners, buildSigners(signatures, false));
    });
}

std::optional<SignatureSnapshot>
signaturesFromWitness(STTx const& witness)
{
    if (witness.getTxnType() != ttEXPORT_SIGNATURES ||
        !witness.isFieldPresent(sfSigners))
        return std::nullopt;

    SignatureSnapshot signatures;
    for (auto const& signer : witness.getFieldArray(sfSigners))
    {
        if (signer.getFName() != sfSigner ||
            !signer.isFieldPresent(sfAccount) ||
            !signer.isFieldPresent(sfSigningPubKey) ||
            !signer.isFieldPresent(sfTxnSignature))
            return std::nullopt;

        auto const pkBlob = signer.getFieldVL(sfSigningPubKey);
        if (!publicKeyType(makeSlice(pkBlob)))
            return std::nullopt;

        if (signer.getAccountID(sfAccount) !=
            calcAccountID(PublicKey(makeSlice(pkBlob))))
            return std::nullopt;

        auto const sigBlob = signer.getFieldVL(sfTxnSignature);
        if (sigBlob.empty() || sigBlob.size() > maxTxnSignatureBytes)
            return std::nullopt;

        auto const [_, inserted] = signatures.emplace(
            PublicKey(makeSlice(pkBlob)),
            Buffer(sigBlob.data(), sigBlob.size()));
        if (!inserted)
            return std::nullopt;
    }

    if (signatures.empty())
        return std::nullopt;

    return signatures;
}

namespace {

AssembledExportResult
assembleImpl(
    STTx const& innerTx,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq,
    uint256 const& exportTxHash,
    std::optional<uint256> const& exportSignatureHash)
{
    auto multiSigned = buildMultiSignedExportedTxn(innerTx, signatures);
    auto const signerCount = multiSigned.isFieldPresent(sfSigners)
        ? multiSigned.getFieldArray(sfSigners).size()
        : 0;
    auto const signedTxHash = multiSigned.getHash(HashPrefix::transactionID);

    STObject exportResult(sfExportResult);
    exportResult.setFieldU32(sfLedgerSequence, currentSeq);
    exportResult.setFieldH256(sfTransactionHash, exportTxHash);
    if (exportSignatureHash)
        exportResult.setFieldH256(sfExportSignatureHash, *exportSignatureHash);
    else
        exportResult.set(std::move(multiSigned));

    return {std::move(exportResult), signedTxHash, signerCount};
}

}  // namespace

AssembledExportResult
assembleDirect(
    STTx const& innerTx,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq,
    uint256 const& exportTxHash)
{
    return assembleImpl(
        innerTx, signatures, currentSeq, exportTxHash, std::nullopt);
}

AssembledExportResult
assembleClosedLedger(
    STTx const& innerTx,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq,
    uint256 const& exportTxHash,
    uint256 const& exportSignatureHash)
{
    return assembleImpl(
        innerTx, signatures, currentSeq, exportTxHash, exportSignatureHash);
}

}  // namespace ExportResultBuilder
}  // namespace ripple
