//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#ifndef RIPPLE_TEST_JTX_CRON_H_INCLUDED
#define RIPPLE_TEST_JTX_CRON_H_INCLUDED

#include <test/jtx/Account.h>
#include <test/jtx/Env.h>

namespace ripple {
namespace test {
namespace jtx {

/** Cron operations. */
namespace cron {

/** Set a cron. */
Json::Value
set(jtx::Account const& account);

/** Sets the optional StartTime on a JTx. */
class startTime
{
private:
    uint32_t startTime_;

public:
    explicit startTime(uint32_t startTime) : startTime_(startTime)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Sets the optional DelaySeconds on a JTx. */
class delay
{
private:
    uint32_t delay_;

public:
    explicit delay(uint32_t delay) : delay_(delay)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

/** Sets the optional RepeatCount on a JTx. */
class repeat
{
private:
    uint32_t repeat_;

public:
    explicit repeat(uint32_t repeat) : repeat_(repeat)
    {
    }

    void
    operator()(Env&, JTx& jtx) const;
};

}  // namespace cron

}  // namespace jtx

}  // namespace test
}  // namespace ripple

#endif  // RIPPLE_TEST_JTX_CRON_H_INCLUDED
