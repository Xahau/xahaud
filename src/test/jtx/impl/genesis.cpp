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

#include <ripple/app/tx/impl/XahauGenesis.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx/genesis.h>

namespace ripple {
namespace test {
namespace jtx {

namespace genesis {

Json::Value
mint(jtx::Account const& account, std::vector<genesis::GenMint> mints)
{
    using namespace jtx;
    Json::Value jv;
    jv[jss::TransactionType] = jss::GenesisMint;
    jv[jss::Account] = account.human();
    jv[jss::GenesisMints] = Json::arrayValue;

    uint32_t counter = 0;
    for (auto const& m : mints)
    {
        Json::Value inner = Json::objectValue;
        if (m.dest)
            inner[jss::Destination] = *m.dest;
        if (m.amt)
            inner[jss::Amount] = (*m.amt).value().getJson(JsonOptions::none);
        if (m.marks)
            inner[jss::GovernanceMarks] = *m.marks;
        if (m.flags)
            inner[jss::GovernanceFlags] = *m.flags;

        jv[jss::GenesisMints][counter] = Json::objectValue;
        jv[jss::GenesisMints][counter][jss::GenesisMint] = inner;
        counter++;
    }
    return jv;
}

Json::Value
setMintHook(jtx::Account const& account)
{
    using namespace jtx;
    Json::Value tx;
    tx[jss::Account] = account.human();
    tx[jss::TransactionType] = "SetHook";
    tx[jss::Hooks] = Json::arrayValue;
    tx[jss::Hooks][0u] = Json::objectValue;
    tx[jss::Hooks][0u][jss::Hook] = Json::objectValue;
    tx[jss::Hooks][0u][jss::Hook][jss::HookOn] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    tx[jss::Hooks][0u][jss::Hook][jss::HookNamespace] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    tx[jss::Hooks][0u][jss::Hook][jss::HookApiVersion] = 0;

    tx[jss::Hooks][0u][jss::Hook][jss::CreateCode] =
        strHex(XahauGenesis::MintTestHook);
    return tx;
}

Json::Value
setAcceptHook(jtx::Account const& account)
{
    using namespace jtx;
    Json::Value tx;
    tx[jss::Account] = account.human();
    tx[jss::TransactionType] = "SetHook";
    tx[jss::Hooks] = Json::arrayValue;
    tx[jss::Hooks][0u] = Json::objectValue;
    tx[jss::Hooks][0u][jss::Hook] = Json::objectValue;
    tx[jss::Hooks][0u][jss::Hook][jss::HookOn] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    tx[jss::Hooks][0u][jss::Hook][jss::HookNamespace] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    tx[jss::Hooks][0u][jss::Hook][jss::HookApiVersion] = 0;
    tx[jss::Hooks][0u][jss::Hook][jss::Flags] = 5;
    tx[jss::Hooks][0u][jss::Hook][jss::CreateCode] =
        strHex(XahauGenesis::AcceptHook);
    return tx;
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

}  // namespace genesis

}  // namespace jtx
}  // namespace test
}  // namespace ripple
