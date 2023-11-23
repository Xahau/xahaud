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

#include <ripple/protocol/jss.h>
#include <test/jtx/uritoken.h>

namespace ripple {
namespace test {
namespace jtx {
namespace uritoken {

uint256
tokenid(jtx::Account const& account, std::string const& uri)
{
    auto const k = keylet::uritoken(account, Blob(uri.begin(), uri.end()));
    return k.key;
}

Json::Value
mint(jtx::Account const& account, std::string const& uri)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::URITokenMint;
    jv[jss::Account] = account.human();
    jv[sfURI.jsonName] = strHex(uri);
    return jv;
}

void
dest::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfDestination.jsonName] = dest_.human();
}

void
amt::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfAmount.jsonName] = amt_.getJson(JsonOptions::none);
}

Json::Value
burn(jtx::Account const& account, std::string const& id)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::URITokenBurn;
    jv[jss::Account] = account.human();
    jv[sfURITokenID.jsonName] = id;
    return jv;
}

Json::Value
sell(jtx::Account const& account, std::string const& id)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::URITokenCreateSellOffer;
    jv[jss::Account] = account.human();
    jv[sfURITokenID.jsonName] = id;
    return jv;
}

Json::Value
cancel(jtx::Account const& account, std::string const& id)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::URITokenCancelSellOffer;
    jv[jss::Account] = account.human();
    jv[sfURITokenID.jsonName] = id;
    return jv;
}

Json::Value
buy(jtx::Account const& account, std::string const& id)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::URITokenBuy;
    jv[jss::Account] = account.human();
    jv[sfURITokenID.jsonName] = id;
    return jv;
}

}  // namespace uritoken
}  // namespace jtx
}  // namespace test
}  // namespace ripple
