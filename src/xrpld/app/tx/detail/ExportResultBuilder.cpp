#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>

#include <algorithm>

namespace ripple {
namespace ExportResultBuilder {
namespace {

STArray
buildSigners(SignatureSnapshot const& signatures)
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
    // cap only materializes a target-chain-valid canonical prefix.
    auto const maxSigners = STTx::maxMultiSigners();
    if (signers.size() > maxSigners)
        signers.erase(signers.begin() + maxSigners, signers.end());

    return signers;
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

STObject
buildMultiSignedExportedTxn(
    STTx const& innerTx,
    SignatureSnapshot const& signatures)
{
    auto signers = buildSigners(signatures);

    STObject multiSigned(sfExportedTxn);
    {
        Serializer s;
        innerTx.addWithoutSigningFields(s);
        SerialIter sit(s.slice());
        multiSigned.set(sit);
    }

    multiSigned.setFieldVL(sfSigningPubKey, Slice{});

    if (!signers.empty())
        multiSigned.setFieldArray(sfSigners, signers);

    return multiSigned;
}

AssembledExportResult
assemble(
    STTx const& innerTx,
    SignatureSnapshot const& signatures,
    LedgerIndex currentSeq,
    uint256 const& exportTxHash)
{
    auto multiSigned = buildMultiSignedExportedTxn(innerTx, signatures);
    auto const signerCount = multiSigned.isFieldPresent(sfSigners)
        ? multiSigned.getFieldArray(sfSigners).size()
        : 0;
    auto const signedTxHash = multiSigned.getHash(HashPrefix::transactionID);

    STObject exportResult(sfExportResult);
    exportResult.setFieldU32(sfLedgerSequence, currentSeq);
    exportResult.setFieldH256(sfTransactionHash, exportTxHash);
    exportResult.set(std::move(multiSigned));

    return {std::move(exportResult), signedTxHash, signerCount};
}

}  // namespace ExportResultBuilder
}  // namespace ripple
