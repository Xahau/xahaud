#ifndef RIPPLE_APP_PROOF_LEDGERPROOF_H_INCLUDED
#define RIPPLE_APP_PROOF_LEDGERPROOF_H_INCLUDED

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/proof/ProofBuilder.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/STTx.h>
#include <optional>

namespace ripple {
namespace proof {

/// Proof-of-ledger: everything needed to prove a transaction (or account
/// state) was in a specific closed ledger. This is the core building
/// block for XPOPs and future proof formats.
struct LedgerProof
{
    // --- Ledger Header ---
    std::uint32_t ledgerIndex{0};
    std::uint64_t totalCoins{0};
    uint256 parentHash;
    uint256 txRoot;
    uint256 accountRoot;
    std::uint32_t parentCloseTime{0};
    std::uint32_t closeTime{0};
    std::uint8_t closeTimeResolution{0};
    std::uint8_t closeFlags{0};

    /// Recompute the ledger hash from header fields.
    uint256
    computeLedgerHash() const;

    // --- Transaction Proof ---
    /// The transaction blob (serialized STTx).
    Blob txBlob;

    /// The transaction metadata blob.
    Blob metaBlob;

    /// Merkle proof from the transaction tree.
    std::optional<MerkleProof> txProof;
};

/// Build a LedgerProof for a specific transaction in a closed ledger.
/// Returns nullopt if the transaction is not found.
std::optional<LedgerProof>
buildLedgerProof(Ledger const& ledger, uint256 const& txHash);

}  // namespace proof
}  // namespace ripple

#endif
