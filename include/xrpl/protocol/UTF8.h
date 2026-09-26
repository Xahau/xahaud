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

#include <cstddef>
#include <cstdint>

namespace ripple {

/** Check that a byte sequence is well-formed UTF-8.

    Rejects overlong encodings, UTF-16 surrogate halves (U+D800..U+DFFF),
    scalar values above U+10FFFF, stray continuation bytes, 0xF8..0xFF lead
    bytes, and truncated multi-byte sequences.

    Noncharacters such as U+FFFE and U+FFFF are accepted: they are
    well-formed UTF-8 and are permitted in interchange. Callers that must
    preserve the historical rejection of those two code points do so on top
    of this check (see URIToken::validateUTF8).

    @param data Pointer to the first byte; may be nullptr iff size is 0.
    @param size Length in bytes.

    @note Total: every byte read is within [data, data + size), so a
          sequence truncated by the end of the buffer is simply invalid.
*/
bool
isValidUTF8(std::uint8_t const* data, std::size_t size) noexcept;

}  // namespace ripple

#endif
