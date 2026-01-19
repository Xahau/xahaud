//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#ifndef RIPPLE_BASICS_HARDENED_HASH_H_INCLUDED
#define RIPPLE_BASICS_HARDENED_HASH_H_INCLUDED

#include <ripple/beast/hash/hash_append.h>
#include <ripple/beast/hash/xxhasher.h>
#include <ripple/beast/xor_shift_engine.h>

#include <atomic>
#include <random>

namespace ripple {

namespace detail {

inline std::atomic hardened_hash_seed = []() {
    std::uint64_t seed = 0x726661627563794;

    std::random_device rd;

    for (int i = 0; i < 16; ++i)
        seed ^= (seed << 6) + rd();

    return seed;
}();

}  // namespace detail

/**
 * Seed functor once per construction

   A std compatible hash adapter that resists adversarial inputs.
   For this to work, T must implement in its own namespace:

   @code

   template <class Hasher>
   void
   hash_append (Hasher& h, T const& t) noexcept
   {
       // hash_append each base and member that should
       //  participate in forming the hash
       using beast::hash_append;
       hash_append (h, static_cast<T::base1 const&>(t));
       hash_append (h, static_cast<T::base2 const&>(t));
       // ...
       hash_append (h, t.member1);
       hash_append (h, t.member2);
       // ...
   }

   @endcode

   Do not use any version of Murmur or CityHash for the Hasher
   template parameter (the hashing algorithm).  For details
   see https://131002.net/siphash/#at
*/

template <class HashAlgorithm = beast::xxhasher>
class hardened_hash
{
    std::uint64_t seed_;

public:
    using result_type = HashAlgorithm::result_type;

    hardened_hash() noexcept
        : seed_(
              beast::fmix64(
                  detail::hardened_hash_seed.fetch_add(
                      1,
                      std::memory_order_relaxed)))
    {
    }

    template <class T>
    [[nodiscard]] result_type
    operator()(T const& t) const noexcept
    {
        HashAlgorithm h(seed_);
        hash_append(h, t);
        return static_cast<result_type>(h);
    }
};

}  // namespace ripple

#endif
