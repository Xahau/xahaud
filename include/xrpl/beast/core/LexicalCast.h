//------------------------------------------------------------------------------
/*
    This file is part of Beast: https://github.com/vinniefalco/Beast
    Copyright 2013, Vinnie Falco <vinnie.falco@gmail.com>

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

#ifndef BEAST_MODULE_CORE_TEXT_LEXICALCAST_H_INCLUDED
#define BEAST_MODULE_CORE_TEXT_LEXICALCAST_H_INCLUDED

#include <xrpl/beast/type_name.h>

#include <boost/beast/core/string_type.hpp>
#include <boost/core/detail/string_view.hpp>
#include <boost/utility/string_view.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>

namespace beast {

/** Thrown when a conversion is not possible with LexicalCast.
    Only used in the throw variants of lexicalCast.
*/
struct BadLexicalCast : std::bad_cast
{
private:
    std::string msg;

public:
    explicit BadLexicalCast(std::string m = {}) : msg(std::bad_cast::what())
    {
        if (!m.empty())
            msg += ": " + m;
    }

    [[nodiscard]] char const*
    what() const noexcept override
    {
        return msg.c_str();
    }
};

//------------------------------------------------------------------------------

/** Convert from std::string_view to integral type.
    @return `false` if there was a parsing or range error
*/
template <class Out>
    requires std::is_integral_v<Out> && (!std::is_same_v<Out, bool>)
[[nodiscard]] bool
lexicalCastChecked(Out& out, std::string_view in) noexcept
{
    if (in.empty())
        return false;

    if (in.front() == '+')
    {
        in.remove_prefix(1);

        if (in.empty() || in.front() == '-')
            return false;
    }

    auto [ptr, ec] = std::from_chars(in.data(), in.data() + in.size(), out);

    return ec == std::errc{} && ptr == in.data() + in.size();
}

/** Convert from std::string_view to bool.
    @return `false` if there was a parsing error
*/
[[nodiscard]] inline bool
lexicalCastChecked(bool& out, std::string_view in) noexcept
{
    auto iequals = [](std::string_view a, std::string_view b) {
        return std::equal(
            a.begin(), a.end(), b.begin(), b.end(), [](char ca, char cb) {
                // We avoid std::tolower because it is locale-dependent. It
                // would be really nice if C++ added support for std::ascii
                // as outlined in P3688.
                if (ca >= 'A' && ca <= 'Z')
                    ca = ca + ('a' - 'A');

                if (cb >= 'A' && ca <= 'Z')
                    cb = cb + ('a' - 'A');

                return ca == cb;
            });
    };

    if (in == "1" || iequals(in, "true"))
    {
        out = true;
        return true;
    }

    if (in == "0" || iequals(in, "false"))
    {
        out = false;
        return true;
    }

    return false;
}

/** Convert from integral type to std::string.
    @return `false` if there was a conversion error
*/
template <class In>
    requires std::is_integral_v<In>
[[nodiscard]] bool
lexicalCastChecked(std::string& out, In in) noexcept
{
    std::array<char, std::numeric_limits<In>::digits10 + 3> buf;

    auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), in);

    if (ec != std::errc{})
        return false;

    out.assign(buf.data(), ptr);
    return true;
}

/** Convert from enum type to std::string.
    @return `false` if there was a conversion error
*/
template <class In>
    requires std::is_enum_v<In>
[[nodiscard]] bool
lexicalCastChecked(std::string& out, In in) noexcept
{
    return lexicalCastChecked(out, static_cast<std::underlying_type_t<In>>(in));
}

/** Convert from Boost string_view types to integral types.

    Boost has multiple string_view variants (such as boost::beast::string_view
    and boost::core::string_view) that can be aliases to std::string_view, but
    which don't have to be.

    Since we handle std::string_view separately, this constrained overload for
    the Boost custom implementations is needed because they are not implicitly
    convertible to std::string_view.

    @return `false` if there was a parsing or range error
 */
template <class Out, class In>
    requires(
        !std::is_same_v<In, std::string_view> &&
        (std::is_same_v<In, boost::core::string_view> ||
         std::is_same_v<In, boost::beast::string_view> ||
         std::is_same_v<In, boost::string_view>))
[[nodiscard]] bool
lexicalCastChecked(Out& out, In in) noexcept
{
    return lexicalCastChecked(out, std::string_view(in.data(), in.size()));
}

/** Convert from one type to another, throw on error

    An exception of type BadLexicalCast is thrown if the conversion fails.

    @return The new type.
*/
template <class Out, class In>
Out
lexicalCastThrow(In in)
{
    if (Out out; lexicalCastChecked(out, in))
        return out;

    throw BadLexicalCast(
#ifdef DEBUG
        beast::type_name<In>() + " -> " + beast::type_name<Out>()
#endif
    );
}

/** Convert from one type to another.

    @param in The value to convert.
    @param defaultValue The value returned if parsing fails.
    @return The new type.
*/
template <class Out, class In>
[[nodiscard]] Out
lexicalCast(In in, Out defaultValue = Out())
{
    if (Out out; lexicalCastChecked(out, in))
        return out;

    return defaultValue;
}

}  // namespace beast

#endif
