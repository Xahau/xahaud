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

#include <xrpl/beast/net/IPEndpoint.h>
#include <boost/algorithm/string/trim.hpp>
#include <charconv>
#include <system_error>

namespace beast {
namespace IP {

namespace {
Port
make_port(std::string_view s)
{
    Port port = 0;

    if (!s.empty())
    {
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), port);

        if (ec != std::errc{} || ptr != s.data() + s.size())
            throw std::system_error(std::make_error_code(ec));
    }

    return port;
}

}  // namespace

std::optional<Endpoint>
Endpoint::from_string_checked(std::string_view s)
{
    using namespace boost::asio::ip;

    // We need to catch exceptions here because we use the throwing versions of
    // the boost address parsing functions. It is also possible that exceptions
    // come from std::string_view, even though we are careful.
    try
    {
        auto is_space = [](std::string_view::value_type c) {
            return std::isspace(std::string_view::traits_type::to_int_type(c));
        };

        s.remove_prefix(
            std::distance(
                s.begin(), std::find_if_not(s.begin(), s.end(), is_space)));
        s.remove_suffix(
            std::distance(
                s.rbegin(), std::find_if_not(s.rbegin(), s.rend(), is_space)));

        if (s.empty())
            return std::nullopt;

        if (s[0] == '[')
        {  // Bracketed notation: must be an IPv6 address
            auto close = s.find(']');

            if (close == std::string_view::npos)
                return std::nullopt;

            auto addr = s.substr(1, close - 1);
            auto rest = s.substr(close + 1);

            if (rest.empty())
                return Endpoint{make_address_v6(addr)};

            if (rest[0] != ':')
                return std::nullopt;

            return Endpoint{make_address_v6(addr), make_port(rest.substr(1))};
        }

        // We now need to check if a space is present. We already trimmed
        // whitespace from the end of the input string, so if we find any
        // it means we have a port present.
        auto sp = std::find_if(s.begin(), s.end(), is_space);

        if (sp == s.end())
        {
            auto colon = s.find(':');

            // A single colon suggests this is an IPv4 address with a port.
            if (colon != std::string_view::npos && colon == s.rfind(':'))
                return Endpoint{
                    make_address_v4(s.substr(0, colon)),
                    make_port(s.substr(colon + 1))};

            // It's a standalone address (either v4 or v6)
            return Endpoint{make_address(s)};
        }

        // Either a v4 or a v6 address followed by one or more spaces and a port
        auto rest = s.substr(std::distance(s.begin(), sp));

        return Endpoint{
            make_address(s.substr(0, std::distance(s.begin(), sp))),
            make_port(rest.substr(
                std::distance(
                    rest.begin(),
                    std::find_if_not(rest.begin(), rest.end(), is_space))))};
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::string
Endpoint::to_string() const
{
    if (port() == 0)
        return address().to_string();

    if (address().is_v6())
        return "[" + address().to_string() + "]:" + std::to_string(port());

    return address().to_string() + ":" + std::to_string(port());
}

}  // namespace IP
}  // namespace beast
