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

#ifndef RIPPLE_PROTOCOL_UTF8_H_INCLUDED
#define RIPPLE_PROTOCOL_UTF8_H_INCLUDED

#include <xrpl/basics/Slice.h>

namespace ripple {

/** Check that a byte sequence is well-formed UTF-8, excluding the two
    noncharacters U+FFFE and U+FFFF.

    Rejects:
      - overlong encodings;
      - UTF-16 surrogate halves (U+D800..U+DFFF);
      - scalar values above U+10FFFF, and 0xF5..0xFF lead bytes;
      - stray continuation bytes;
      - multi-byte sequences truncated by the end of the buffer;
      - U+FFFE and U+FFFF (EF BF BE, EF BF BF).

    U+FFFE and U+FFFF are well-formed, but have no use in a URI or a name
    and exist to be misread: U+FFFE is a byte-swapped BOM, so a client that
    transcodes to UTF-16/32 and sniffs the byte order can be pushed into
    decoding the rest of the string with the wrong endianness, and both are
    commonly used as in-band sentinels. They have always been rejected by
    URIToken and HookName validation, and this function preserves that.

    This is consensus-critical. For every input the previous decoder could
    evaluate without reading past the end of the buffer, the result is
    identical to it. The only difference is that truncated trailing
    sequences, where the previous decoder read out of bounds, are now always
    rejected. Any further change to what this accepts needs an amendment.

    @param s The bytes to check.

    @note Total: every byte read is within s.
*/
bool
isValidUTF8(Slice const& s) noexcept;

}  // namespace ripple

#endif
