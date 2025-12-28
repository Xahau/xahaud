/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright 2022, Nikolaos D. Bougalis <nikb@bougalis.net>

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

#ifndef RIPPLE_BASICS_SPINLOCK_H_INCLUDED
#define RIPPLE_BASICS_SPINLOCK_H_INCLUDED

#include <xrpl/beast/utility/instrumentation.h>
#include <atomic>
#include <concepts>
#include <limits>
#include <type_traits>

#ifndef __aarch64__
#include <immintrin.h>
#endif

namespace ripple {

namespace detail {

/** Inform the processor that we are in a tight spin-wait loop.

    Spinlocks caught in tight loops can result in the processor's pipeline
    filling up with comparison operations, resulting in a misprediction at
    the time the lock is finally acquired, necessitating pipeline flushing
    which is ridiculously expensive and results in very high latency.

    This function instructs the processor to "pause" for some architecture
    specific amount of time, to prevent this.
 */
inline void
spin_pause() noexcept
{
#ifdef __aarch64__
    asm volatile("yield");
#else
    _mm_pause();
#endif
}

}  // namespace detail

//------------------------------------------------------------------------------

/** Attempt to acquire a spinlock without blocking.

    @tparam T An unsigned integral type.
    @param lock The atomic variable used as the lock.
    @return true if the lock was acquired, false if it was already held.
 */
template <typename T>
    requires(std::is_unsigned_v<T> && std::atomic<T>::is_always_lock_free)
[[nodiscard]] bool
spin_try_lock(std::atomic<T>& lock) noexcept
{
    T expected = 0;

    return lock.compare_exchange_strong(
        expected,
        std::numeric_limits<T>::max(),
        std::memory_order::acquire,
        std::memory_order::relaxed);
}

/** Acquire a spinlock, blocking until available.

    Uses a TTAS (test-and-test-and-set) pattern to reduce cache coherency
    traffic during contention.

    @tparam T An unsigned integral type.
    @param lock The atomic variable used as the lock.
 */
template <typename T>
    requires(std::is_unsigned_v<T> && std::atomic<T>::is_always_lock_free)
void
spin_lock(std::atomic<T>& lock) noexcept
{
    T expected = 0;

    while (!lock.compare_exchange_weak(
        expected,
        std::numeric_limits<T>::max(),
        std::memory_order::acquire,
        std::memory_order::relaxed))
    {
        expected = 0;

        while (lock.load(std::memory_order::relaxed) != 0)
            detail::spin_pause();
    }
}

/** Release a spinlock.

    @tparam T An unsigned integral type.
    @param lock The atomic variable used as the lock.
 */
template <typename T>
    requires(std::is_unsigned_v<T> && std::atomic<T>::is_always_lock_free)
void
spin_unlock(std::atomic<T>& lock) noexcept
{
    lock.store(0, std::memory_order::release);
}

//------------------------------------------------------------------------------

/** A Lockable interface to a spinlock implemented on top of an atomic.

    @tparam T An unsigned integral type.

    @note Using `packed_spinlock` and `spinlock` against the same underlying
          atomic integer can result in `spinlock` not being able to actually
          acquire the lock during periods of high contention, because of how
          the two locks operate: `spinlock` will spin trying to grab all the
          bits at once, whereas any given `packed_spinlock` will only try to
          grab one bit at a time. Caveat emptor.

    This class meets the requirements of Lockable:
        https://en.cppreference.com/w/cpp/named_req/Lockable
 */
template <typename T>
    requires(std::is_unsigned_v<T> && std::atomic<T>::is_always_lock_free)
class spinlock
{
    std::atomic<T>& lock_;

public:
    spinlock(spinlock const&) = delete;
    spinlock&
    operator=(spinlock const&) = delete;

    /** Construct a spinlock handle.

        @param lock The atomic integer to spin against.

        @note For performance reasons, you should strive to have `lock` be
              on a cacheline by itself.
     */
    explicit spinlock(std::atomic<T>& lock) noexcept : lock_(lock)
    {
    }

    [[nodiscard]] bool
    try_lock() noexcept
    {
        return spin_try_lock(lock_);
    }

    void
    lock() noexcept
    {
        spin_lock(lock_);
    }

    void
    unlock() noexcept
    {
        spin_unlock(lock_);
    }
};

//------------------------------------------------------------------------------

/** A Lockable interface to a packed spinlock implemented on top of an atomic.

    Packed spinlocks offer tremendous space-efficient lock-sharding but
    they come at a cost.

    First, the implementation is necessarily low-level and uses advanced
    features like memory ordering and highly platform-specific tricks to
    maximize performance. This imposes a significant and ongoing cost to
    developers.

    Second, and perhaps most important, is that the packing of multiple
    locks into a single integer which, albeit space-efficient, also has
    performance implications stemming from data dependencies, increased
    cache-coherency traffic between processors and heavier loads on the
    processor's load/store units.

    To be sure, these locks can have advantages but they are definitely
    not general purpose locks and should not be thought of or used that
    way. The use cases for them are likely few and far between; without
    a compelling reason to use them, backed by profiling data, it might
    be best to use one of the standard locking primitives instead. Note
    that in most common platforms, `std::mutex` is so heavily optimized
    that it can, usually, outperform spinlocks.

    @tparam T An unsigned integral type (e.g. std::uint16_t)

    This class meets the requirements of Lockable:
        https://en.cppreference.com/w/cpp/named_req/Lockable
 */
template <typename T>
    requires(
        std::is_unsigned_v<T> && std::atomic<T>::is_always_lock_free &&
        requires(std::atomic<T>& a, T v) {
            { a.fetch_or(v) } -> std::same_as<T>;
            { a.fetch_and(v) } -> std::same_as<T>;
        })
class packed_spinlock
{
    std::atomic<T>& bits_;
    T const mask_;

public:
    packed_spinlock(packed_spinlock const&) = delete;
    packed_spinlock&
    operator=(packed_spinlock const&) = delete;

    /** Construct a packed spinlock handle for a single bit.

        @param lock The atomic integer inside which the spinlock is packed.
        @param index The index of the spinlock this object acquires.

        @note For performance reasons, you should strive to have `lock` be
              on a cacheline by itself.
     */
    packed_spinlock(std::atomic<T>& lock, int index) noexcept
        : bits_(lock), mask_(static_cast<T>(1) << index)
    {
        XRPL_ASSERT(
            index >= 0 && (mask_ != 0),
            "ripple::packed_spinlock::packed_spinlock : valid index and mask");
    }

    [[nodiscard]] bool
    try_lock() noexcept
    {
        return (bits_.fetch_or(mask_, std::memory_order::acquire) & mask_) == 0;
    }

    void
    lock() noexcept
    {
        while (!try_lock())
        {
            // The use of relaxed memory ordering here is intentional and
            // serves to help reduce cache coherency traffic during times
            // of contention by avoiding writes that would definitely not
            // result in the lock being acquired.
            while ((bits_.load(std::memory_order::relaxed) & mask_) != 0)
                detail::spin_pause();
        }
    }

    void
    unlock() noexcept
    {
        bits_.fetch_and(~mask_, std::memory_order::release);
    }
};

}  // namespace ripple

#endif
