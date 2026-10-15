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

#ifndef BEAST_NET_IPENDPOINT_H_INCLUDED
#define BEAST_NET_IPENDPOINT_H_INCLUDED

#include <xrpl/beast/hash/hash_append.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/beast/net/IPAddress.h>

#include <compare>
#include <optional>
#include <string>
#include <string_view>

namespace beast {
namespace IP {

using Port = std::uint16_t;

/** A version-independent IP address and port combination. */
class Endpoint
{
public:
    /** Create an unspecified endpoint. */
    Endpoint() noexcept = default;

    /** Create an endpoint from the address and optional port. */
    explicit Endpoint(
        boost::asio::ip::address const& addr,
        Port port = 0) noexcept
        : m_addr(addr), m_port(port)
    {
    }

    /** Create an Endpoint from a string.

        Supported formats:
        - IPv4:                 `1.2.3.4`
        - IPv4 with port:       `1.2.3.4:80` or `1.2.3.4 80`
        - IPv6:                 `::1` or `2001:db8::1`
        - IPv6 with port:       `::1 80` or `2001:db8::1 80`
        - Bracketed IPv6:       `[::1]`
        - Bracketed IPv6 port:  `[::1]:80`

        Leading and trailing whitespace is ignored. If the port is
        omitted, the endpoint will have a zero port.

        @param s The string to parse
        @return The parsed endpoint, or `std::nullopt` on failure
     */
    static std::optional<Endpoint>
    from_string_checked(std::string_view s);

    static Endpoint
    from_string(std::string_view s)
    {
        return from_string_checked(s).value_or(Endpoint{});
    }

    /** Returns a string representing the endpoint. */
    std::string
    to_string() const;

    /** Returns the port number on the endpoint. */
    Port
    port() const noexcept
    {
        return m_port;
    }

    /** Returns a new Endpoint with a different port. */
    Endpoint
    at_port(Port port) const
    {
        return Endpoint(m_addr, port);
    }

    /** Returns the address portion of this endpoint. */
    boost::asio::ip::address const&
    address() const noexcept
    {
        return m_addr;
    }

    /** Convenience accessors for the address part. */
    /** @{ */
    bool
    is_v4() const
    {
        return m_addr.is_v4();
    }
    bool
    is_v6() const
    {
        return m_addr.is_v6();
    }
    boost::asio::ip::address_v4 const
    to_v4() const
    {
        return m_addr.to_v4();
    }
    boost::asio::ip::address_v6 const
    to_v6() const
    {
        return m_addr.to_v6();
    }
    /** @} */

    template <class Hasher>
    friend void
    hash_append(Hasher& h, Endpoint const& endpoint)
    {
        using ::beast::hash_append;
        hash_append(h, endpoint.m_addr, endpoint.m_port);
    }

private:
    boost::asio::ip::address m_addr;
    Port m_port = 0;
};

/** Comparison operators. */
[[nodiscard]] inline bool
operator==(Endpoint const& lhs, Endpoint const& rhs) noexcept
{
    return lhs.address() == rhs.address() && lhs.port() == rhs.port();
}

[[nodiscard]] inline std::strong_ordering
operator<=>(Endpoint const& lhs, Endpoint const& rhs) noexcept
{
    if (lhs.address() < rhs.address())
        return std::strong_ordering::less;

    if (rhs.address() < lhs.address())
        return std::strong_ordering::greater;

    return lhs.port() <=> rhs.port();
}

//------------------------------------------------------------------------------

// Properties

/** Returns `true` if the endpoint is a loopback address. */
inline bool
is_loopback(Endpoint const& endpoint)
{
    return endpoint.address().is_loopback();
}

/** Returns `true` if the endpoint is unspecified. */
inline bool
is_unspecified(Endpoint const& endpoint)
{
    return endpoint.address().is_unspecified();
}

/** Returns `true` if the endpoint is a multicast address. */
inline bool
is_multicast(Endpoint const& endpoint)
{
    return endpoint.address().is_multicast();
}

/** Returns `true` if the endpoint is a private unroutable address. */
inline bool
is_private(Endpoint const& endpoint)
{
    return is_private(endpoint.address());
}

/** Returns `true` if the endpoint is a public routable address. */
inline bool
is_public(Endpoint const& endpoint)
{
    return is_public(endpoint.address());
}

//------------------------------------------------------------------------------

/** Returns the endpoint represented as a string. */
inline std::string
to_string(Endpoint const& endpoint)
{
    return endpoint.to_string();
}

/** Output stream conversion. */
template <typename OutputStream>
OutputStream&
operator<<(OutputStream& os, Endpoint const& endpoint)
{
    os << to_string(endpoint);
    return os;
}

}  // namespace IP
}  // namespace beast

//------------------------------------------------------------------------------

namespace std {
/** std::hash support. */
template <>
struct hash<::beast::IP::Endpoint>
{
    explicit hash() = default;

    std::size_t
    operator()(::beast::IP::Endpoint const& endpoint) const
    {
        return ::beast::uhash<>{}(endpoint);
    }
};
}  // namespace std

namespace boost {
/** boost::hash support. */
template <>
struct hash<::beast::IP::Endpoint>
{
    explicit hash() = default;

    std::size_t
    operator()(::beast::IP::Endpoint const& endpoint) const
    {
        return ::beast::uhash<>{}(endpoint);
    }
};
}  // namespace boost

#endif
