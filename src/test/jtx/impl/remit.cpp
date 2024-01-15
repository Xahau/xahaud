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
#include <test/jtx/remit.h>

namespace ripple {
namespace test {
namespace jtx {
namespace remit {

Json::Value
remit(
    jtx::Account const& account,
    jtx::Account const& dest,
    std::optional<std::uint32_t> const& dstTag)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::Remit;
    jv[jss::Account] = account.human();
    jv[jss::Destination] = dest.human();
    if (dstTag)
        jv[sfDestinationTag.jsonName] = *dstTag;
    return jv;
}

void
amts::operator()(Env& env, JTx& jt) const
{
    auto& ja = jt.jv[sfAmounts.getJsonName()];
    for (std::size_t i = 0; i < amts_.size(); ++i)
    {
        ja[i][sfAmountEntry.jsonName] = Json::Value{};
        ja[i][sfAmountEntry.jsonName][jss::Amount] =
            amts_[i].getJson(JsonOptions::none);
    }
    jt.jv[sfAmounts.jsonName] = ja;
}

void
blob::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfBlob.jsonName] = blob_;
}

void
inform::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfInform.jsonName] = inform_.human();
}

void
token_ids::operator()(Env& env, JTx& jt) const
{
    for (std::size_t i = 0; i < token_ids_.size(); ++i)
    {
        jt.jv[sfURITokenIDs.jsonName] = Json::arrayValue;
        jt.jv[sfURITokenIDs.jsonName][i] = token_ids_[i];
    }
}

void
uri::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfMintURIToken.jsonName] = Json::Value{};
    jt.jv[sfMintURIToken.jsonName][sfURI.jsonName] = strHex(uri_);
    if (flags_)
    {
        jt.jv[sfMintURIToken.jsonName][sfFlags.jsonName] = *flags_;
    }
}

}  // namespace remit
}  // namespace jtx
}  // namespace test
}  // namespace ripple
