//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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

#ifndef RIPPLE_TEST_JTX_HOOK_H_INCLUDED
#define RIPPLE_TEST_JTX_HOOK_H_INCLUDED

#include <ripple/json/json_value.h>
#include <ripple/protocol/jss.h>
#include <optional>
#include <test/jtx/Account.h>

namespace ripple {
namespace test {
namespace jtx {

Json::Value
hook(
    Account const& account,
    std::optional<std::vector<Json::Value>> hooks,
    std::uint32_t flags);

Json::Value
hso(std::vector<uint8_t> const& wasmBytes, void (*f)(Json::Value& jv) = 0);

Json::Value
hsov1(
    std::vector<uint8_t> const& wasmBytes,
    uint16_t const& apiVersion,
    STAmount const& fee,
    void (*f)(Json::Value& jv) = 0);

Json::Value
hso(std::string const& wasmHex, void (*f)(Json::Value& jv) = 0);

Json::Value
hso_delete(void (*f)(Json::Value& jv) = 0);

class JSSHasher
{
public:
    size_t
    operator()(const Json::StaticString& n) const
    {
        return std::hash<std::string_view>{}(n.c_str());
    }
};

class JSSEq
{
public:
    bool
    operator()(const Json::StaticString& a, const Json::StaticString& b) const
    {
        return a == b;
    }
};

using JSSMap =
    std::unordered_map<Json::StaticString, Json::Value, JSSHasher, JSSEq>;

}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif
