//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2019 Ripple Labs Inc.

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

#include <ripple/json/json_reader.h>
#include <ripple/json/json_writer.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/import.h>

namespace ripple {
namespace test {
namespace jtx {

namespace import {

// Import tx.
Json::Value
import(
    jtx::Account const& account,
    Json::Value const& xpop,
    std::optional<jtx::Account> const& issuer)
{
    using namespace jtx;
    Json::Value jv;
    std::string strJson = Json::FastWriter().write(xpop);
    jv[jss::TransactionType] = jss::Import;
    jv[jss::Account] = account.human();
    jv[jss::Blob] = strHex(strJson);
    if (issuer)
        jv[sfIssuer.jsonName] = issuer->human();
    return jv;
}

Json::Value
loadXpop(std::string content)
{
    // If the string is empty, return an empty Json::Value
    if (content.empty())
    {
        std::cout << "JSON string was empty"
                    << "\n";
        return {};
    }

    Json::Value jsonValue;
    Json::Reader reader;
    reader.parse(content, jsonValue);
    return jsonValue;
}

}  // namespace import

}  // namespace jtx
}  // namespace test
}  // namespace ripple
