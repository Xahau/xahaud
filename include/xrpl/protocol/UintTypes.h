//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#ifndef RIPPLE_PROTOCOL_UINTTYPES_H_INCLUDED
#define RIPPLE_PROTOCOL_UINTTYPES_H_INCLUDED

#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/SystemParameters.h>

namespace ripple {

using LedgerHash = uint256;

/** Directory is an index into the directory of offer books.
    The last 64 bits of this are the quality. */
using Directory = base_uint<256, struct DirectoryTag>;

/** Currency is a hash representing a specific currency. */
using Currency = base_uint<160, struct CurrencyTag>;

/** NodeID is a 160-bit hash representing one node. */
using NodeID = base_uint<160, struct NodeIDTag>;

/** A Multi-Purpose Token Issuance ID,

    The ID is the a concatenation of a 32-bit sequence number,
    in big endian, and a 160-bit account.

    @note This type is, unfortunately, untagged because the authors
          of the original code used a deserialization APIs that did
          not support tags. It should be fixed.
 */
using MPTID = base_uint<192>;

namespace detail {

// A table mapping which characters we are willing to allow in the ASCII
// representation of a three-letter currency code.
inline constexpr auto validIsoChars = []() consteval {
    std::string_view isoCharSet =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "<>(){}[]|?!@#$%^&*";

    std::array<bool, 256> table{};

    for (unsigned char c : isoCharSet)
        table[c] = true;

    return table;
}();

// Determines if the given code is composed entirely of letters valid in
// ISO 4217 codes.
[[nodiscard]] inline constexpr bool
isIsoCode(std::string_view code) noexcept
{
    return std::all_of(code.begin(), code.end(), [](char c) {
        return validIsoChars[static_cast<unsigned char>(c)];
    });
}

// The location (in bytes) of the 3 digit currency inside a 160-bit value
inline constexpr std::size_t isoCodeOffset = 12;

// The length of an ISO-4217 like code
inline constexpr std::size_t isoCodeLength = 3;

inline constexpr Currency isoMaskBits = ~Currency(0xFFFFFF0000000000);

// The special currency identifier for the system currency: all-zero
inline constexpr Currency xrpCurrency{0x0000000000000000};

// The special currency identifier meaning "no currency"
inline constexpr Currency noCurrency{0x0000000000000001};

// The system currency identifier in "ISO4217 format" which is reserved.
// We derive this value dynamically from the system currency code.
inline constexpr Currency badCurrency = []() consteval {
    if (systemCurrencyCode.size() != isoCodeLength)
        throw "Incorrect systemCurrencyCode size (must be 3 digits)";

    std::array<std::uint8_t, Currency::bytes> bytes{};

    for (std::size_t i = 0; i < isoCodeLength; ++i)
        bytes[isoCodeOffset + i] =
            static_cast<std::uint8_t>(systemCurrencyCode[i]);

    return Currency(bytes);
}();

// We take advantage of the fact that an ASCII value in the [a-z] range
// transforms into the equivalent character in the [A-Z] range when you
// AND it with 0xDF, so we can compare against all possible variants of
// "XAH" in one go:
inline constexpr Currency badCurrencyCodeMask{0xDFDFDF0000000000};

}  // namespace detail

/** XRP currency. */
[[nodiscard]] constexpr Currency const&
xrpCurrency() noexcept
{
    return detail::xrpCurrency;
}

[[nodiscard]] constexpr bool
isXRP(Currency const& c) noexcept
{
    return c == detail::xrpCurrency;
}

/** A placeholder for empty currencies. */
[[nodiscard]] constexpr Currency const&
noCurrency() noexcept
{
    return detail::noCurrency;
}

/** We deliberately disallow the currency that looks like "XAH" because too
    many people were using it instead of the correct XAH currency. */
[[nodiscard]] constexpr Currency const&
badCurrency() noexcept
{
    return detail::badCurrency;
}

[[nodiscard]] constexpr bool
isBadCurrency(Currency const& c) noexcept
{
    // We take advantage of the fact that an ASCII value in the [a-z] range
    // transforms into the equivalent character in the [A-Z] range when you
    // AND it with 0xDF, so we can compare against all possible variants of
    // the bad currency code in one go:
    return (c & detail::badCurrencyCodeMask) == badCurrency();
}

/** Returns "", "XAH", three letter ISO code or the hex representation. */
[[nodiscard]] inline std::string
to_string(Currency const& currency)
{
    if (currency == xrpCurrency())
        return std::string{systemCurrencyCode};

    if (currency == noCurrency())
        return "1";

    if ((currency & detail::isoMaskBits) == beast::zero)
    {
        std::string_view const iso(
            reinterpret_cast<char const*>(currency.data()) +
                detail::isoCodeOffset,
            detail::isoCodeLength);

        // Specifying the system currency code using ISO-style representation
        // is not allowed. Note that the check is case-sensitive; yet another
        // instance of legacy code smell.
        if (detail::isIsoCode(iso) && iso != systemCurrencyCode)
            return std::string{iso};
    }

    return strHex(currency);
}

/** Tries to convert a string to a Currency, returns true on success.

    @note This function will return success if the resulting currency is
          badCurrency(). This legacy behavior is unfortunate; changing this
          will require very careful checking everywhere and may mean having
          to rewrite some unit test code.
*/
[[nodiscard]] constexpr bool
to_currency(Currency& currency, std::string_view code) noexcept
{
    if (code.empty() || code == systemCurrencyCode)
    {
        currency = xrpCurrency();
        return true;
    }

    // Handle ISO-4217-like 3-digit character codes.
    if (code.size() != detail::isoCodeLength)
        return currency.parseHex(code);

    if (!detail::isIsoCode(code))
        return false;

    currency = beast::zero;

    std::copy_n(
        code.data(), code.size(), currency.begin() + detail::isoCodeOffset);

    return true;
}

/** Tries to convert a string to a Currency, returns noCurrency() on failure.

    @note This function can return badCurrency(). This legacy behavior is
          unfortunate; changing this will require very careful checking
          everywhere and may mean having to rewrite some unit test code.
*/
[[nodiscard]] constexpr Currency
to_currency(std::string_view code) noexcept
{
    if (Currency currency; to_currency(currency, code))
        return currency;

    return noCurrency();
}

inline std::ostream&
operator<<(std::ostream& os, Currency const& x)
{
    os << to_string(x);
    return os;
}

}  // namespace ripple

namespace std {

template <>
struct hash<ripple::Currency> : ripple::Currency::hasher
{
    explicit hash() = default;
};

template <>
struct hash<ripple::NodeID> : ripple::NodeID::hasher
{
    explicit hash() = default;
};

template <>
struct hash<ripple::Directory> : ripple::Directory::hasher
{
    explicit hash() = default;
};

template <>
struct hash<ripple::uint256> : ripple::uint256::hasher
{
    explicit hash() = default;
};

}  // namespace std

#endif
