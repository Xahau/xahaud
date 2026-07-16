#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpl/basics/contract.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/ValidatorBitset.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ripple {
namespace ExportResultBuilder {
namespace {

constexpr std::size_t maxTxnSignatureBytes =
    ExportLimits::maxExportSignatureBytes - 32 - 33;

struct BuiltWitnessSignatures
{
    STArray entries;
    Blob contributors;
};

BuiltWitnessSignatures
buildWitnessSignatures(
    PositionedSignatureSnapshot const& signatures,
    std::size_t const universeSize)
{
    if (universeSize == 0 ||
        universeSize > ExportLimits::maxValidatorUniverseMembers ||
        signatures.empty() ||
        signatures.size() > ExportLimits::maxCommitteeMembers)
        Throw<std::invalid_argument>("invalid Export witness dimensions");

    STArray entries(sfExportSigners);
    Blob contributors(validatorBitsetBytes(universeSize), 0);
    std::set<PublicKey> signingKeys;
    hash_set<AccountID> signerAccounts;

    for (auto const& [position, witness] : signatures)
    {
        if (position >= universeSize || witness.signature.empty() ||
            witness.signature.size() > maxTxnSignatureBytes ||
            !signingKeys.insert(witness.signingKey).second ||
            !signerAccounts.insert(calcAccountID(witness.signingKey)).second)
            Throw<std::invalid_argument>("invalid Export witness signer");

        contributors[position / 8] |=
            static_cast<std::uint8_t>(1u << (position % 8));

        STObject entry(sfExportSigner);
        entry.setFieldVL(sfSigningPubKey, witness.signingKey.slice());
        entry.setFieldVL(
            sfTxnSignature,
            Slice{witness.signature.data(), witness.signature.size()});
        entries.push_back(std::move(entry));
    }

    return {std::move(entries), std::move(contributors)};
}

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
    STTx const& exportSigningPayload,
    PositionedSignatureSnapshot const& signatures,
    std::size_t const universeSize,
    LedgerIndex currentSeq)
{
    auto witnessSignatures = buildWitnessSignatures(signatures, universeSize);
    auto const target = normalizeAuthorizationEnvelope(exportSigningPayload);
    return STTx(ttEXPORT_SIGNATURES, [&](auto& obj) {
        obj.setFieldU32(sfLedgerSequence, currentSeq);
        obj.setAccountID(sfAccount, AccountID{});
        obj.setFieldU32(sfSequence, 0);
        obj.setFieldAmount(sfFee, STAmount{});
        obj.setFieldH256(sfTransactionHash, exportTxHash);
        obj.set(std::make_unique<STObject>(target));
        obj.setFieldVL(sfEntropyContributors, witnessSignatures.contributors);
        obj.setFieldArray(
            sfExportSigners, std::move(witnessSignatures.entries));
    });
}

std::optional<PositionedSignatureSnapshot>
signaturesFromWitness(STTx const& witness)
{
    if (witness.getTxnType() != ttEXPORT_SIGNATURES ||
        !witness.isFieldPresent(sfExportedTxn) ||
        !witness.isFieldPresent(sfEntropyContributors) ||
        !witness.isFieldPresent(sfExportSigners))
        return std::nullopt;

    auto const& exported = const_cast<STTx&>(witness)
                               .peekAtField(sfExportedTxn)
                               .downcast<STObject>();
    if (exported.isFieldPresent(sfTxnSignature) ||
        exported.isFieldPresent(sfSigners) ||
        !exported.isFieldPresent(sfSigningPubKey) ||
        !exported.getFieldVL(sfSigningPubKey).empty())
        return std::nullopt;

    auto const& contributors = witness.getFieldVL(sfEntropyContributors);
    auto const& entries = witness.getFieldArray(sfExportSigners);
    if (contributors.empty() ||
        contributors.size() > ExportLimits::maxCommitteeMaskBytes ||
        entries.empty() || entries.size() > ExportLimits::maxCommitteeMembers)
        return std::nullopt;

    std::vector<std::uint16_t> positions;
    positions.reserve(entries.size());
    for (std::size_t byte = 0; byte < contributors.size(); ++byte)
    {
        for (std::size_t bit = 0; bit < 8; ++bit)
        {
            if ((contributors[byte] & static_cast<std::uint8_t>(1u << bit)) !=
                0)
                positions.push_back(static_cast<std::uint16_t>(byte * 8 + bit));
        }
    }
    if (positions.size() != entries.size())
        return std::nullopt;

    PositionedSignatureSnapshot signatures;
    std::set<PublicKey> signingKeys;
    hash_set<AccountID> signerAccounts;
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        auto const& entry = entries[i];
        if (entry.getFName() != sfExportSigner || entry.getCount() != 2 ||
            !entry.isFieldPresent(sfSigningPubKey) ||
            !entry.isFieldPresent(sfTxnSignature))
            return std::nullopt;

        auto const pkBlob = entry.getFieldVL(sfSigningPubKey);
        if (!publicKeyType(makeSlice(pkBlob)))
            return std::nullopt;
        PublicKey const signingKey{makeSlice(pkBlob)};
        if (!signingKeys.insert(signingKey).second ||
            !signerAccounts.insert(calcAccountID(signingKey)).second)
            return std::nullopt;

        auto const sigBlob = entry.getFieldVL(sfTxnSignature);
        if (sigBlob.empty() || sigBlob.size() > maxTxnSignatureBytes)
            return std::nullopt;

        auto const [_, inserted] = signatures.emplace(
            positions[i],
            PositionedSignature{
                signingKey, Buffer(sigBlob.data(), sigBlob.size())});
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
