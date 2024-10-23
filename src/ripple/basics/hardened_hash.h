#ifndef RIPPLE_BASICS_HARDENED_HASH_H_INCLUDED
#define RIPPLE_BASICS_HARDENED_HASH_H_INCLUDED

#include <ripple/beast/hash/hash_append.h>
#include <ripple/beast/hash/xxhasher.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace ripple {

namespace detail {

#if defined(__x86_64__) || defined(_M_X64)
inline bool
check_aesni_support()
{
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
    {
        return (ecx & bit_AES) != 0;
    }
    return false;
}

// Helper function to contain all AES-NI operations
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__target__("aes")))
#endif
inline __m128i
aesni_hash_block(__m128i state, __m128i key, const void* data, size_t len)
{
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    while (len >= 16)
    {
        __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
        state = _mm_xor_si128(state, block);
        state = _mm_aesenc_si128(state, key);
        ptr += 16;
        len -= 16;
    }

    if (len > 0)
    {
        alignas(16) uint8_t last_block[16] = {0};
        std::memcpy(last_block, ptr, len);
        __m128i block =
            _mm_load_si128(reinterpret_cast<const __m128i*>(last_block));
        state = _mm_xor_si128(state, block);
        state = _mm_aesenc_si128(state, key);
    }

    return state;
}

// Helper function for final AES round
#if defined(__GNUC__) || defined(__clang__)
__attribute__((__target__("aes")))
#endif
inline __m128i
aesni_hash_final(__m128i state, __m128i key)
{
    return _mm_aesenclast_si128(state, key);
}
#endif

using seed_pair = std::pair<std::uint64_t, std::uint64_t>;

template <bool = true>
seed_pair
make_seed_pair() noexcept
{
    struct state_t
    {
        std::mutex mutex;
        std::random_device rng;
        std::mt19937_64 gen;
        std::uniform_int_distribution<std::uint64_t> dist;
        state_t() : gen(rng())
        {
        }
    };
    static state_t state;
    std::lock_guard lock(state.mutex);
    return {state.dist(state.gen), state.dist(state.gen)};
}

}  // namespace detail

template <class HashAlgorithm = beast::xxhasher>
class hardened_hash
{
private:
    detail::seed_pair m_seeds;
#if defined(__x86_64__) || defined(_M_X64)
    bool using_aesni_;
#endif

public:
    using result_type = typename HashAlgorithm::result_type;

    hardened_hash()
        : m_seeds(detail::make_seed_pair<>())
#if defined(__x86_64__) || defined(_M_X64)
        , using_aesni_(detail::check_aesni_support())
#endif
    {
    }

    template <class T>
    result_type
    operator()(T const& t) const noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
        if (using_aesni_)
        {
            alignas(16) __m128i key =
                _mm_set_epi64x(m_seeds.first, m_seeds.second);
            alignas(16) __m128i state = _mm_setzero_si128();

            // Hash the data using AES-NI
            const char* data = reinterpret_cast<const char*>(&t);
            state = detail::aesni_hash_block(state, key, data, sizeof(t));
            state = detail::aesni_hash_final(state, key);

            return static_cast<result_type>(_mm_cvtsi128_si64(state));
        }
#endif
        // Original implementation using xxhasher
        HashAlgorithm h(m_seeds.first, m_seeds.second);
        using beast::hash_append;
        hash_append(h, t);
        return static_cast<result_type>(h);
    }
};

}  // namespace ripple

#endif
