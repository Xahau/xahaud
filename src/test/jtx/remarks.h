//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL Labs

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

#ifndef RIPPLE_TEST_JTX_REMARKS_H_INCLUDED
#define RIPPLE_TEST_JTX_REMARKS_H_INCLUDED

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

namespace remarks {

struct remark
{
    std::string name;
    std::optional<std::string> value;
    std::optional<std::uint32_t> flags;
    remark(
        std::string name_,
        std::optional<std::string> value_ = std::nullopt,
        std::optional<std::uint32_t> flags_ = std::nullopt)
        : name(name_), value(value_), flags(flags_)
    {
        if (value_)
            value = *value_;

        if (flags_)
            flags = *flags_;
    }
};

Json::Value
setRemarks(
    jtx::Account const& account,
    uint256 const& id,
    std::vector<remark> const& marks);

}  // namespace remarks

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_REMARKS_H_INCLUDED