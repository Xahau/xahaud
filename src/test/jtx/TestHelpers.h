//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 Ripple Labs Inc.

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
#ifndef RIPPLE_TEST_JTX_TESTHELPERS_H_INCLUDED
#define RIPPLE_TEST_JTX_TESTHELPERS_H_INCLUDED

#include <ripple/basics/base_uint.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/protocol/Quality.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/Env.h>

#include <vector>

namespace ripple {
namespace test {
namespace jtx {

// Functions used in debugging
Json::Value
getAccountOffers(Env& env, AccountID const& acct, bool current = false);

inline Json::Value
getAccountOffers(Env& env, Account const& acct, bool current = false)
{
    return getAccountOffers(env, acct.id(), current);
}

Json::Value
getAccountLines(Env& env, AccountID const& acctId);

inline Json::Value
getAccountLines(Env& env, Account const& acct)
{
    return getAccountLines(env, acct.id());
}

template <typename... IOU>
Json::Value
getAccountLines(Env& env, AccountID const& acctId, IOU... ious)
{
    auto const jrr = getAccountLines(env, acctId);
    Json::Value res;
    for (auto const& line : jrr[jss::lines])
    {
        for (auto const& iou : {ious...})
        {
            if (line[jss::currency].asString() == to_string(iou.currency))
            {
                Json::Value v;
                v[jss::currency] = line[jss::currency];
                v[jss::balance] = line[jss::balance];
                v[jss::limit] = line[jss::limit];
                v[jss::account] = line[jss::account];
                res[jss::lines].append(v);
            }
        }
    }
    if (!res.isNull())
        return res;
    return jrr;
}

[[nodiscard]] bool
checkArraySize(Json::Value const& val, unsigned int size);

// Helper function that returns the owner count on an account.
std::uint32_t
ownerCount(test::jtx::Env const& env, test::jtx::Account const& account);

PrettyAmount
xrpMinusFee(Env const& env, std::int64_t xrpAmount);

Json::Value
ledgerEntryRoot(Env& env, Account const& acct);

Json::Value
ledgerEntryState(
    Env& env,
    Account const& acct_a,
    Account const& acct_b,
    std::string const& currency);

}  // namespace jtx
}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_TESTHELPERS_H_INCLUDED