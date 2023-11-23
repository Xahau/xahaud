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

#ifndef RIPPLE_TEST_JTX_GENESIS_H_INCLUDED
#define RIPPLE_TEST_JTX_GENESIS_H_INCLUDED

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

/** Genesis operations. */
namespace genesis {

struct GenMint
{
    std::optional<std::string> dest;
    std::optional<jtx::PrettyAmount> amt;
    std::optional<std::string> marks;
    std::optional<std::string> flags;

    GenMint(
        AccountID const& dst,
        jtx::PrettyAmount const& x,
        std::optional<std::string> m = std::nullopt,
        std::optional<std::string> f = std::nullopt)
        : dest(toBase58(dst)), amt(x), marks(m), flags(f)
    {
    }
};

Json::Value
mint(jtx::Account const& account, std::vector<genesis::GenMint> mints);

Json::Value
setMintHook(jtx::Account const& account);

Json::Value
setAcceptHook(jtx::Account const& account);

std::string
makeBlob(std::vector<std::tuple<
                std::optional<AccountID>,
                std::optional<STAmount>,
                std::optional<uint256>,
                std::optional<uint256>>> entries);

}  // namespace genesis

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif // RIPPLE_TEST_JTX_GENESIS_H_INCLUDED
