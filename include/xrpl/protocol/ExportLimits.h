#ifndef RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED

#include <xrpl/protocol/ValidatorBitset.h>

#include <cstddef>
#include <cstdint>

namespace ripple {

// Export system caps.
//
// These limits bound the DoS surface of the export signature system:
// - Each pending export requires every validator to sign it every round
//   (sign-once, attach once via TMProposeSet)
// - Inbound signature processing involves crypto verification per sig
// - The open-ledger cap (maxPendingExports) is the root constraint;
//   signing throughput and inbound processing are transitively bounded by it
struct ExportLimits
{
    // V1 bitmaps are defined over at most 256 canonical pre-NegativeUNL
    // members. Raising this changes transaction/latch shape and requires a
    // protocol upgrade, not a local configuration change.
    static constexpr std::size_t maxValidatorUniverseMembers = 256;
    static constexpr std::size_t maxCommitteeMaskBytes =
        validatorBitsetBytes(maxValidatorUniverseMembers);

    // Ordinary XRPL multisigning accepts at most 32 signers. An account that
    // reserves a separate operator signer must select fewer validators.
    static constexpr std::size_t maxCommitteeMembers = 32;

    // V1 witnesses require the standard 80% quorum of the intent-selected
    // committee, rounded up. The intent selects members but cannot lower this
    // threshold. Split quotient/remainder arithmetic avoids overflow while
    // preserving ceil(memberCount * 0.8).
    static constexpr std::size_t
    committeeQuorumThreshold(std::size_t memberCount)
    {
        auto const quotient = memberCount / 5;
        auto const remainder = memberCount % 5;
        return quotient * 4 + (remainder * 4 + 4) / 5;
    }

    // Maximum exports a single hook execution may produce. Hook API ABI
    // constant hook_api::max_export must stay equal.
    static constexpr std::uint8_t maxExportsPerHook = 2;

    // Maximum pending export transactions in an open/apply ledger.
    // Hook-emitted export backlog drains into the open ledger at this cap.
    // This transitively caps:
    //   - signatures per TMProposeSet message (1 per pending export)
    //   - inbound proposal signature processing (clamped to this)
    //   - validator signing work per round
    static constexpr std::uint8_t maxPendingExports = 8;

    // Maximum number of ledgers a pending export may retry before its
    // mandatory LastLedgerSequence expires. This bounds validator signing work
    // for both hook-emitted and user-submitted exports.
    static constexpr std::uint32_t maxRetryLedgers = 5;

    // Maximum byte length of a single export-signature wire blob:
    //   txHash(32) + validator pubkey(33) + multisign signature(<= 72).
    // A fully-canonical secp256k1 signature is at most 72 bytes (ed25519 is
    // 64), so 137 is the true upper bound for a well-formed entry. The proposal
    // ingress path hashes these blobs BEFORE the proposal signature is
    // verified, so bounding the per-blob size caps pre-auth hashing/copy work
    // (DoS).
    static constexpr std::size_t maxExportSignatureBytes = 32 + 33 + 72;

    // Export-signature sidecar leaves wrap one export-signature blob in an
    // STObject envelope. Keep this comfortably above the canonical encoding
    // while bounding fetched, peer-supplied leaf bytes before parse/hash work.
    static constexpr std::size_t maxExportSignatureSidecarBytes = 256;

    // Post-validation relay framing. Values are deliberately conservative
    // local tuning knobs and require measurement before activation; changing
    // them does not change the canonical per-share format.
    static constexpr std::size_t maxCanonicalExportSignatureBytes = 72;
    // version + AccountID + 3 hashes + ledger sequence + universe position +
    // compressed public key + one-byte VL prefix + maximum signature.
    static constexpr std::size_t maxSerializedExportShareBytes = 1 + 20 + 32 +
        4 + 32 + 32 + 2 + 33 + 1 + maxCanonicalExportSignatureBytes;
    static constexpr std::size_t maxExportSharesPerRelay = 32;
    static constexpr std::size_t maxExportShareRelayPayloadBytes =
        maxSerializedExportShareBytes * maxExportSharesPerRelay;
    // Protobuf repeated-bytes framing is one tag byte plus a two-byte varint
    // length for every maximum-size frame. This excludes the overlay header.
    static constexpr std::size_t maxExportShareRelayMessageBytes =
        maxExportShareRelayPayloadBytes + 3 * maxExportSharesPerRelay;
};

}  // namespace ripple

#endif
