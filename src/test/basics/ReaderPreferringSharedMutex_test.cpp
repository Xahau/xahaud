//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Ripple Labs Inc.

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

#include <xrpl/basics/ReaderPreferringSharedMutex.h>
#include <xrpl/beast/unit_test.h>

#include <mutex>
#include <shared_mutex>

namespace ripple {
namespace test {

class ReaderPreferringSharedMutex_test : public beast::unit_test::suite
{
    void
    testExclusiveAndShared()
    {
        testcase("exclusive and shared locking");

        ReaderPreferringSharedMutex mutex;

        BEAST_EXPECT(mutex.try_lock());
        BEAST_EXPECT(!mutex.try_lock());
        BEAST_EXPECT(!mutex.try_lock_shared());
        mutex.unlock();

        mutex.lock_shared();
        BEAST_EXPECT(mutex.try_lock_shared());
        BEAST_EXPECT(!mutex.try_lock());
        mutex.unlock_shared();
        mutex.unlock_shared();

        {
            std::shared_lock shared{mutex};
            BEAST_EXPECT(!mutex.try_lock());
            BEAST_EXPECT(mutex.try_lock_shared());
            mutex.unlock_shared();
        }

        {
            std::unique_lock exclusive{mutex};
            BEAST_EXPECT(!mutex.try_lock());
            BEAST_EXPECT(!mutex.try_lock_shared());
        }

        mutex.lock();
        BEAST_EXPECT(!mutex.try_lock_shared());
        mutex.unlock();
        BEAST_EXPECT(mutex.try_lock_shared());
        mutex.unlock_shared();
    }

    void
    run() override
    {
        testExclusiveAndShared();
    }
};

BEAST_DEFINE_TESTSUITE(ReaderPreferringSharedMutex, ripple_basics, ripple);

}  // namespace test
}  // namespace ripple
