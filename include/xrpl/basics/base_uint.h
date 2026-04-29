//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2011 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#ifndef RIPPLE_BASICS_BASE_UINT_H_INCLUDED
#define RIPPLE_BASICS_BASE_UINT_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/hardened_hash.h>
#include <xrpl/basics/partitioned_unordered_map.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/beast/utility/instrumentation.h>

#include <boost/endian/conversion.hpp>
#include <boost/functional/hash.hpp>
#include <boost/predef.h>
#include <algorithm>

#include <array>
#include <bit>
#include <compare>
#include <cstring>
#include <functional>
#include <optional>
#include <rocksdb/thread_status.h>
#include <type_traits>

#if BOOST_COMP_MSVC
#include <intrin.h>
#endif

namespace ripple {

template <std::size_t Bits>
using base_uint_limb_t =
    std::conditional_t<(Bits % 64 == 0), std::uint64_t, std::uint32_t>;

template <std::size_t Bits>
inline constexpr std::size_t base_uint_limb_count =
    Bits / (sizeof(base_uint_limb_t<Bits>) * 8);

namespace detail {

/** A contiguous source of trivially copyable elements.

    This models a container-like type (i.e. something that provides `size()`
    and `data()` and whose `value_type` is trivially copyable.
*/
template <class Container>
concept byte_copy_source = requires(Container const& c) {
    c.size();
    c.data();
    typename Container::value_type;
} && std::is_trivially_copyable_v<typename Container::value_type>;

/** Table mapping hex digits to their integer value. Other digits map to -1. */
inline constexpr auto hex_to_nibble = []() consteval {
    std::array<int, 256> table;
    table.fill(-1);

    for (int i = 0; i <= 9; ++i)
        table[static_cast<unsigned char>('0' + i)] = i;

    for (int i = 0; i < 6; ++i)
    {
        table[static_cast<unsigned char>('a' + i)] = 10 + i;
        table[static_cast<unsigned char>('A' + i)] = 10 + i;
    }

    return table;
}();

/** Parse a hexadecimal string into a limb array.

    The input is consumed left-to-right (MSB first).

    @param sv  The string to parse; it must be exactly `out.size() * 2`
               characters long, or the special value "0".
    @param out The buffer to write into. Must be non-empty.
    @return    `true` if the input was parsed successfully; `false` if the
               length was wrong or a non-hex character was encountered.
 */
template <typename T>
    requires(
        std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::uint64_t>)
constexpr bool
parseHex(std::string_view sv, std::span<T> out) noexcept
{
    constexpr std::size_t hex_per_limb = sizeof(T) * 2;

    if (sv == "0")
    {
        std::fill(out.begin(), out.end(), T{0});
        return true;
    }

    if (sv.size() != out.size() * hex_per_limb)
        return false;

    auto in = sv.begin();

    for (auto& limb : out)
    {
        limb = 0;

        for (std::size_t h = 0; h < hex_per_limb; ++h)
        {
            auto const nibble =
                hex_to_nibble[static_cast<unsigned char>(*in++)];

            if (nibble == -1)
                return false;

            limb = (limb << 4) | static_cast<T>(nibble);
        }

        limb = boost::endian::native_to_big(limb);
    }

    return true;
}

[[nodiscard]] inline std::uint64_t
add_carry(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t carry_in,
    std::uint64_t& carry_out) noexcept
{
#if BOOST_COMP_GNUC || BOOST_COMP_CLANG
    unsigned long long co;
    auto const result = __builtin_addcll(a, b, carry_in, &co);
    carry_out = co;
    return result;
#elif BOOST_COMP_MSVC
    std::uint64_t result;
    carry_out =
        _addcarry_u64(static_cast<unsigned char>(carry_in), a, b, &result);
    return result;
#else
    // Portable fallback — manual carry detection.
    std::uint64_t sum = a + carry_in;
    std::uint64_t c = sum < a;
    std::uint64_t result = sum + b;
    carry_out = c + (result < sum);
    return result;
#endif
}

[[nodiscard]] inline std::uint32_t
add_carry(
    std::uint32_t a,
    std::uint32_t b,
    std::uint32_t carry_in,
    std::uint32_t& carry_out) noexcept
{
#if BOOST_COMP_GNUC || BOOST_COMP_CLANG
    unsigned co;
    auto const result = __builtin_addc(a, b, carry_in, &co);
    carry_out = co;
    return result;
#elif BOOST_COMP_MSVC
    std::uint32_t result;
    carry_out =
        _addcarry_u32(static_cast<unsigned char>(carry_in), a, b, &result);
    return result;
#else
    // Portable fallback — widen to 64 bits.
    std::uint64_t sum = static_cast<std::uint64_t>(a) + b + carry_in;
    carry_out = static_cast<std::uint32_t>(sum >> 32);
    return static_cast<std::uint32_t>(sum);
#endif
}

template <typename T, std::size_t N>
void
add(std::array<T, N>& lhs, std::array<T, N> const& rhs) noexcept
{
    T carry{};
    for (std::size_t i = N; i-- > 0;)
    {
        T carry_out;
        T const sum = add_carry(
            boost::endian::big_to_native(lhs[i]),
            boost::endian::big_to_native(rhs[i]),
            carry,
            carry_out);
        lhs[i] = boost::endian::native_to_big(sum);
        carry = carry_out;
    }
}

template <typename T, std::size_t N>
void
increment(std::array<T, N>& data) noexcept
{
    for (std::size_t i = N; i-- > 0;)
    {
        T native = boost::endian::big_to_native(data[i]);
        data[i] = boost::endian::native_to_big(++native);
        if (native != 0)
            return;
    }
}

template <typename T, std::size_t N>
void
decrement(std::array<T, N>& data) noexcept
{
    for (std::size_t i = N; i-- > 0;)
    {
        T const native = boost::endian::big_to_native(data[i]);
        data[i] = boost::endian::native_to_big(native - 1);
        if (native != 0)
            return;
    }
}

template <std::size_t N>
constexpr void
assign(std::array<std::uint64_t, N>& data, std::uint64_t v) noexcept
{
    data.fill(0);
    data[N - 1] = boost::endian::native_to_big(v);
}

template <std::size_t N>
constexpr void
assign(std::array<std::uint32_t, N>& data, std::uint64_t v) noexcept
{
    static_assert(N >= 2);
    data.fill(0);
    data[N - 1] = boost::endian::native_to_big(static_cast<std::uint32_t>(v));
    data[N - 2] =
        boost::endian::native_to_big(static_cast<std::uint32_t>(v >> 32));
}

}  // namespace detail

/** Arbitrarily long unsigned integers.

    @note This class stores its values internally in big-endian form and
          that internal representation is part of the binary protocol of
          the XRP Ledger. Changing it arbitrarily will cause breakage in
          the unplumbed depths of the low level ledger internals.

          @tparam Bits The width of this integer in bits, with a minimum
                       of 64. It must be a multiple of 32.
          @tparam Tag An arbitrary type that is used as a tag and allows
                      the instantiation of multiple "distinct" types.
 */
template <std::size_t Bits, class Tag = void>
class base_uint
{
    static_assert(
        (Bits % 32) == 0,
        "The length of a base_uint in bits must be a multiple of 32.");

