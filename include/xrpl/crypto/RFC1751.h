//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_CRYPTO_RFC1751_H_INCLUDED
#define RIPPLE_CRYPTO_RFC1751_H_INCLUDED

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace ripple {
namespace rfc1751 {

/** Convert a 128-bit value (big-endian) to a human-readable word string.

    @param key The 128-bit value. The span must be precisely 16 bytes long.
    @return The human readable word string if successful; nullopt on failure
 */
[[nodiscard]] std::optional<std::string>
englishFromKey(std::span<std::uint8_t const> key);

/** Convert words separated by spaces into a 128-bit value (big-endian).

    @param key The human readable word string.
    @return The 128-bit value if successful; nullopt on failure
 */
[[nodiscard]] std::optional<std::array<std::uint8_t, 16>>
keyFromEnglish(std::string_view human);

/** Pick a single dictionary word from arbitrary data.

    Not cryptographically secure. Useful for generating a short
    human-readable label from a GUID or public key.
*/
[[nodiscard]] std::string_view
wordFromBlob(std::span<std::uint8_t const> blob);

}  // namespace rfc1751
}  // namespace ripple

#endif
