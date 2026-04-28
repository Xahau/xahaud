//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#ifndef RIPPLE_RPC_CATALOGUESTREAM_H_INCLUDED
#define RIPPLE_RPC_CATALOGUESTREAM_H_INCLUDED

#include <xrpld/nodestore/NodeObject.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/beast/utility/Journal.h>

#include <boost/iostreams/filtering_stream.hpp>

#include <functional>
#include <optional>

namespace ripple {
namespace RPC {

using CatalogueInputStream = boost::iostreams::filtering_istream;
using CatalogueOutputStream = boost::iostreams::filtering_ostream;

std::size_t
serializeStateMapToStream(
    SHAMap const& stateMap,
    CatalogueOutputStream& stream,
    std::optional<std::reference_wrapper<SHAMap const>> prevStateMap =
        std::nullopt);

std::size_t
serializeTxMapToStream(SHAMap const& txMap, CatalogueOutputStream& stream);

bool
deserializeStateMapFromStream(
    SHAMap& stateMap,
    CatalogueInputStream& stream,
    NodeObjectType flushType,
    beast::Journal const& j);

bool
deserializeTxMapFromStream(
    SHAMap& txMap,
    CatalogueInputStream& stream,
    NodeObjectType flushType,
    beast::Journal const& j);

}  // namespace RPC
}  // namespace ripple

#endif
