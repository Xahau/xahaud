//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_PROTOCOL_SIGNINGPOLICY_H_INCLUDED
#define RIPPLE_PROTOCOL_SIGNINGPOLICY_H_INCLUDED

#include <cstdint>

namespace ripple {

/** Per-account opt-in policy bits stored in the optional sfSigningPolicy
    field on AccountRoot. Governs participation in nested multi-sign.

    Two-sided consent model:
    - acceptsBelow*: what proofs an account accepts when being signed for
                     (the account whose SignerList is being evaluated).
    - permitsSelf* : what an account permits when delegated to as a signer
                     (the account named in another account's SignerList).

    Effective policy at each tree edge (parent A -> signer B) is the
    intersection of inherited path cap, A.acceptsBelow*, and B.permitsSelf*.
    Authority can only narrow as the proof descends.

    The meta toggle pfApplyPoliciesAllMultiSign lives only on the txn
    account; when set, the policy intersection applies even at depth 1
    (flat multi-sign). When clear (default), depth 1 follows legacy
    flat-multisign behavior unconditionally.
 */
enum SigningPolicyFlags : std::uint32_t {
    pfApplyPoliciesAllMultiSign = 0x00000001,

    pfAcceptsBelowNested = 0x00000010,
    pfAcceptsBelowCycleAdjustedQuorum = 0x00000020,
    pfAcceptsBelowDisabledMaster = 0x00000040,

    pfPermitsSelfNested = 0x00000100,
    pfPermitsSelfCycleAdjustedQuorum = 0x00000200,
    pfPermitsSelfDisabledMaster = 0x00000400,
};

namespace sigpol {

constexpr std::uint32_t acceptsBelowMask = pfAcceptsBelowNested |
    pfAcceptsBelowCycleAdjustedQuorum | pfAcceptsBelowDisabledMaster;

constexpr std::uint32_t permitsSelfMask = pfPermitsSelfNested |
    pfPermitsSelfCycleAdjustedQuorum | pfPermitsSelfDisabledMaster;

/** Capability axes, expressed in the permitsSelf bit positions so a single
    uint32 can carry the path's effective capabilities. The acceptsBelow
    bits (0x10..0x40) at parents are translated to these axes
    (0x100..0x400) by left-shifting 4 bits.
 */
enum CapabilityAxis : std::uint32_t {
    capNested = pfPermitsSelfNested,
    capCycleAdjustedQuorum = pfPermitsSelfCycleAdjustedQuorum,
    capDisabledMaster = pfPermitsSelfDisabledMaster,
};

constexpr std::uint32_t capAllMask =
    capNested | capCycleAdjustedQuorum | capDisabledMaster;

/** Translate a parent account's acceptsBelow bits into the common
    capability axes (permitsSelf bit positions) so they can be intersected
    with a child's permitsSelf bits.
 */
constexpr std::uint32_t
acceptsBelowToCapabilities(std::uint32_t policy)
{
    return (policy & acceptsBelowMask) << 4;
}

/** Extract the permitsSelf bits as capability axes (identity). */
constexpr std::uint32_t
permitsSelfToCapabilities(std::uint32_t policy)
{
    return policy & permitsSelfMask;
}

constexpr bool
hasCapability(std::uint32_t cap, CapabilityAxis axis)
{
    return (cap & axis) != 0;
}

}  // namespace sigpol

}  // namespace ripple

#endif
