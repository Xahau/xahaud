//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_RPC_UDPINFOSUB_H
#define RIPPLE_RPC_UDPINFOSUB_H

#include <ripple/beast/net/IPAddressConversion.h>
#include <ripple/json/json_writer.h>
#include <ripple/json/to_string.h>
#include <ripple/net/InfoSub.h>
#include <ripple/rpc/Role.h>
#include <ripple/server/WSSession.h>
#include <boost/utility/string_view.hpp>
#include <memory>
#include <string>

namespace ripple {
class UDPInfoSub : public InfoSub
{
    std::function<void(std::string const&)> send_;
    boost::asio::ip::tcp::endpoint endpoint_;

    UDPInfoSub(
        Source& source,
        std::function<void(std::string const&)>& sendResponse,
        boost::asio::ip::tcp::endpoint const& remoteEndpoint)
        : InfoSub(source), send_(sendResponse), endpoint_(remoteEndpoint)
    {
    }

    struct RefCountedSub
    {
        std::shared_ptr<UDPInfoSub> sub;
        size_t refCount;

        RefCountedSub(std::shared_ptr<UDPInfoSub> s)
            : sub(std::move(s)), refCount(1)
        {
        }
    };

    static inline std::mutex mtx_;
    static inline std::map<boost::asio::ip::tcp::endpoint, RefCountedSub> map_;

public:
    static std::shared_ptr<UDPInfoSub>
    getInfoSub(
        Source& source,
        std::function<void(std::string const&)>& sendResponse,
        boost::asio::ip::tcp::endpoint const& remoteEndpoint)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = map_.find(remoteEndpoint);
        if (it != map_.end())
        {
            it->second.refCount++;
            return it->second.sub;
        }

        auto sub = std::shared_ptr<UDPInfoSub>(
            new UDPInfoSub(source, sendResponse, remoteEndpoint));
        map_.emplace(remoteEndpoint, RefCountedSub(sub));
        return sub;
    }

    static bool
    increment(boost::asio::ip::tcp::endpoint const& remoteEndpoint)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = map_.find(remoteEndpoint);
        if (it != map_.end())
        {
            it->second.refCount++;
            return true;
        }
        return false;
    }

    bool
    increment()
    {
        return increment(endpoint_);
    }

    static bool
    destroy(boost::asio::ip::tcp::endpoint const& remoteEndpoint)
    {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = map_.find(remoteEndpoint);
        if (it != map_.end())
        {
            if (--it->second.refCount == 0)
            {
                map_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool
    destroy()
    {
        return destroy(endpoint_);
    }

    void
    send(Json::Value const& jv, bool) override
    {
        std::string const str = to_string(jv);
        send_(str);
    }

    boost::asio::ip::tcp::endpoint const&
    endpoint() const
    {
        return endpoint_;
    }
};
}  // namespace ripple
#endif