    static_assert(
        Bits >= 64,
        "The length of a base_uint in bits must be at least 64.");

    using limb_type = base_uint_limb_t<Bits>;

    // Internal storage: big-endian limbs.  data_[0] is the most
    // significant limb.  Within each limb, bytes are stored in
    // big-endian order (via native_to_big at write time).
    std::array<limb_type, base_uint_limb_count<Bits>> data_;

public:
    //--------------------------------------------------------------------------
    //
    // STL Container Interface
    //

    static std::size_t constexpr bytes = Bits / 8;
    static_assert(sizeof(data_) == bytes);

    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using value_type = unsigned char;
    using pointer = value_type*;
    using reference = value_type&;
    using const_pointer = value_type const*;
    using const_reference = value_type const&;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    using tag_type = Tag;

    [[nodiscard]] pointer
    data() noexcept
    {
        return reinterpret_cast<pointer>(data_.data());
    }

    [[nodiscard]] const_pointer
    data() const noexcept
    {
        return reinterpret_cast<const_pointer>(data_.data());
    }

    [[nodiscard]] char const*
    cdata() const noexcept
    {
        return reinterpret_cast<char const*>(data_.data());
    }

    [[nodiscard]] char*
    cdata() noexcept
    {
        return reinterpret_cast<char*>(data_.data());
    }

