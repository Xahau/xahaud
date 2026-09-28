//------------------------------------------------------------------------------
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
//==============================================================================

#ifndef RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED
#define RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED

#include <xrpl/basics/ByteUtilities.h>
#include <xrpl/basics/spinlock.h>
#include <xrpl/beast/utility/instrumentation.h>

#include <boost/align.hpp>
#include <boost/predef/os.h>

#include <array>
#include <atomic>
#include <bit>

#include <limits>
#include <new>

#ifdef BOOST_OS_LINUX
#include <sys/mman.h>
#endif

namespace ripple {
namespace slab {
namespace detail {

/// Alignment for hugepage support.
inline constexpr std::size_t pageSize = megabytes(std::size_t(2));

/** Allocate a new 2MB-aligned data buffer.

    @param size Size of the buffer to allocate in bytes.
    @return Pointer to allocated memory, or nullptr on failure.

    @note On Linux, advises the kernel to back the allocation with
          transparent huge pages if available.
 */
[[nodiscard, gnu::malloc]] inline std::uint8_t*
allocateBuffer(std::size_t size) noexcept
{
    auto ptr = reinterpret_cast<std::uint8_t*>(
        boost::alignment::aligned_alloc(pageSize, size));

#if BOOST_OS_LINUX
    if (ptr != nullptr) [[likely]]
        madvise(ptr, size, MADV_HUGEPAGE);
#endif

    return ptr;
}

/** Deallocate a buffer previously allocated by allocateBuffer.

    @param ptr Pointer to buffer, or nullptr (no-op).
 */
inline void
deallocateBuffer(std::uint8_t* ptr) noexcept
{
    boost::alignment::aligned_free(ptr);
}

//------------------------------------------------------------------------------

/** A block of memory metadata for slab allocators.

    Each block manages a single slab buffer and its associated free list.
    Blocks are linked together to form a list of slabs per allocator.

    @note slab_t instances are intentionally never destroyed or reclaimed
          once allocated, to support lock-free iteration of lists of slabs
          without introducing ABA hazards. The data buffer the class holds
          can be released independently.
 */
struct slab_t
{
    /** A node in the intrusive free list.

        Constructed in-place within free memory blocks. The next pointer
        is const because nodes are never modified after construction;
        they are simply constructed anew when reused.
     */
    struct item_t
    {
        item_t* const next;

        constexpr explicit item_t(item_t* n) noexcept : next(n)
        {
        }
    };

    // We need this to be trivially destructible so that we do not
    // have to explicitly invoke the destructor when popping items
    // from the free list or when deallocating the slab buffer.
    static_assert(std::is_trivially_destructible_v<item_t>);

    item_t* head_ = nullptr;
    slab_t* const next_ = nullptr;
    std::atomic<std::uint8_t*> data_{nullptr};
    std::atomic<std::uint8_t> lock_{0};
    std::uint16_t cycles_ = 0;
    std::uint32_t outstanding_ = 0;

    /** Construct a block that is unlinked. */
    constexpr slab_t() noexcept = default;

    /** Construct a block with a link to the next block in the chain.

        @param next Pointer to the next block, or nullptr for the tail.
     */
    constexpr explicit slab_t(slab_t* next) noexcept : next_(next)
    {
    }

    ~slab_t() = default;

    slab_t(slab_t const&) = delete;
    slab_t&
    operator=(slab_t const&) = delete;
    slab_t(slab_t&&) = delete;
    slab_t&
    operator=(slab_t&&) = delete;

    /** Attempt to acquire the block's spinlock.

        @return true if lock was acquired, false if already held.
     */
    [[nodiscard]] bool
    try_lock() noexcept
    {
        return spin_try_lock(lock_);
    }

    /** Acquire the block's spinlock, blocking until available. */
    void
    lock() noexcept
    {
        spin_lock(lock_);
    }

    /** Release the block's spinlock. */
    void
    unlock() noexcept
    {
        spin_unlock(lock_);
    }

    /** Attempt to assign a buffer to this block.

        If the block has no buffer, takes ownership and initializes
        the free list.

        @param buffer Pointer to the buffer to assign.
        @param slabSize Size of the buffer in bytes.
        @param itemSize Size of each item in bytes.
        @return Pointer to first item if assigned, nullptr if block already
                has a buffer.
     */
    [[nodiscard]] std::uint8_t*
    try_assign(
        std::uint8_t* buffer,
        std::size_t slabSize,
        std::size_t itemSize) noexcept
    {
        std::uint8_t* expected = nullptr;

        if (!data_.compare_exchange_strong(
                expected, buffer, std::memory_order::acquire))
            return nullptr;

        spin_lock(lock_);

        head_ = nullptr;
        outstanding_ = 1;

        auto p = buffer;

        for (std::size_t n = (slabSize / itemSize) - 1; n--; p += itemSize)
            head_ = std::construct_at(reinterpret_cast<item_t*>(p), head_);

        spin_unlock(lock_);

        return p;
    }

