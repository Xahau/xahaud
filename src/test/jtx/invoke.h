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

#ifndef RIPPLE_TEST_JTX_INVOKE_H_INCLUDED
#define RIPPLE_TEST_JTX_INVOKE_H_INCLUDED

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

/** Invoke operations. */
namespace invoke {

Json::Value
invoke(
    jtx::Account const& account);

Json::Value
invoke(
    jtx::Account const& account, 
    std::optional<jtx::Account> const& dest,
    std::optional<std::string> const& blob);

/** Set the optional "Blob" on a JTx */
class blob
{
private:
   std::string value_;

public:
    explicit blob(std::string const& value) : value_(value)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Sets the optional "Destination" on a JTx. */
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

}  // namespace invoke

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif // RIPPLE_TEST_JTX_INVOKE_H_INCLUDED