    [[nodiscard]] iterator
    begin() noexcept
    {
        return data();
    }

    [[nodiscard]] iterator
    end() noexcept
    {
        return data() + bytes;
    }

    [[nodiscard]] const_iterator
    begin() const noexcept
    {
        return data();
    }

    [[nodiscard]] const_iterator
    end() const noexcept
    {
        return data() + bytes;
    }

    [[nodiscard]] const_iterator
    cbegin() const noexcept
    {
        return data();
    }

    [[nodiscard]] const_iterator
    cend() const noexcept
    {
        return data() + bytes;
    }

    /** Value hashing function.
        The seed prevents crafted inputs from causing degenerate parent
       containers.
    */
    using hasher = hardened_hash<>;

    /** Returns a read-only view of the object's byte representation.

        The returned span references the underlying storage and remains
        valid for the lifetime of the object.
     */
    [[nodiscard]] constexpr std::span<std::byte const, bytes>
    span() const noexcept
    {
        return {reinterpret_cast<std::byte const*>(data_.data()), bytes};
    }

private:
    /** Construct from a raw pointer.
        The buffer pointed to by `data` must be at least Bits/8 bytes.

        @note the structure is used to disambiguate this from the std::uint64_t
              constructor: something like base_uint(0) is ambiguous.
    */
    struct VoidHelper
    {
        explicit VoidHelper() = default;
    };

    explicit base_uint(void const* data, VoidHelper) noexcept
    {
        std::memcpy(data_.data(), data, bytes);
    }

public:
    constexpr base_uint() noexcept : data_{}
    {
    }

    constexpr base_uint(beast::Zero) noexcept : data_{}
    {
    }

    constexpr base_uint(base_uint const& b) noexcept = default;
    constexpr base_uint&
    operator=(base_uint const& b) noexcept = default;

    constexpr explicit base_uint(std::uint64_t b) noexcept
    {
        detail::assign(data_, b);
    }

    static constexpr base_uint
    from_hex(std::string_view s) noexcept(false)
    {
        base_uint ret;

        if (!ret.parseHex(s))
            Throw<std::invalid_argument>("invalid hex string");

        return ret;
    }

    explicit constexpr base_uint(std::string_view sv) noexcept(false)
    {
        if (!parseHex(sv)) [[unlikely]]
            Throw<std::invalid_argument>("invalid hex string");
    }

    template <detail::byte_copy_source Container>
    constexpr base_uint&
    operator=(Container const& c)
    {
        static_assert(sizeof(typename Container::value_type) == 1);

        if (std::is_constant_evaluated())
        {
            using CVT = Container::value_type;

            if (!std::is_same_v<CVT, unsigned char> &&
                !std::is_same_v<CVT, std::uint8_t> &&
                !std::is_same_v<CVT, std::byte>)
                throw "ripple::base_uint::operator=(Container): invalid data in constexpr assignment";

            if (c.size() != bytes)
                throw "ripple::base_uint::operator=(Container): invalid data in constexpr assignment";

            std::array<std::uint8_t, bytes> tmp;
            std::copy(c.begin(), c.end(), tmp.begin());
            data_ = std::bit_cast<decltype(data_)>(tmp);
        }
        else
        {
            XRPL_ASSERT(
                c.size() * sizeof(typename Container::value_type) == size(),
                "ripple::base_uint::operator=(Container auto) : input size "
                "match");

            // While unlikely, it is not impossible that a programmer error
            // can cause overlap between the source and destination buffers
            // which invokes UB with memcpy. We could detect the overlap at
            // runtime, but then what? Using memmove avoids the UB entirely
            // and the overhead is roughly equivalent to copying four extra
            // bytes.
            std::memmove(data_.data(), c.data(), size());
        }

        return *this;
    }

