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

#ifndef RIPPLE_APP_LEDGER_DETAIL_PUBLISH_GAP_H_INCLUDED
#define RIPPLE_APP_LEDGER_DETAIL_PUBLISH_GAP_H_INCLUDED

#include <xrpl/basics/RangeSet.h>

#include <cstdint>

namespace ripple {
namespace detail {

/** Decide whether pubSeq can safely advance across a gap in the
    publication set, given the pinned-ledger ranges.

    Used by LedgerMaster::findNewLedgersToPublish: when the
    publish-target set has been split into intervals (because pinned
    ranges were subtracted from it), pubSeq may be behind the start of
    the next interval. We may only "jump" pubSeq forward across that
    gap if every skipped sequence is pinned — pinned ledgers are
    persisted on disk and are intentionally never republished, so
    skipping them does not violate the contiguous-publication
    invariant. If any sequence in the gap is *not* pinned, then pubSeq
    is lagging because an earlier non-pinned ledger could not be
    fetched/published; we must halt rather than publish newer ledgers
    out of order.

    Practical note: in production this guard is essentially never
    exercised. Pinned ranges are old historical catalogue ranges and
    pubSeq tracks the recent published tip — they don't overlap. The
    guard matters only in test/standalone catalogue-load scenarios
    where a catalogue can be loaded into an environment whose ledger
    history places pinned seqs near the publish horizon. We keep the
    guard rather than rely on the assumption holding because (a) the
    cost is one RangeSet subtraction per gap and (b) silently
    publishing across a non-pinned gap would corrupt the stream
    contract.

    @param pubSeq          The next sequence the publisher would like
                           to publish.
    @param intervalStart   The first sequence in the next non-pinned
                           interval to publish.
    @param pinned          The current pinned-ledger ranges.

    @return true  pubSeq can safely jump to intervalStart
                  (pubSeq >= intervalStart, or every skipped seq is
                  pinned).
    @return false the gap [pubSeq, intervalStart) contains at least
                  one non-pinned sequence; the publisher must stop.
*/
inline bool
canSkipPinnedGap(
    std::uint32_t pubSeq,
    std::uint32_t intervalStart,
    RangeSet<std::uint32_t> const& pinned)
{
    // No gap.
    if (pubSeq >= intervalStart)
        return true;

    RangeSet<std::uint32_t> skipped;
    skipped.insert(range(pubSeq, intervalStart - 1));
    skipped -= pinned;
    return skipped.empty();
}

}  // namespace detail
}  // namespace ripple

#endif
