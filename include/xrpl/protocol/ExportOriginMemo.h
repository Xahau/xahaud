//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_PROTOCOL_EXPORTORIGINMEMO_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORTORIGINMEMO_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace ripple::ExportOriginMemo {

inline constexpr std::string_view memoType = "xahau/export";
inline constexpr std::uint8_t version = 1;
inline constexpr std::uint8_t allowedFlags = 0;
inline constexpr std::size_t identityBytes = 42;
inline constexpr std::size_t releaseBytes = 78;

struct Origin
{
    std::uint32_t sourceDomain;
    std::uint32_t targetDomain;
    uint256 transactionHash;

    bool
    operator==(Origin const&) const = default;
};

struct Anchor
{
    LedgerIndex ledgerSequence;
    uint256 ledgerHash;

    bool
    operator==(Anchor const&) const = default;
};

struct Stamp
{
    Origin origin;
    std::optional<Anchor> anchor;

    bool
    operator==(Stamp const&) const = default;
};

enum class Error {
    reservedMemoPresent,
    reservedMemoMissing,
    reservedMemoPosition,
    malformedMemo,
    unsupportedVersion,
    unsupportedFlags,
    localChecks
};

/** Return true when any Memo reserves the Export protocol MemoType. */
bool
hasReservedMemo(STTx const& tx);

/** Append the canonical identity projection as the final Memo.

    Existing Memos remain byte-identical and in their original order. This
    function does not normalize transaction authorization fields; callers must
    supply the canonical unsigned multisign base.
*/
Expected<STTx, Error>
identityForm(STTx const& base, Origin const& origin);

/** Append the canonical release projection as the final Memo. */
Expected<STTx, Error>
releaseForm(STTx const& base, Origin const& origin, Anchor const& anchor);

/** Parse one canonical final Export Memo. */
Expected<Stamp, Error>
parse(STTx const& tx);

/** Replace a canonical release projection with its identity projection. */
Expected<STTx, Error>
projectIdentity(STTx const& stamped);

}  // namespace ripple::ExportOriginMemo

#endif