    /** Attempt to allocate from this block.

        @return Pointer to allocated memory, or nullptr if block is empty.
     */
    [[nodiscard]] std::uint8_t*
    try_allocate() noexcept
    {
        spin_lock(lock_);

        auto ret = head_;

        if (ret)
        {
            head_ = ret->next;
            ++outstanding_;
        }

        spin_unlock(lock_);

        return reinterpret_cast<std::uint8_t*>(ret);
    }

    /** Attempt to return a pointer to this block.

        @param ptr Pointer to memory block.
        @param slabSize Size of the slab's data buffer.
        @param releaseBuffer Callback invoked with a pointer to the block's
                             buffer so that it can be released.
        @return true if ptr belonged to this block and was freed,
                false otherwise.
     */
    template <typename ReleaseFunc>
    [[nodiscard]] bool
    try_deallocate(
        std::uint8_t* ptr,
        std::size_t slabSize,
        ReleaseFunc&& releaseBuffer) noexcept
    {
        auto data = data_.load(std::memory_order::acquire);

        if (!data || ptr < data || ptr >= data + slabSize)
            return false;

        std::uint8_t* buf = nullptr;

        spin_lock(lock_);

        assert(ptr != nullptr);
        assert(outstanding_ > 0);

        head_ = std::construct_at(reinterpret_cast<item_t*>(ptr), head_);

        // If this block became empty and it is not the first block, we
        // release its buffer. The first block (at the tail of the list
        // with next_ == nullptr) is exempt to keep one block ready.
        if (--outstanding_ == 0 && next_ != nullptr)
        {
            head_ = nullptr;
            buf = data_.exchange(buf, std::memory_order::relaxed);
            cycles_ += (cycles_ < std::numeric_limits<std::uint16_t>::max());
        }

        spin_unlock(lock_);

        if (buf != nullptr)
            releaseBuffer(buf);

        return true;
    }
};

//------------------------------------------------------------------------------

/** Global pool of slab_t objects shared by all slab allocators.

    Provides a fixed-size array of pre-allocated slabs with fallback
    to heap allocation if exhausted.
 */
class slab_pool_t
{
    std::array<slab_t, 32768> blocks_{};
    std::atomic<std::size_t> count_{0};

public:
    constexpr slab_pool_t() = default;

    slab_pool_t(slab_pool_t const&) = delete;
    slab_pool_t&
    operator=(slab_pool_t const&) = delete;
    slab_pool_t(slab_pool_t&&) = delete;
    slab_pool_t&
    operator=(slab_pool_t&&) = delete;

    /** Acquire a slab from the pool.

        @param next Pointer to link as the slab's next pointer.
        @return Pointer to acquired block, or nullptr if allocation failed.
     */
    [[nodiscard]] slab_t*
    acquire(slab_t* next) noexcept
    {
        auto idx = count_.load(std::memory_order::relaxed);

        while (idx < blocks_.size())
        {
            if (count_.compare_exchange_weak(
                    idx, idx + 1, std::memory_order::relaxed))
            {
                return std::construct_at(&blocks_[idx], next);
            }
        }

        // Pool exhausted (unlikely!) so fall back to heap
        return new (std::nothrow) slab_t(next);
    }
};

/// Single global block pool for all slab allocators.
inline constinit slab_pool_t globalSlabPool;

}  // namespace detail

//------------------------------------------------------------------------------

/** A slab allocator for fixed-size memory blocks.

    Allocates memory in large slabs (multiples of 2MB) and sub-allocates
    fixed-size blocks from them. Provides fast allocation with minimal
    fragmentation for objects of uniform size.

    @tparam Type The type used to determine minimum block size and alignment.
    @tparam Align Alignment for allocated blocks (must be >= alignof(Type)).
 */
template <typename Type, std::size_t Align = alignof(Type)>
    requires(
        sizeof(Type) >= sizeof(void*) && Align >= alignof(Type) &&
        std::has_single_bit(Align))
class sized_allocator_t
{
    /// Linked list of slabs for this allocator.
    std::atomic<detail::slab_t*> slabs_{nullptr};

    /// Cached buffer to avoid thrashing when oscillating around block
    /// boundaries.
    std::atomic<std::uint8_t*> cachedBuffer_{nullptr};

    /// Spinlock to serialize the slow allocation path.
    std::atomic<std::uint8_t> blockLock_{0};

