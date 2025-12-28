//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/xahau/xahaud
    Copyright (c) 2026, the Xahaud developers.

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

#ifndef XAHAU_PROTOCOL_KEYBASE_H_INCLUDED
#define XAHAU_PROTOCOL_KEYBASE_H_INCLUDED

#include <algorithm>
#include <cstdint>

namespace ripple {
namespace detail {

/** Common base for fixed-size key and seed types.

    This CRTP base provides storage, accessors, and equality comparison
    for types like seeds and secret/public keys, whichwrap a fixed-size
    byte array. The Derived parameter ensures that unrelated types with
    the same size do not implicitly interoperate with one another.

    Equality comparison is provided automatically. Derived classes that
    want to can opt in to ordering by declaring operator<=> as a
    hidden friend.

    @tparam Derived The concrete type inheriting from this base (CRTP).
    @tparam N       The size of the underlying byte array.
 */
template <class Derived, std::size_t N>
class KeyBase
{
public:
    /** The type we use to store the data. */
    using value_t = std::array<std::uint8_t, N>;

    /** The span type corresponding to the type we use to store the data. */
    using span_t = std::span<std::uint8_t, N>;

protected:
    value_t buf_;

    KeyBase() noexcept = default;

    explicit KeyBase(span_t data) noexcept
    {
        std::copy_n(data.data(), N, buf_.data());
    }

public:
    [[nodiscard]] std::uint8_t const*
    data() const
    {
        return buf_.data();
    }

    [[nodiscard]] std::size_t
    size() const
    {
        return buf_.size();
    }

    [[nodiscard]] auto
    begin() const noexcept
    {
        return buf_.begin();
    }

    [[nodiscard]] auto
    cbegin() const noexcept
    {
        return buf_.cbegin();
    }

    [[nodiscard]] auto
    end() const noexcept
    {
        return buf_.end();
    }

    [[nodiscard]] auto
    cend() const noexcept
    {
        return buf_.cend();
    }

    [[nodiscard]] friend bool
    operator==(Derived const& lhs, Derived const& rhs)
    {
        // This is not a constant time comparison. This is probably OK
        // for the xahaud codebase.
        return lhs.buf_ == rhs.buf_;
    }
};

}  // namespace detail
}  // namespace ripple

#endif
