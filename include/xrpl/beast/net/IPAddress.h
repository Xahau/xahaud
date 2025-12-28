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

#ifndef BEAST_NET_IPADDRESS_H_INCLUDED
#define BEAST_NET_IPADDRESS_H_INCLUDED

#include <xrpl/beast/hash/hash_append.h>
#include <xrpl/beast/hash/uhash.h>
#include <xrpl/beast/net/IPAddressV4.h>
#include <xrpl/beast/net/IPAddressV6.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <boost/asio/ip/address.hpp>
#include <boost/functional/hash.hpp>

//------------------------------------------------------------------------------

namespace beast {
namespace IP {

/** Returns `true` if the address is a private unroutable address. */
inline bool
is_private(boost::asio::ip::address const& addr)
{
    return (addr.is_v4()) ? is_private(addr.to_v4()) : is_private(addr.to_v6());
}

/** Returns `true` if the address is a public routable address. */
inline bool
is_public(boost::asio::ip::address const& addr)
{
    return (addr.is_v4()) ? is_public(addr.to_v4()) : is_public(addr.to_v6());
}

}  // namespace IP

//------------------------------------------------------------------------------

template <class Hasher>
void
hash_append(Hasher& h, boost::asio::ip::address const& addr) noexcept
{
    using beast::hash_append;
    if (addr.is_v4())
        hash_append(h, addr.to_v4().to_bytes());
    else if (addr.is_v6())
        hash_append(h, addr.to_v6().to_bytes());
    else
        UNREACHABLE("beast::hash_append : invalid address type");
}
}  // namespace beast

namespace boost {
template <>
struct hash<::boost::asio::ip::address>
{
    explicit hash() = default;

    std::size_t
    operator()(::boost::asio::ip::address const& addr) const
    {
        return ::beast::uhash<>{}(addr);
    }
};
}  // namespace boost

#endif