    template <detail::byte_copy_source Container>
    constexpr explicit base_uint(Container const& c) noexcept
    {
        *this = c;
    }

    /* Construct from a raw pointer.
        The buffer pointed to by `data` must be at least Bits/8 bytes.
    */
    [[nodiscard]] static base_uint
    fromVoid(void const* data) noexcept
    {
        return base_uint(data, VoidHelper());
    }

    template <class T>
    [[nodiscard]] static std::optional<base_uint>
    fromVoidChecked(T const& from) noexcept
    {
        if (from.size() != size())
            return {};
        return fromVoid(from.data());
    }

    constexpr int
    signum() const noexcept
    {
        return std::any_of(
            data_.begin(), data_.end(), [](auto v) { return v != 0; });
    }

    constexpr bool
    operator!() const noexcept
    {
        return signum() == 0;
    }

    constexpr base_uint
    operator~() const noexcept
    {
        base_uint ret;
        for (std::size_t i = 0; i < data_.size(); ++i)
            ret.data_[i] = ~data_[i];
        return ret;
    }

    constexpr base_uint&
    operator^=(base_uint const& b) noexcept
    {
        for (std::size_t i = 0; i < data_.size(); ++i)
            data_[i] ^= b.data_[i];
        return *this;
    }

    constexpr base_uint&
    operator&=(base_uint const& b) noexcept
    {
        for (std::size_t i = 0; i < data_.size(); ++i)
            data_[i] &= b.data_[i];
        return *this;
    }

    constexpr base_uint&
    operator|=(base_uint const& b) noexcept
    {
        for (std::size_t i = 0; i < data_.size(); ++i)
            data_[i] |= b.data_[i];
        return *this;
    }

    base_uint&
    operator=(std::uint64_t v) noexcept
    {
        detail::assign(data_, v);
        return *this;
    }

    constexpr base_uint&
    operator=(beast::Zero) noexcept
    {
        data_.fill(0);
        return *this;
    }

    base_uint&
    operator+=(base_uint const& b) noexcept
    {
        detail::add(data_, b.data_);
        return *this;
    }

    base_uint&
    operator++() noexcept
    {
        detail::increment(data_);
        return *this;
    }

    base_uint
    operator++(int) noexcept
    {
        base_uint ret = *this;
        ++(*this);
        return ret;
    }

    base_uint&
    operator--() noexcept
    {
        detail::decrement(data_);
        return *this;
    }

    base_uint
    operator--(int) noexcept
    {
        base_uint ret = *this;
        --(*this);
        return ret;
    }

    base_uint
    next() const noexcept
    {
        auto ret = *this;
        return ++ret;
    }

    base_uint
    prev() const noexcept
    {
        auto ret = *this;
        return --ret;
    }

    /** Parse a hex string into a base_uint

        The input must be precisely `2 * bytes` hexadecimal characters
        long, with one exception: the value '0'.

        @param sv The hexadecimal characters composing the string
        @return true if the input was parsed properly; false otherwise.
     */
    [[nodiscard]] constexpr bool
    parseHex(std::string_view sv) noexcept
    {
        return detail::parseHex(sv, std::span<limb_type>{data_});
    }

    constexpr static std::size_t
    size() noexcept
    {
        return bytes;
    }

    template <class Hasher>
    friend void
    hash_append(Hasher& h, base_uint const& a) noexcept
    {
        // Do not allow any endian transformations on this memory.
        h(a.data_.data(), sizeof(a.data_));
    }

    // Deprecated.
    bool
    isZero() const
    {
        return *this == beast::zero;
    }

    bool
    isNonZero() const
    {
        return *this != beast::zero;
    }

    void
    zero()
    {
        *this = beast::zero;
    }

