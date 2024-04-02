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
#include <test/jtx/remarks.h>

namespace ripple {
namespace test {
namespace jtx {
namespace remarks {

Json::Value
setRemarks(
    jtx::Account const& account,
    uint256 const& id,
    std::vector<remark> const& marks)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::SetRemarks;
    jv[jss::Account] = account.human();
    jv[sfObjectID.jsonName] = strHex(id);
    auto& ja = jv[sfRemarks.getJsonName()];
    for (std::size_t i = 0; i < marks.size(); ++i)
    {
        ja[i][sfRemark.jsonName] = Json::Value{};
        ja[i][sfRemark.jsonName][sfRemarkName.jsonName] = marks[i].name;
        if (marks[i].value)
            ja[i][sfRemark.jsonName][sfRemarkValue.jsonName] = *marks[i].value;
        if (marks[i].flags)
            ja[i][sfRemark.jsonName][sfFlags.jsonName] = *marks[i].flags;
    }
    jv[sfRemarks.jsonName] = ja;
    return jv;
}

}  // namespace remarks
}  // namespace jtx
}  // namespace test
}  // namespace ripple
