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

#include <xrpl/protocol/AppLoader.h>
#include <xrpl/protocol/Protocol.h>

#include <string_view>

namespace ripple {
namespace appLoader {

namespace {

// HTML "ASCII whitespace" per the WHATWG spec: TAB, LF, FF, CR, SPACE.
inline bool
isSpace(std::uint8_t c) noexcept
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

// Control characters we refuse to store. Everything in C0 that is not HTML
// whitespace, plus DEL. These have no business in a loader document and
// their presence is a strong smell of a non-HTML payload being smuggled
// through as "text".
inline bool
isForbiddenControl(std::uint8_t c) noexcept
{
    if (isSpace(c))
        return false;
    return c < 0x20 || c == 0x7F;
}

inline std::uint8_t
asciiLower(std::uint8_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c + 32) : c;
}

// True if `lit` (which must be lowercase ASCII) occurs at `pos`, compared
// case-insensitively. A match that would run past the end is no match.
bool
matchAt(
    std::uint8_t const* data,
    std::size_t size,
    std::size_t pos,
    std::string_view lit) noexcept
{
    if (pos > size || size - pos < lit.size())
        return false;
    for (std::size_t i = 0; i < lit.size(); ++i)
        if (asciiLower(data[pos + i]) != static_cast<std::uint8_t>(lit[i]))
            return false;
    return true;
}

// Index of the first case-insensitive occurrence of `lit` at or after
// `from`, or `size` if absent.
std::size_t
findFrom(
    std::uint8_t const* data,
    std::size_t size,
    std::size_t from,
    std::string_view lit) noexcept
{
    if (lit.empty() || lit.size() > size)
        return size;
    for (std::size_t i = from; i + lit.size() <= size; ++i)
        if (matchAt(data, size, i, lit))
            return i;
    return size;
}

// A tag name must be terminated by whitespace, '>' or '/', otherwise
// "<htmlish>" would be mistaken for "<html>".
inline bool
isTagNameEnd(std::uint8_t c) noexcept
{
    return isSpace(c) || c == '>' || c == '/';
}

}  // namespace

bool
isValidUTF8(std::uint8_t const* data, std::size_t size)
{
    // Table-free decoder in the style of Markus Kuhn's utf8_check.c. Each
    // branch establishes that the whole sequence is present before reading
    // any continuation byte, so a truncated tail is simply invalid.
    std::size_t i = 0;
    while (i < size)
    {
        std::uint8_t const c0 = data[i];

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
            if ((data[i + 1] & 0xC0) != 0x80)
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
            std::uint8_t const c1 = data[i + 1];
            if ((c1 & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80)
                return false;
            if (c0 == 0xE0 && (c1 & 0xE0) == 0x80)  // overlong
                return false;
            if (c0 == 0xED && (c1 & 0xE0) == 0xA0)  // UTF-16 surrogate
                return false;
            i += 3;
            continue;
        }

        if ((c0 & 0xF8) == 0xF0)
        {
            // 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
            if (size - i < 4)
                return false;
            std::uint8_t const c1 = data[i + 1];
            if ((c1 & 0xC0) != 0x80 || (data[i + 2] & 0xC0) != 0x80 ||
                (data[i + 3] & 0xC0) != 0x80)
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

Result
validate(std::uint8_t const* data, std::size_t size)
{
    if (size == 0 || data == nullptr)
        return Result::empty;

    if (size > maxAppLoaderLength)
        return Result::tooLarge;

    if (!isValidUTF8(data, size))
        return Result::badUTF8;

    for (std::size_t i = 0; i < size; ++i)
        if (isForbiddenControl(data[i]))
            return Result::badControlChar;

    // Skip an optional UTF-8 BOM, then any leading whitespace.
    std::size_t begin = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        begin = 3;
    while (begin < size && isSpace(data[begin]))
        ++begin;

    if (begin == size)
        return Result::empty;

    // The document must open with a doctype or with the <html> tag itself.
    // We accept "<!doctype" followed by whitespace, because that is the
    // only doctype form any browser treats as standards mode for HTML.
    bool const opensWithDoctype = matchAt(data, size, begin, "<!doctype") &&
        begin + 9 < size && isSpace(data[begin + 9]);

    bool const opensWithHtml = matchAt(data, size, begin, "<html") &&
        begin + 5 < size && isTagNameEnd(data[begin + 5]);

    if (!opensWithDoctype && !opensWithHtml)
        return Result::noDoctype;

    // Locate the <html> start tag.
    std::size_t open = begin;
    for (;;)
    {
        open = findFrom(data, size, open, "<html");
        if (open == size)
            return Result::noHtmlElement;
        if (open + 5 < size && isTagNameEnd(data[open + 5]))
            break;
        ++open;
    }

    // Locate the last </html> end tag. Scanning backwards means a literal
    // "</html>" inside a script string does not shadow the real one.
    std::size_t close = size;
    std::size_t closeEnd = size;
    {
        std::size_t probe = open;
        for (;;)
        {
            std::size_t const found = findFrom(data, size, probe, "</html");
            if (found == size)
                break;

            // Allow whitespace between the tag name and '>'.
            std::size_t j = found + 6;
            while (j < size && isSpace(data[j]))
                ++j;
            if (j < size && data[j] == '>')
            {
                close = found;
                closeEnd = j + 1;
            }
            probe = found + 1;
        }
    }

    if (close == size)
        return Result::unclosed;

    if (close < open)
        return Result::unclosed;

    // Nothing but whitespace may follow the closing tag.
    for (std::size_t i = closeEnd; i < size; ++i)
        if (!isSpace(data[i]))
            return Result::trailingGarbage;

    return Result::ok;
}

char const*
to_string(Result r)
{
    switch (r)
    {
        case Result::ok:
            return "ok";
        case Result::empty:
            return "empty document";
        case Result::tooLarge:
            return "document exceeds maxAppLoaderLength";
        case Result::badUTF8:
            return "malformed UTF-8";
        case Result::badControlChar:
            return "forbidden control character";
        case Result::noDoctype:
            return "does not open with a doctype or <html> tag";
        case Result::noHtmlElement:
            return "missing <html> start tag";
        case Result::unclosed:
            return "missing or misplaced </html> end tag";
        case Result::trailingGarbage:
            return "content after </html>";
    }
    return "unknown";
}

}  // namespace appLoader
}  // namespace ripple
