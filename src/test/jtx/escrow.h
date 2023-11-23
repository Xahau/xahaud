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

#ifndef RIPPLE_TEST_JTX_ESCROW_H_INCLUDED
#define RIPPLE_TEST_JTX_ESCROW_H_INCLUDED

#include <ripple/basics/chrono.h>
#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

/** Escrow operations. */
namespace escrow {

Json::Value
create(
    jtx::Account const& account,
    jtx::Account const& to,
    STAmount const& amount);

Json::Value
finish(
    jtx::Account const& account,
    jtx::Account const& from,
    std::uint32_t seq);

Json::Value
cancel(
    jtx::Account const& account,
    jtx::Account const& from,
    std::uint32_t seq);

/** Set the "FinishAfter" time tag on a JTx */
class finish_time
{
private:
    NetClock::time_point value_;

public:
    explicit finish_time(NetClock::time_point const& value) : value_(value)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Set the "CancelAfter" time tag on a JTx */
class cancel_time
{
private:
    NetClock::time_point value_;

public:
    explicit cancel_time(NetClock::time_point const& value) : value_(value)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

class condition
{
private:
    std::string value_;

public:
    explicit condition(Slice cond) : value_(strHex(cond))
    {
    }

    template <size_t N>
    explicit condition(std::array<std::uint8_t, N> c) : condition(makeSlice(c))
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

class fulfillment
{
private:
    std::string value_;

public:
    explicit fulfillment(Slice condition) : value_(strHex(condition))
    {
    }

    template <size_t N>
    explicit fulfillment(std::array<std::uint8_t, N> f)
        : fulfillment(makeSlice(f))
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

}  // namespace escrow

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_ESCROW_H_INCLUDED
