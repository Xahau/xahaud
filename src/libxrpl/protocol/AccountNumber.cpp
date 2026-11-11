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

#include <xrpl/protocol/AccountNumber.h>

#include <charconv>
#include <limits>

namespace ripple {
namespace accountNumber {

std::uint64_t
check(AccountID const& id)
{
    // Big-endian value of the last two bytes. AccountID is a hash output,
    // so these are uniformly distributed; 65536 mod 1000 leaves a 1.5%
    // bias toward 0..535 which is irrelevant for error detection.
    auto const* const bytes = id.data();
    std::uint64_t const tail =
        (static_cast<std::uint64_t>(bytes[AccountID::bytes - 2]) << 8) |
        static_cast<std::uint64_t>(bytes[AccountID::bytes - 1]);
    return tail % checkModulus;
}

std::optional<std::uint64_t>
make(std::uint64_t index, AccountID const& id)
{
    if (index > std::numeric_limits<std::uint64_t>::max() / checkModulus)
        return std::nullopt;
    auto const base = index * checkModulus;
    auto const c = check(id);
    if (base > std::numeric_limits<std::uint64_t>::max() - c)
        return std::nullopt;
    return base + c;
}

Split
split(std::uint64_t number)
{
    return {number / checkModulus, number % checkModulus};
}

std::optional<std::uint64_t>
parse(std::string_view s)
{
    if (s.empty() || s.size() > 20)
        return std::nullopt;
    if (s.size() > 1 && s.front() == '0')
        return std::nullopt;
    for (char const c : s)
        if (c < '0' || c > '9')
            return std::nullopt;

    std::uint64_t out = 0;
    auto const [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return out;
}

std::string
toString(std::uint64_t number)
{
    return std::to_string(number);
}

}  // namespace accountNumber
}  // namespace ripple
