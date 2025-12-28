//------------------------------------------------------------------------------
/*
    This file is part of Beast: https://github.com/vinniefalco/Beast
    Copyright 2014, Vinnie Falco <vinnie.falco@gmail.com>

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

#ifndef BEAST_RANDOM_RNGFILL_H_INCLUDED
#define BEAST_RANDOM_RNGFILL_H_INCLUDED

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <type_traits>

namespace beast {

template <class Generator>
void
rngfill(std::span<std::byte> buf, Generator& g)
{
    if constexpr (std::is_invocable_r_v<
                      void,
                      Generator,
                      decltype(buf.data()),
                      decltype(buf.size())>)
        return g(buf.data(), buf.size());

    using result_type = decltype(g());
    auto constexpr bs = sizeof(result_type);

    auto fill_impl = [](std::span<std::byte> s, result_type v) {
        std::copy_n(reinterpret_cast<std::byte const*>(&v), s.size(), s.data());
    };

    if (auto misalign =
            reinterpret_cast<std::uintptr_t>(buf.data()) % alignof(result_type))
    {
        auto const prefix =
            std::min(buf.size(), alignof(result_type) - misalign);
        fill_impl(buf.first(prefix), g());
        buf = buf.subspan(prefix);
    }

    auto const count = buf.size() / bs;
    std::generate_n(
        reinterpret_cast<result_type*>(buf.data()), count, std::ref(g));
    buf = buf.subspan(count * bs);

    if (!buf.empty())
        fill_impl(buf, g());
}

template <class T, std::size_t Extent, class Generator>
    requires std::is_integral_v<T>
void
rngfill(std::span<T, Extent> buf, Generator& g)
{
    rngfill(std::as_writable_bytes(buf), g);
}

template <class T, std::size_t N, class Generator>
    requires std::is_integral_v<T> && (N != 0)
void
rngfill(std::array<T, N>& a, Generator& g)
{
    rngfill(std::as_writable_bytes(std::span{a}), g);
}

template <class T, std::size_t N, class Generator>
    requires std::is_integral_v<T> && (N != 0)
void
rngfill(T (&a)[N], Generator& g)
{
    rngfill(std::as_writable_bytes(std::span{a}), g);
}

template <class T, class Generator>
    requires(
        std::is_same_v<T, std::byte> || std::is_same_v<T, char> ||
        std::is_same_v<T, signed char> || std::is_same_v<T, unsigned char> ||
        std::is_same_v<T, std::uint8_t>)
void
rngfill(T* ptr, std::size_t count, Generator& g)
{
    rngfill(std::as_writable_bytes(std::span{ptr, count}), g);
}


}  // namespace beast

#endif
