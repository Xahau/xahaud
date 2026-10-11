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

/** Validation of an AppLoader document (sfAppLoader), the bootstrap page of
    a Progressive Web App served from an account.

    Consensus cannot afford a real HTML parser, so this is a shallow sanity
    gate and nothing more. A document must be non-empty and at most
    maxAppLoaderLength bytes; well-formed UTF-8; free of C0 control
    characters other than TAB, LF, FF and CR, and of DEL; begin, after an
    optional BOM and whitespace, with an HTML doctype or an <html start tag;
    and contain an <html start tag followed later by a </html> end tag with
    nothing but whitespace after the last one. Nothing parses comments,
    scripts or attributes, and content is not policed.

    Consensus critical: a change to what it accepts needs an amendment.
*/
namespace appLoader {

enum class Result : std::uint8_t {
    ok = 0,
    empty,
    tooLarge,
    badUTF8,
    badControlChar,
    noDoctype,        // does not open with a doctype or <html tag
    noHtmlElement,    // no <html start tag
    unclosed,         // no </html> end tag after it
    trailingGarbage,  // non-whitespace after the last </html>
};

/** Total: every input yields a result. `data` may be null iff size is 0. */
Result
validate(std::uint8_t const* data, std::size_t size);

char const*
to_string(Result r);

}  // namespace appLoader
}  // namespace ripple

#endif
