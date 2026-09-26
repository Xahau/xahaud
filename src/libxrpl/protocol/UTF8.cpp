//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#include <xrpl/protocol/UTF8.h>

namespace ripple {

bool
isValidUTF8(Slice const& s) noexcept
{
    std::size_t const size = s.size();

    // Markus Kuhn's utf8_check.c
    // (https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c), the decoder
    // URIToken has always used, with one change: each branch establishes
    // that the whole sequence is present before reading any continuation
    // byte, so a truncated tail is invalid rather than an out-of-bounds read.
    std::size_t i = 0;
    while (i < size)
    {
        std::uint8_t const c0 = s[i];

        if (c0 < 0x80)
        {
            // 0xxxxxxx
            ++i;
            continue;
        }

        if ((c0 & 0xE0) == 0xC0)
        {
            // 110xxxxx 10xxxxxx
            if (size - i < 2)
                return false;
            if ((s[i + 1] & 0xC0) != 0x80)
                return false;
            if ((c0 & 0xFE) == 0xC0)  // overlong
                return false;
            i += 2;
            continue;
        }

        if ((c0 & 0xF0) == 0xE0)
        {
            // 1110xxxx 10xxxxxx 10xxxxxx
            if (size - i < 3)
                return false;
            std::uint8_t const c1 = s[i + 1];
            if ((c1 & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80)
                return false;
            if (c0 == 0xE0 && (c1 & 0xE0) == 0x80)  // overlong
                return false;
            if (c0 == 0xED && (c1 & 0xE0) == 0xA0)  // UTF-16 surrogate
                return false;
            if (c0 == 0xEF && c1 == 0xBF &&
                (s[i + 2] & 0xFE) == 0xBE)  // U+FFFE or U+FFFF
                return false;
            i += 3;
            continue;
        }

        if ((c0 & 0xF8) == 0xF0)
        {
            // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
            if (size - i < 4)
                return false;
            std::uint8_t const c1 = s[i + 1];
            if ((c1 & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80 ||
                (s[i + 3] & 0xC0) != 0x80)
                return false;
            if (c0 == 0xF0 && (c1 & 0xF0) == 0x80)  // overlong
                return false;
            if (c0 > 0xF4 || (c0 == 0xF4 && c1 > 0x8F))  // > U+10FFFF
                return false;
            i += 4;
            continue;
        }

        // Stray continuation byte or 0xF8..0xFF.
        return false;
    }
    return true;
}

}  // namespace ripple
