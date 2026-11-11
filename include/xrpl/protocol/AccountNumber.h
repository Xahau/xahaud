//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
    Copyright (c) 2026 XRPL Labs

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

#ifndef RIPPLE_PROTOCOL_ACCOUNTNUMBER_H_INCLUDED
#define RIPPLE_PROTOCOL_ACCOUNTNUMBER_H_INCLUDED

#include <xrpl/protocol/AccountID.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ripple {

/** Account numbers: a human-facing, typo-resistant form of AccountIndex.

    An *account index* is the dense uint64 counter stored in sfAccountIndex
    on each AccountRoot. Every value below sfAccountCount is a live account,
    so a mistyped index is always a different, valid account.

    An *account number* is the account index in decimal with three check
    digits appended, derived from the account's AccountID:

        check  = (last two bytes of AccountID, big-endian) mod 1000
        number = index * 1000 + check

    e.g. index 18723, AccountID ending in 0xBEEF (48879) -> 18723879.

    Resolving a number splits it with /1000 and %1000, looks up the index,
    and compares the check against the account found. A mistyped index
    lands on an account whose check digits are independent, so any error
    is detected with probability ~0.999; an error confined to the check
    digits is always detected.

    The two forms are NOT interchangeable and share no number space:
    an index is rendered in JSON as a hex string (like every uint64 ledger
    field); a number is a digits-only decimal string. Interfaces may group
    the digits in threes for display (the last group is then always the
    check digits); the JSON form is never grouped.
*/
namespace accountNumber {

/// Number of decimal check digits appended to the index.
constexpr unsigned checkDigits = 3;
constexpr std::uint64_t checkModulus = 1000;

/// Check digits for an account, in [0, checkModulus).
std::uint64_t
check(AccountID const& id);

/// index * 1000 + check(id); nullopt if the result would overflow.
std::optional<std::uint64_t>
make(std::uint64_t index, AccountID const& id);

struct Split
{
    std::uint64_t index;
    std::uint64_t check;
};

/// Split a number into its index and check digits. Never fails.
Split
split(std::uint64_t number);

/// Parse the canonical JSON form: non-empty, digits only, no leading
/// zero (except "0"), fits in uint64. Embedded spaces are NOT accepted;
/// interfaces strip display grouping before calling this.
std::optional<std::uint64_t>
parse(std::string_view s);

/// Canonical JSON form: decimal digits, no grouping.
std::string
toString(std::uint64_t number);

}  // namespace accountNumber

}  // namespace ripple

#endif
