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

#ifndef RIPPLE_APP_MISC_DETAIL_ONLINE_DELETE_RANGES_H_INCLUDED
#define RIPPLE_APP_MISC_DETAIL_ONLINE_DELETE_RANGES_H_INCLUDED

#include <xrpl/basics/RangeSet.h>

#include <cstdint>

namespace ripple {
namespace detail {

/** Compute the actual SQL-deletion target windows for an online-delete
    pass, given a base [minSeq, lastRotated - 1] window and the pinned
    ranges that must be preserved.

    The base window is the half-open span the caller would normally
    delete (everything older than `lastRotated`, but no older than the
    minimum sequence currently stored in the table being cleared).
    Pinned ranges (catalogue history that must survive rotation) are
    subtracted from the base — the result may be empty (everything is
    pinned), one interval (no overlap, or pinned only trims an edge),
    or multiple disjoint intervals (pins cut a hole in the base).

    Pure function so the math is unit-testable without standing up
    SHAMapStoreImp. The actual side-effecting deletions (driven by
    a per-table lambda in clearSqlRanges) operate on whatever interval
    set this helper returns.

    @param minSeq        The smallest sequence currently stored in the
                         table being cleared. If >= lastRotated, the
                         base window is empty and we return {}.
    @param lastRotated   The exclusive upper bound — sequences from
                         this seq forward are kept.
    @param pinned        Pinned-ledger ranges to preserve.

    @return The set of intervals to actually delete. Empty if either
            the base window is empty or pinned ranges fully cover it.
*/
inline RangeSet<std::uint32_t>
computeOnlineDeleteTargets(
    std::uint32_t minSeq,
    std::uint32_t lastRotated,
    RangeSet<std::uint32_t> const& pinned)
{
    // Empty base window: nothing newer than minSeq qualifies for
    // deletion.
    if (minSeq >= lastRotated)
        return {};

    RangeSet<std::uint32_t> target;
    target.insert(range(minSeq, lastRotated - 1));
    target -= pinned;
    return target;
}

}  // namespace detail
}  // namespace ripple

#endif
