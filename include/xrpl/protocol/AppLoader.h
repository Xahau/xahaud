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

#ifndef RIPPLE_PROTOCOL_APPLOADER_H_INCLUDED
#define RIPPLE_PROTOCOL_APPLOADER_H_INCLUDED

#include <cstddef>
#include <cstdint>

namespace ripple {

/** Validation of the AccountRoot `AppLoader` blob (sfAppLoader).

    The blob carries the bootstrap document of a Progressive Web App that is
    served from the account itself. Consensus cannot afford to run a real
    HTML parser, so enforcement here is deliberately *shallow*: it rejects
    payloads that plainly are not a UTF-8 HTML document, and nothing more.
    It is a sanity gate, not a conformance checker -- a blob that passes is
    not thereby guaranteed to render, and clients must still treat the
    contents as wholly untrusted input.

    A conforming blob must:
      - be non-empty and no longer than `maxAppLoaderLength` bytes;
      - be well-formed UTF-8 (no overlongs, surrogates, or scalar values
        above U+10FFFF);
      - contain no C0 control characters other than TAB, LF and CR, and no
        NUL or DEL;
      - begin (after an optional BOM and leading whitespace) with either an
        HTML doctype or an `<html` start tag;
      - contain an `<html` start tag and a matching `</html>` end tag, in
        that order, with the end tag being the last non-whitespace content.
*/
namespace appLoader {

/** Reasons an AppLoader blob may be rejected.

    Returned by `validate` so that callers can log something more useful
    than a bare failure. Not part of the protocol: only `ok` vs. everything
    else is consensus-relevant.
*/
enum class Result : std::uint8_t {
    ok = 0,
    empty,           // zero-length blob
    tooLarge,        // exceeds maxAppLoaderLength
    badUTF8,         // malformed UTF-8 sequence
    badControlChar,  // disallowed control character
    noDoctype,       // does not open with a doctype or <html tag
    noHtmlElement,   // missing <html ...> start tag
    unclosed,        // missing </html> end tag, or it precedes <html
    trailingGarbage  // non-whitespace content after </html>
};

/** Validate a UTF-8 byte sequence as an AppLoader document.

    @param data Pointer to the first byte; may be nullptr iff size is 0.
    @param size Length of the blob in bytes.
    @return Result::ok if the blob is acceptable, otherwise the first
            problem encountered.

    @note Total: every input yields a defined result, including inputs
          whose final bytes form a truncated multi-byte sequence.
*/
Result
validate(std::uint8_t const* data, std::size_t size);

/** Human-readable description of a validation result, for logging. */
char const*
to_string(Result r);

/** Check that a byte sequence is well-formed UTF-8.

    Rejects overlong encodings, UTF-16 surrogate halves (U+D800..U+DFFF),
    and scalar values above U+10FFFF. Noncharacters such as U+FFFE and
    U+FFFF are accepted: they are well-formed UTF-8 and are permitted in
    interchange.
*/
bool
isValidUTF8(std::uint8_t const* data, std::size_t size);

}  // namespace appLoader

}  // namespace ripple

#endif
