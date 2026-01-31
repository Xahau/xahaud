//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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
#include <test/jtx/cron.h>

namespace ripple {
namespace test {
namespace jtx {

namespace cron {

// Set a cron.
Json::Value
set(jtx::Account const& account)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::CronSet;
    jv[jss::Account] = account.human();
    return jv;
}

void
startTime::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfStartTime.jsonName] = startTime_;
}

void
delay::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfDelaySeconds.jsonName] = delay_;
}

void
repeat::operator()(Env& env, JTx& jt) const
{
    jt.jv[sfRepeatCount.jsonName] = repeat_;
}

}  // namespace cron

}  // namespace jtx
}  // namespace test
}  // namespace ripple
