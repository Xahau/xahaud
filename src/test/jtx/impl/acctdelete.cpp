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

#include <test/jtx/Env.h>
#include <test/jtx/acctdelete.h>

namespace ripple {
namespace test {
namespace jtx {

// Delete account.  If successful transfer remaining XRP to dest.
Json::Value
acctdelete(jtx::Account const& account, jtx::Account const& dest)
{
    Json::Value jv;
    jv[sfAccount.jsonName] = account.human();
    jv[sfDestination.jsonName] = dest.human();
    jv[sfTransactionType.jsonName] = jss::AccountDelete;
    return jv;
}

void
incLgrSeqForAccDel(jtx::Env& env, jtx::Account const& acc, std::uint32_t margin)
{
    int const delta = [&]() -> int {
        if (env.seq(acc) + 255 > env.current()->seq())
            return env.seq(acc) - env.current()->seq() + 255 - margin;
        return 0;
    }();
    for (int i = 0; i < delta; ++i)
        env.close();
}

}  // namespace jtx
}  // namespace test
}  // namespace ripple
