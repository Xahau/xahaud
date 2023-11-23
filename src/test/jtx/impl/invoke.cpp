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

#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/invoke.h>

namespace ripple {
namespace test {
namespace jtx {

namespace invoke {

// Invoke a tx.

Json::Value
invoke(
    jtx::Account const& account, 
    std::optional<jtx::Account> const& dest)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::Invoke;
    jv[jss::Account] = account.human();
    if (dest)
        jv[jss::Destination] = dest->human();
    
    return jv;
}

Json::Value
invoke(
    jtx::Account const& account, 
    std::optional<jtx::Account> const& dest,
    std::optional<std::string> const& blob)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::Invoke;
    jv[jss::Account] = account.human();
    if (dest)
        jv[jss::Destination] = dest->human();

    if (blob.has_value())
        jv[jss::Blob] = blob.value();
    
    return jv;
}

std::string
makeBlob(std::vector<std::tuple<
        std::optional<AccountID>,
        std::optional<STAmount>,
        std::optional<uint256>,
        std::optional<uint256>>> entries)
{
    std::string blob = "F060";

    for (auto const& [acc, amt, flags, marks] : entries)
    {
        STObject m(sfGenesisMint);
        if (acc)
            m.setAccountID(sfDestination, *acc);
        if (amt)
            m.setFieldAmount(sfAmount, *amt);
        if (flags)
            m.setFieldH256(sfGovernanceFlags, *flags);
        if (marks)
            m.setFieldH256(sfGovernanceMarks, *marks);

        Serializer s;
        m.add(s);
        blob += "E060" + strHex(s.getData()) + "E1";
    }

    blob += "F1";
    return blob;
}

}  // namespace invoke

}  // namespace jtx
}  // namespace test
}  // namespace ripple