    [[nodiscard]] constexpr bool
    operator==(base_uint const& other) const noexcept
    {
        return data_ == other.data_;
    }
};

/** Convenient type aliases for common sizes */
using uint128 = base_uint<128>;
using uint160 = base_uint<160>;
using uint192 = base_uint<192>;
using uint256 = base_uint<256>;
using uint512 = base_uint<512>;

/** Lexicographic comparison of two base_uint values.

    @note The comparison operates on the raw byte representation. Since
          the internal representation is an array of integers, that are
          each stored as big endian, meaning the first byte is the most
          significant, makes a left-to-right lexicographical comparison
          correct.
*/
/** @{ */
template <std::size_t Bits, class Tag1, class Tag2>
constexpr std::strong_ordering
tagless_compare(
    base_uint<Bits, Tag1> const& lhs,
    base_uint<Bits, Tag2> const& rhs) noexcept
{
#if __cpp_lib_three_way_comparison >= 201907L
    return std::lexicographical_compare_three_way(
        lhs.cbegin(),
        lhs.cend(),
        rhs.cbegin(),
        rhs.cend(),
        std::compare_three_way{});
#else
    auto const [a, b] =
        std::mismatch(lhs.cbegin(), lhs.cend(), rhs.cbegin(), rhs.cend());

    if (a == lhs.cend())
        return std::strong_ordering::equivalent;

    return (*a > *b) ? std::strong_ordering::greater
                     : std::strong_ordering::less;
#endif
}

template <std::size_t Bits, class Tag>
constexpr std::strong_ordering
compare(
    base_uint<Bits, Tag> const& lhs,
    base_uint<Bits, Tag> const& rhs) noexcept
{
    return tagless_compare(lhs, rhs);
}

template <std::size_t Bits, class Tag>
[[nodiscard]] constexpr std::strong_ordering
operator<=>(
    base_uint<Bits, Tag> const& lhs,
    base_uint<Bits, Tag> const& rhs) noexcept
{
    return compare(lhs, rhs);
}
/** @} */

/** Equality comparison */
template <std::size_t Bits, class Tag>
constexpr bool
operator==(base_uint<Bits, Tag> const& a, std::uint64_t b) noexcept
{
    return a == base_uint<Bits, Tag>(b);
}

template <std::size_t Bits, class Tag>
constexpr base_uint<Bits, Tag>
operator^(base_uint<Bits, Tag> const& a, base_uint<Bits, Tag> const& b) noexcept
{
    return base_uint<Bits, Tag>(a) ^= b;
}

template <std::size_t Bits, class Tag>
constexpr base_uint<Bits, Tag>
operator&(base_uint<Bits, Tag> const& a, base_uint<Bits, Tag> const& b) noexcept
{
    return base_uint<Bits, Tag>(a) &= b;
}

template <std::size_t Bits, class Tag>
constexpr base_uint<Bits, Tag>
operator|(base_uint<Bits, Tag> const& a, base_uint<Bits, Tag> const& b) noexcept
{
    return base_uint<Bits, Tag>(a) |= b;
}

template <std::size_t Bits, class Tag>
constexpr base_uint<Bits, Tag>
operator+(base_uint<Bits, Tag> const& a, base_uint<Bits, Tag> const& b) noexcept
{
    return base_uint<Bits, Tag>(a) += b;
}

template <std::size_t Bits, class Tag>
std::string
to_string(base_uint<Bits, Tag> const& a)
{
    return strHex(a.cbegin(), a.cend());
}

template <std::size_t Bits, class Tag>
std::ostream&
operator<<(std::ostream& out, base_uint<Bits, Tag> const& u)
{
    return out << to_string(u);
}

// This function is used by the TaggedCache partitioning code, which is
// both poorly thought out and poorly implemented.
template <>
inline std::size_t
extract(uint256 const& key)
{
    std::size_t result;

    // We use memcpy to avoid potential UB in reinterpreting the leading
    // bytes of the buffer as std::size_t, not because of concerns about
    // data alignment. This will likely optimize away to a single load.
    std::memcpy(&result, key.data(), sizeof(std::size_t));
    return result;
}

}  // namespace ripple

template <std::size_t Bits, class Tag>
struct beast::is_uniquely_represented<ripple::base_uint<Bits, Tag>>
    : std::true_type
{
    explicit is_uniquely_represented() = default;
};

#endif
