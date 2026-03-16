#ifndef RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORT_LIMITS_H_INCLUDED

#include <cstdint>

namespace ripple {

// Export system caps.
//
// These limits bound the DoS surface of the export signature system:
// - Each pending export requires every validator to sign it every round
//   (sign-once, broadcast-many via TMValidation)
// - Inbound signature processing involves crypto verification per sig
// - The directory cap (maxPendingExports) is the root constraint;
//   signing throughput and inbound processing are transitively bounded by it
struct ExportLimits
{
    // Maximum exports a single hook execution may produce
    // (also enforced by hook_api::max_export in Enum.h)
    static constexpr std::uint8_t maxExportsPerHook = 2;

    // Maximum pending exports in the exported directory at any time.
    // This transitively caps:
    //   - signatures per TMValidation message (1 per pending export)
    //   - inbound signature processing in PeerImp (clamped to this)
    //   - validator signing work per round
    static constexpr std::uint8_t maxPendingExports = 8;
};

}  // namespace ripple

#endif
