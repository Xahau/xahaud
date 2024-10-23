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

#include <ripple/protocol/digest.h>
#include <immintrin.h>
#include <openssl/ripemd.h>
#include <openssl/sha.h>

namespace ripple {
namespace detail {

#if defined(__x86_64__)
#if defined(__clang__)
#pragma clang attribute push( \
    __attribute__((target("xsave,avx512f,avx512bw"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("xsave,avx512f,avx512bw")
#endif

static bool
check_avx512()
{
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
    {
        if ((ecx & bit_AVX) && (ecx & bit_OSXSAVE))
        {
            unsigned long long xcr0 = _xgetbv(0);
            if ((xcr0 & 6) == 6)
            {
                if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
                    return (ebx & bit_AVX512F) && (ebx & bit_AVX512BW);
            }
        }
    }
    return false;
}

#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
#endif

static bool
has_avx512()
{
    static const bool support = [] {
#if defined(__x86_64__)
        return check_avx512();
#else
        return false;
#endif
    }();
    return support;
}

#if defined(__x86_64__)
#if defined(__clang__)
#pragma clang attribute push( \
    __attribute__((target("avx512f,avx512bw"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("avx512f,avx512bw")
#endif

static void
process_sha256_blocks_avx512(
    SHA256_CTX* ctx,
    const uint8_t* data,
    size_t blocks)
{
    for (size_t i = 0; i < blocks; ++i)
    {
        __m512i block = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(data + (i * 64)));

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        const __m512i swap = _mm512_setr_epi64(
            0x0001020304050607,
            0x08090a0b0c0d0e0f,
            0x1011121314151617,
            0x18191a1b1c1d1e1f,
            0x2021222324252627,
            0x28292a2b2c2d2e2f,
            0x3031323334353637,
            0x38393a3b3c3d3e3f);
        block = _mm512_shuffle_epi8(block, swap);
#endif

        SHA256_Update(ctx, data + (i * 64), 64);
    }
}

static void
process_sha512_blocks_avx512(
    SHA512_CTX* ctx,
    const uint8_t* data,
    size_t blocks)
{
    for (size_t i = 0; i < blocks; ++i)
    {
        __m512i block = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(data + (i * 64)));

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        const __m512i swap = _mm512_setr_epi64(
            0x0001020304050607,
            0x08090a0b0c0d0e0f,
            0x1011121314151617,
            0x18191a1b1c1d1e1f,
            0x2021222324252627,
            0x28292a2b2c2d2e2f,
            0x3031323334353637,
            0x38393a3b3c3d3e3f);
        block = _mm512_shuffle_epi8(block, swap);
#endif

        SHA512_Update(ctx, data + (i * 64), 64);
    }
}

static void
process_ripemd160_blocks_avx512(
    RIPEMD160_CTX* ctx,
    const uint8_t* data,
    size_t blocks)
{
    for (size_t i = 0; i < blocks; ++i)
    {
        __m512i block = _mm512_loadu_si512(
            reinterpret_cast<const __m512i*>(data + (i * 64)));

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        const __m512i swap = _mm512_setr_epi64(
            0x0001020304050607,
            0x08090a0b0c0d0e0f,
            0x1011121314151617,
            0x18191a1b1c1d1e1f,
            0x2021222324252627,
            0x28292a2b2c2d2e2f,
            0x3031323334353637,
            0x38393a3b3c3d3e3f);
        block = _mm512_shuffle_epi8(block, swap);
#endif

        RIPEMD160_Update(ctx, data + (i * 64), 64);
    }
}

#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
#endif

}  // namespace detail

// RIPEMD160 implementation
openssl_ripemd160_hasher::openssl_ripemd160_hasher()
{
    static_assert(sizeof(ctx_) >= sizeof(RIPEMD160_CTX), "");
    auto ctx = reinterpret_cast<RIPEMD160_CTX*>(ctx_);
    RIPEMD160_Init(ctx);
}

void
openssl_ripemd160_hasher::operator()(
    void const* data,
    std::size_t size) noexcept
{
    auto ctx = reinterpret_cast<RIPEMD160_CTX*>(ctx_);

#if defined(__x86_64__)
    if (detail::has_avx512() && size >= 64)
    {
        size_t blocks = size / 64;
        detail::process_ripemd160_blocks_avx512(
            ctx, static_cast<const uint8_t*>(data), blocks);
        size_t remaining = size % 64;
        if (remaining)
            RIPEMD160_Update(
                ctx,
                static_cast<const uint8_t*>(data) + (blocks * 64),
                remaining);
        return;
    }
#endif

    RIPEMD160_Update(ctx, data, size);
}

openssl_ripemd160_hasher::operator result_type() noexcept
{
    auto ctx = reinterpret_cast<RIPEMD160_CTX*>(ctx_);
    result_type digest;
    RIPEMD160_Final(digest.data(), ctx);
    return digest;
}

// SHA256 implementation
openssl_sha256_hasher::openssl_sha256_hasher()
{
    static_assert(sizeof(ctx_) >= sizeof(SHA256_CTX), "");
    auto ctx = reinterpret_cast<SHA256_CTX*>(ctx_);
    SHA256_Init(ctx);
}

void
openssl_sha256_hasher::operator()(void const* data, std::size_t size) noexcept
{
    auto ctx = reinterpret_cast<SHA256_CTX*>(ctx_);

#if defined(__x86_64__)
    if (detail::has_avx512() && size >= 64)
    {
        size_t blocks = size / 64;
        detail::process_sha256_blocks_avx512(
            ctx, static_cast<const uint8_t*>(data), blocks);
        size_t remaining = size % 64;
        if (remaining)
            SHA256_Update(
                ctx,
                static_cast<const uint8_t*>(data) + (blocks * 64),
                remaining);
        return;
    }
#endif

    SHA256_Update(ctx, data, size);
}

openssl_sha256_hasher::operator result_type() noexcept
{
    auto ctx = reinterpret_cast<SHA256_CTX*>(ctx_);
    result_type digest;
    SHA256_Final(digest.data(), ctx);
    return digest;
}

// SHA512 implementation
openssl_sha512_hasher::openssl_sha512_hasher()
{
    static_assert(sizeof(ctx_) >= sizeof(SHA512_CTX), "");
    auto ctx = reinterpret_cast<SHA512_CTX*>(ctx_);
    SHA512_Init(ctx);
}

void
openssl_sha512_hasher::operator()(void const* data, std::size_t size) noexcept
{
    auto ctx = reinterpret_cast<SHA512_CTX*>(ctx_);

#if defined(__x86_64__)
    if (detail::has_avx512() && size >= 64)
    {
        size_t blocks = size / 64;
        detail::process_sha512_blocks_avx512(
            ctx, static_cast<const uint8_t*>(data), blocks);
        size_t remaining = size % 64;
        if (remaining)
            SHA512_Update(
                ctx,
                static_cast<const uint8_t*>(data) + (blocks * 64),
                remaining);
        return;
    }
#endif

    SHA512_Update(ctx, data, size);
}

openssl_sha512_hasher::operator result_type() noexcept
{
    auto ctx = reinterpret_cast<SHA512_CTX*>(ctx_);
    result_type digest;
    SHA512_Final(digest.data(), ctx);
    return digest;
}

}  // namespace ripple
