#ifndef RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED

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
    // Maximum exports a single hook execution may produce
    // (also enforced by hook_api::max_export in Enum.h)
    static constexpr std::uint8_t maxExportsPerHook = 2;

    // Maximum pending export transactions in an open/apply ledger.
    // Hook-emitted export backlog drains into the open ledger at this cap.
    // This transitively caps:
    //   - signatures per TMProposeSet message (1 per pending export)
    //   - inbound proposal signature processing (clamped to this)
    //   - validator signing work per round
    static constexpr std::uint8_t maxPendingExports = 8;
};

}  // namespace ripple

#endif
