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

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/UTF8.h>
#include <xrpl/protocol/AppLoader.h>
#include <xrpl/protocol/Protocol.h>

#include <string_view>

namespace ripple {
namespace appLoader {

namespace {

// HTML "ASCII whitespace": TAB, LF, FF, CR, SPACE.
inline bool
isSpace(std::uint8_t c) noexcept
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

inline std::uint8_t
lower(std::uint8_t c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c + 32) : c;
}

// Case-insensitive match of lower-case `lit` at `pos`; never reads past
// the end.
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
        if (lower(data[pos + i]) != static_cast<std::uint8_t>(lit[i]))
            return false;
    return true;
}

std::size_t
findFrom(
    std::uint8_t const* data,
    std::size_t size,
    std::size_t from,
    std::string_view lit) noexcept
{
    for (std::size_t i = from; i + lit.size() <= size; ++i)
        if (matchAt(data, size, i, lit))
            return i;
    return size;
}

// So that "<htmlish>" is not mistaken for "<html>".
inline bool
isTagNameEnd(std::uint8_t c) noexcept
{
    return isSpace(c) || c == '>' || c == '/';
}

}  // namespace

Result
validate(std::uint8_t const* data, std::size_t size)
{
    if (size == 0 || data == nullptr)
        return Result::empty;
    if (size > maxAppLoaderLength)
        return Result::tooLarge;
    if (!isValidUTF8(Slice(data, size)))
        return Result::badUTF8;
    for (std::size_t i = 0; i < size; ++i)
        if (!isSpace(data[i]) && (data[i] < 0x20 || data[i] == 0x7F))
            return Result::badControlChar;

    std::size_t begin = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        begin = 3;
    while (begin < size && isSpace(data[begin]))
        ++begin;
    if (begin == size)
        return Result::empty;

    // "<!doctype" followed by whitespace is the only doctype form any
    // browser treats as standards-mode HTML.
    bool const doctype = matchAt(data, size, begin, "<!doctype") &&
        begin + 9 < size && isSpace(data[begin + 9]);
    bool const html = matchAt(data, size, begin, "<html") && begin + 5 < size &&
        isTagNameEnd(data[begin + 5]);
    if (!doctype && !html)
        return Result::noDoctype;

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

    // The LAST well-formed </html>, so that one quoted inside a script
    // does not shadow the real one. Linear: each probe resumes one past
    // the previous match.
    std::size_t closeEnd = size;
    for (std::size_t probe = open;;)
    {
        std::size_t const found = findFrom(data, size, probe, "</html");
        if (found == size)
            break;
        std::size_t j = found + 6;
        while (j < size && isSpace(data[j]))
            ++j;
        if (j < size && data[j] == '>')
            closeEnd = j + 1;
        probe = found + 1;
    }
    if (closeEnd == size)
        return Result::unclosed;

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
