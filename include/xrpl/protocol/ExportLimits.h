#ifndef RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED

#include <xrpl/protocol/ValidatorBitset.h>

#include <cstddef>
#include <cstdint>

namespace ripple {

// Export system caps.
//
// These limits bound the DoS surface of the export signature system:
// - Each selected validator signs once after exact source validation
// - Pending shares are re-advertised through bounded proposal/relay batches
// - Inbound signature processing involves crypto verification per sig
// - Durable and per-message caps bound scans, crypto, and sidecar leaves
struct ExportLimits
{
    // Ordinary XRPL multisigning accepts at most 32 signers. An account that
    // reserves a separate operator signer must select fewer validators.
    static constexpr std::size_t maxCommitteeMembers = 32;
    static constexpr std::size_t maxCommitteeRosterBytes =
        maxCommitteeMembers * 33;
    static constexpr std::size_t maxCommitteeContributorBytes =
        validatorBitsetBytes(maxCommitteeMembers);

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

    // Maximum Export intents admitted in one ledger and maximum live latches
    // owned by one account.
    static constexpr std::uint8_t maxPendingExports = 8;

    // Global pending-latch and per-validation scan bound. Witnessed or canceled
    // latches release this signing-work slot while their account owner count
    // and reserve continue to bound retained state. This is a provisional
    // activation tuning value.
    static constexpr std::uint16_t maxLiveExportLatches = 64;

    // Maximum admission window requested through the mandatory outer
    // LastLedgerSequence. This bounds how long an Export may remain queued
    // before entering a ledger; it does not bound post-validation release.
    static constexpr std::uint32_t maxRetryLedgers = 5;

    // Fixed source-ledger window for post-validation share publication and
    // witness materialization, measured from the ledger that admits the
    // intent. This is a provisional activation tuning value. Keeping it
    // separate from the outer LastLedgerSequence prevents queue delay from
    // consuming the publication window.
    static constexpr std::uint32_t maxPublicationLedgers = 5;

    // A fully-canonical secp256k1 signature is at most 72 bytes; Ed25519 is 64.
    static constexpr std::size_t maxCanonicalExportSignatureBytes = 72;

    // Export-signature sidecar leaves encode an origin, committee position,
    // signing key, and signature in an STObject envelope. Keep this comfortably
    // above the canonical encoding while bounding peer-supplied leaf bytes
    // before parse/hash work.
    static constexpr std::size_t maxExportSignatureSidecarBytes = 256;

    // Post-validation relay framing. Values are deliberately conservative
    // local tuning knobs and require measurement before activation; changing
    // them does not change the canonical per-share format.
    // version + AccountID + 2 hashes + ledger sequence + committee position +
    // compressed public key + one-byte VL prefix + maximum signature.
    static constexpr std::size_t maxSerializedExportShareBytes = 1 + 20 + 32 +
        4 + 32 + 2 + 33 + 1 + maxCanonicalExportSignatureBytes;
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