    /// Item size (sizeof(Type) + extra, rounded up for alignment).
    std::size_t itemSize_;

    /// Size of each slab's data buffer (multiple of 2 MB).
    std::size_t slabSize_;

    /** Allocate a memory block from the existing slabs.

        Fast path allocation that doesn't acquire the global lock.

        @return Pointer to allocated memory, or nullptr if all slabs are full.
     */
    [[nodiscard]] std::uint8_t*
    fast_allocate() noexcept
    {
        for (auto slab = slabs_.load(std::memory_order::acquire); slab;
             slab = slab->next_)
        {
            if (auto ret = slab->try_allocate())
                return ret;
        }

        return nullptr;
    }

    /** Allocate a memory block, potentially creating new capacity.

        Slow path allocation that may allocate a new buffer or slab.
        Must be called with blockLock_ held.

        @return Pointer to allocated memory, or nullptr on failure.
     */
    [[nodiscard]] std::uint8_t*
    slow_allocate() noexcept
    {
        if (auto ret = fast_allocate())
            return ret;

        // Use a cached buffer first, if we have one, or allocate memory
        // from the operating system.
        auto buf = cachedBuffer_.exchange(nullptr, std::memory_order::acquire);

        if (!buf)
            buf = detail::allocateBuffer(slabSize_);

        if (!buf) [[unlikely]]
            return nullptr;

        // Try to give buffer to an existing empty slab
        for (auto slab = slabs_.load(std::memory_order::acquire); slab;
             slab = slab->next_)
        {
            if (auto ret = slab->try_assign(buf, slabSize_, itemSize_))
                return ret;
        }

        // Create new slab from global pool
        auto* slab = detail::globalSlabPool.acquire(
            slabs_.load(std::memory_order::relaxed));

        if (!slab) [[unlikely]]
        {
            detail::deallocateBuffer(buf);
            return nullptr;
        }

        auto ret = slab->try_assign(buf, slabSize_, itemSize_);
        assert(ret);

        // Link the new slab. Since we hold the lock, we can just do a
        // direct assignment here.
        slabs_.store(slab, std::memory_order::release);

        return ret;
    }

public:
    /** Construct a slab allocator.

        @param extra Extra bytes per item beyond sizeof(Type).
        @param minItems Minimum number of items per slab (rounded up to 2MB).
     */
    constexpr explicit sized_allocator_t(
        std::size_t extra,
        std::size_t minItems)
        : itemSize_(boost::alignment::align_up(sizeof(Type) + extra, Align))
        , slabSize_(
              boost::alignment::align_up(
                  itemSize_ * minItems,
                  detail::pageSize))
    {
    }

    // Data buffers are intentionally not released. C++ destruction order
    // does not guarantee that all items allocated from this allocator
    // have been freed by the time this destructor runs.
    ~sized_allocator_t() = default;

    sized_allocator_t(sized_allocator_t const&) = delete;
    sized_allocator_t&
    operator=(sized_allocator_t const&) = delete;
    sized_allocator_t(sized_allocator_t&&) = delete;
    sized_allocator_t&
    operator=(sized_allocator_t&&) = delete;

    /** Returns the size of memory blocks returned by this allocator.

        @return Size of each allocated block in bytes.
     */
    [[nodiscard]] constexpr std::size_t
    size() const noexcept
    {
        return itemSize_;
    }

    /** Allocate a memory block.

        @return Pointer to allocated memory, or nullptr on failure.

        @note The gnu::malloc attribute is an optimization hint that can
              be leveraged by GCC and Clang.
     */
    [[nodiscard, gnu::malloc]] std::uint8_t*
    allocate() noexcept
    {
        // Fast path: try existing slabs
        auto ret = fast_allocate();

        if (ret == nullptr)
        {
            // Slow path: need new capacity. Grab the lock to serialize access
            // to this code path because it is expensive. Then re-check in case
            // another thread added capacity while we waited.
            spin_lock(blockLock_);
            ret = slow_allocate();
            spin_unlock(blockLock_);
        }

        return ret;
    }

