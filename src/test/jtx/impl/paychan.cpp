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

#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/paychan.h>

namespace ripple {
namespace test {
namespace jtx {

namespace paychan {

Json::Value
create(
    jtx::Account const& account,
    jtx::Account const& to,
    STAmount const& amount,
    NetClock::duration const& settleDelay,
    PublicKey const& pk,
    std::optional<NetClock::time_point> const& cancelAfter,
    std::optional<std::uint32_t> const& dstTag)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::PaymentChannelCreate;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv[jss::Destination] = to.human();
    jv[jss::Amount] = amount.getJson(JsonOptions::none);
    jv["SettleDelay"] = settleDelay.count();
    jv["PublicKey"] = strHex(pk.slice());
    if (cancelAfter)
        jv["CancelAfter"] = cancelAfter->time_since_epoch().count();
    if (dstTag)
        jv["DestinationTag"] = *dstTag;
    return jv;
}

Json::Value
fund(
    jtx::Account const& account,
    uint256 const& channel,
    STAmount const& amount,
    std::optional<NetClock::time_point> const& expiration)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::PaymentChannelFund;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv["Channel"] = to_string(channel);
    jv[jss::Amount] = amount.getJson(JsonOptions::none);
    if (expiration)
        jv["Expiration"] = expiration->time_since_epoch().count();
    return jv;
}

Json::Value
claim(
    jtx::Account const& account,
    uint256 const& channel,
    std::optional<STAmount> const& balance,
    std::optional<STAmount> const& amount,
    std::optional<Slice> const& signature,
    std::optional<PublicKey> const& pk)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::PaymentChannelClaim;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv["Channel"] = to_string(channel);
    if (amount)
        jv[jss::Amount] = amount->getJson(JsonOptions::none);
    if (balance)
        jv["Balance"] = balance->getJson(JsonOptions::none);
    if (signature)
        jv["Signature"] = strHex(*signature);
    if (pk)
        jv["PublicKey"] = strHex(pk->slice());
    return jv;
}

}  // namespace paychan

}  // namespace jtx
}  // namespace test
}  // namespace ripple
