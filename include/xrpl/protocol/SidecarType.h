#ifndef RIPPLE_PROTOCOL_SIDECAR_TYPE_H_INCLUDED
#define RIPPLE_PROTOCOL_SIDECAR_TYPE_H_INCLUDED

#include <cstdint>

namespace ripple {

/// Discriminator for sidecar set entries (SHAMap leaves used for
/// consensus extension data: RNG commit/reveal, export signatures).
///
/// Stored in sfSidecarType (UINT8) on each STObject entry.
/// Makes sidecar sets self-describing — no content-sniffing needed.
enum SidecarType : std::uint8_t {
    sidecarRngCommit = 1,
    sidecarRngReveal = 2,
    sidecarExportSig = 3,
};

}  // namespace ripple

#endif
