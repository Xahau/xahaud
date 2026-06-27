#ifndef RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED

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

    // Maximum byte length of a single export-signature wire blob:
    //   txHash(32) + validator pubkey(33) + multisign signature(<= 72).
    // A fully-canonical secp256k1 signature is at most 72 bytes (ed25519 is
    // 64), so 137 is the true upper bound for a well-formed entry. The proposal
    // ingress path hashes these blobs BEFORE the proposal signature is
    // verified, so bounding the per-blob size caps pre-auth hashing/copy work
    // (DoS).
    static constexpr std::size_t maxExportSignatureBytes = 32 + 33 + 72;
};

}  // namespace ripple

#endif
