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

#ifndef RIPPLE_TEST_JTX_URITOKEN_H_INCLUDED
#define RIPPLE_TEST_JTX_URITOKEN_H_INCLUDED

#include <ripple/protocol/STAmount.h>
#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

namespace uritoken {

uint256
tokenid(jtx::Account const& account, std::string const& uri);

Json::Value
mint(jtx::Account const& account, std::string const& uri);

/** Sets the optional Destination on a JTx. */
class dest
{
private:
    jtx::Account dest_;

public:
    explicit dest(jtx::Account const& dest) : dest_(dest)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Sets the optional Amount on a JTx. */
class amt
{
private:
    STAmount amt_;

public:
    explicit amt(STAmount const& amt) : amt_(amt)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

Json::Value
burn(jtx::Account const& account, std::string const& id);

Json::Value
sell(jtx::Account const& account, std::string const& id);

Json::Value
cancel(jtx::Account const& account, std::string const& id);

Json::Value
buy(jtx::Account const& account, std::string const& id);

}  // namespace uritoken

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_URITOKEN_H_INCLUDED