    /** Return a memory block to the allocator.

        @param ptr Pointer to memory block.
        @return true if the block belonged to this allocator and was freed.
     */
    [[nodiscard]] bool
    deallocate(std::uint8_t* ptr) noexcept
    {
        XRPL_ASSERT(
            ptr,
            "ripple::SlabAllocator::SlabAllocator::deallocate : non-null "
            "input");

        auto release = [this](std::uint8_t* buf) {
            std::uint8_t* expected = nullptr;

            if (buf != nullptr &&
                !cachedBuffer_.compare_exchange_strong(
                    expected, buf, std::memory_order::release))
                detail::deallocateBuffer(buf);
        };

        for (auto slab = slabs_.load(std::memory_order::acquire); slab;
             slab = slab->next_)
        {
            if (slab->try_deallocate(ptr, slabSize_, release))
                return true;
        }

        return false;
    }
};

//------------------------------------------------------------------------------

/** Configuration for a single slab allocator.

    @tparam MinItems Minimum number of items per slab (must be > 0).
    @tparam Extra Extra bytes per item beyond sizeof(Type).
 */
template <std::size_t MinItems, std::size_t Extra = 0>
    requires(MinItems > 0)
struct config
{
    static constexpr std::size_t minItems = MinItems;
    static constexpr std::size_t extra = Extra;
};

/** Concept for valid slab configuration types. */
template <typename T>
concept SlabConfig = requires {
    { T::minItems } -> std::convertible_to<std::size_t>;
    { T::extra } -> std::convertible_to<std::size_t>;
    requires T::minItems > 0;
};

/** Validate slab configurations at compile time.

    Checks that configurations produce strictly increasing sizes after
    alignment. This catches both unsorted configs and configs that
    collapse to the same size after alignment.

    @tparam Type The type used to determine base size.
    @tparam Align Alignment for allocated blocks.
    @tparam Configs Configuration types to validate.
    @return true if configurations are valid, false otherwise.
 */
template <typename Type, std::size_t Align, SlabConfig... Configs>
consteval bool
validate_slab_config()
{
    constexpr auto align_up = [](std::size_t n, std::size_t a) {
        return (n + a - 1) & ~(a - 1);
    };

    constexpr std::array<std::size_t, sizeof...(Configs)> sizes{
        align_up(sizeof(Type) + Configs::extra, Align)...};

    for (std::size_t i = 1; i < sizes.size(); ++i)
        if (sizes[i - 1] >= sizes[i])
            return false;

    return true;
}

//------------------------------------------------------------------------------

/** A collection of slab allocators for different sizes.

    Manages multiple sized_allocator_t instances, each configured for a
    different block size. Allocations are routed to the smallest allocator
    that can satisfy the request.

    @tparam Type The type used to determine minimum block size and alignment.
    @tparam Align Alignment for allocated blocks (must be >= alignof(Type)).
    @tparam Configs Configuration types specifying each size class.
 */
template <typename Type, std::size_t Align, SlabConfig... Configs>
    requires(
        sizeof(Type) >= sizeof(void*) && Align >= alignof(Type) &&
        std::has_single_bit(Align) && sizeof...(Configs) > 0 &&
        validate_slab_config<Type, Align, Configs...>())
class aligned_allocator_t
{
    std::array<sized_allocator_t<Type, Align>, sizeof...(Configs)> allocators_;

public:
    /** Construct an allocator set. */
    constexpr aligned_allocator_t()
        : allocators_{sized_allocator_t<Type, Align>(
              Configs::extra,
              Configs::minItems)...}
    {
    }

    ~aligned_allocator_t() = default;

    aligned_allocator_t(aligned_allocator_t const&) = delete;
    aligned_allocator_t&
    operator=(aligned_allocator_t const&) = delete;
    aligned_allocator_t(aligned_allocator_t&&) = delete;
    aligned_allocator_t&
    operator=(aligned_allocator_t&&) = delete;

    /** Allocate memory for an object with extra bytes.

        @param extra Extra bytes needed beyond sizeof(Type).
        @return Pointer to memory, or nullptr if no suitable allocator
                or allocation failed.

        @note The gnu::malloc attribute is an optimization hint that can
              be leveraged by GCC and Clang.
     */
    [[nodiscard, gnu::malloc]] std::uint8_t*
    allocate(std::size_t extra) noexcept
    {
        auto const size = sizeof(Type) + extra;

        for (auto& a : allocators_)
        {
            if (a.size() >= size)
                return a.allocate();
        }

        return nullptr;
    }

    /** Return memory to the allocator set.

        @param ptr Pointer to memory block.
        @return true if memory belonged to this set and was freed.
     */
    [[nodiscard]] bool
    deallocate(std::uint8_t* ptr) noexcept
    {
        for (auto& a : allocators_)
        {
            if (a.deallocate(ptr))
                return true;
        }
        return false;
    }
};

/** Alias for aligned_allocator_t with default alignment. */
template <typename Type, SlabConfig... Configs>
using allocator_t = aligned_allocator_t<Type, alignof(Type), Configs...>;

}  // namespace slab

}  // namespace ripple

#endif  // RIPPLE_BASICS_SLABALLOCATOR_H_INCLUDED
