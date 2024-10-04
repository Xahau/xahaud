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
#include <test/jtx/escrow.h>

namespace ripple {
namespace test {
namespace jtx {

namespace escrow {

void
finish_time::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfFinishAfter.jsonName] = value_.time_since_epoch().count();
}

void
cancel_time::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfCancelAfter.jsonName] = value_.time_since_epoch().count();
}

void
condition::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfCondition.jsonName] = value_;
}

void
fulfillment::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfFulfillment.jsonName] = value_;
}

void
escrow_id::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfEscrowID.jsonName] = to_string(value_);
}

Json::Value
create(
    jtx::Account const& account,
    jtx::Account const& to,
    STAmount const& amount)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::EscrowCreate;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv[jss::Destination] = to.human();
    jv[jss::Amount] = amount.getJson(JsonOptions::none);
    return jv;
}

Json::Value
finish(jtx::Account const& account, jtx::Account const& from, std::uint32_t seq)
{
    Json::Value jv;
    jv[jss::TransactionType] = jss::EscrowFinish;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv[sfOwner.jsonName] = from.human();
    jv[sfOfferSequence.jsonName] = seq;
    return jv;
}

Json::Value
finish(jtx::Account const& account, jtx::Account const& from)
{
    Json::Value jv;
    jv[jss::TransactionType] = jss::EscrowFinish;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv[sfOwner.jsonName] = from.human();
    return jv;
}

Json::Value
cancel(jtx::Account const& account, jtx::Account const& from, std::uint32_t seq)
{
    Json::Value jv;
    jv[jss::TransactionType] = jss::EscrowCancel;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv[sfOwner.jsonName] = from.human();
    jv[sfOfferSequence.jsonName] = seq;
    return jv;
}

Json::Value
cancel(jtx::Account const& account, jtx::Account const& from)
{
    Json::Value jv;
    jv[jss::TransactionType] = jss::EscrowCancel;
    jv[jss::Flags] = tfUniversal;
    jv[jss::Account] = account.human();
    jv[sfOwner.jsonName] = from.human();
    return jv;
}

}  // namespace escrow

}  // namespace jtx
}  // namespace test
}  // namespace ripple
