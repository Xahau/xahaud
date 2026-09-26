#include <xrpld/app/proof/LedgerProof.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/digest.h>

namespace ripple {
namespace proof {

uint256
LedgerProof::computeLedgerHash() const
{
    return sha512Half(
        HashPrefix::ledgerMaster,
        ledgerIndex,
        totalCoins,
        parentHash,
        txRoot,
        accountRoot,
        parentCloseTime,
        closeTime,
        closeTimeResolution,
        closeFlags);
}

std::optional<LedgerProof>
buildLedgerProof(Ledger const& ledger, uint256 const& txHash)
{
    auto const& info = ledger.info();

    // Find the transaction in the ledger.
    auto const txResult = ledger.txRead(txHash);
    if (!txResult.first)
        return std::nullopt;

    LedgerProof proof;

    // Ledger header fields.
    proof.ledgerIndex = info.seq;
    proof.totalCoins = info.drops.drops();
    proof.parentHash = info.parentHash;
    proof.txRoot = info.txHash;
    proof.accountRoot = info.accountHash;
    proof.parentCloseTime = info.parentCloseTime.time_since_epoch().count();
    proof.closeTime = info.closeTime.time_since_epoch().count();
    proof.closeTimeResolution = info.closeTimeResolution.count();
    proof.closeFlags = info.closeFlags;

    // Transaction blob.
    {
        Serializer s;
        txResult.first->add(s);
        proof.txBlob = s.peekData();
    }

    // Transaction metadata.
    if (txResult.second)
    {
        Serializer s;
        txResult.second->add(s);
        proof.metaBlob = s.peekData();
    }

    // Merkle proof from the transaction SHAMap (v1 format).
    auto const& txMap = ledger.txMap();
    auto const txKey =
        sha512Half(HashPrefix::transactionID, makeSlice(proof.txBlob));
    proof.txProof = extractProofV1(txMap, txKey);

    return proof;
}

}  // namespace proof
}  // namespace ripple
