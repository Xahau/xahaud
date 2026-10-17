//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
    Copyright (c) 2026 XRPL Labs

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

#include <xrpl/protocol/PQPublicKey.h>
#include <xrpl/basics/hardened_hash.h>
#include <xrpl/protocol/digest.h>

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

// pqov shim symbols (external/pqov/pqov_shim.c), one set per SIMD variant.
extern "C" {
int
pqov_portable_verify_cpk(
    const std::uint8_t* msg,
    std::size_t mlen,
    const std::uint8_t* sig,
    const std::uint8_t* cpk);
int
pqov_portable_expand_public_key(std::uint8_t* pk, const std::uint8_t* cpk);
int
pqov_portable_verify_pk(
    const std::uint8_t* msg,
    std::size_t mlen,
    const std::uint8_t* sig,
    const std::uint8_t* pk);
int
pqov_portable_keypair_from_seed(std::uint8_t* cpk, const std::uint8_t* seed);
int
pqov_portable_sign_from_seed(
    std::uint8_t* sig,
    const std::uint8_t* seed,
    const std::uint8_t* msg,
    std::size_t mlen);
#if PQOV_HAVE_AVX2
int
pqov_avx2_verify_cpk(
    const std::uint8_t* msg,
    std::size_t mlen,
    const std::uint8_t* sig,
    const std::uint8_t* cpk);
int
pqov_avx2_expand_public_key(std::uint8_t* pk, const std::uint8_t* cpk);
int
pqov_avx2_verify_pk(
    const std::uint8_t* msg,
    std::size_t mlen,
    const std::uint8_t* sig,
    const std::uint8_t* pk);
int
pqov_avx2_keypair_from_seed(std::uint8_t* cpk, const std::uint8_t* seed);
int
pqov_avx2_sign_from_seed(
    std::uint8_t* sig,
    const std::uint8_t* seed,
    const std::uint8_t* msg,
    std::size_t mlen);
#endif
#if PQOV_HAVE_NEON
int
pqov_neon_verify_cpk(
    const std::uint8_t* msg,
    std::size_t mlen,
    const std::uint8_t* sig,
    const std::uint8_t* cpk);
int
pqov_neon_expand_public_key(std::uint8_t* pk, const std::uint8_t* cpk);
int
pqov_neon_verify_pk(
    const std::uint8_t* msg,
    std::size_t mlen,
    const std::uint8_t* sig,
    const std::uint8_t* pk);
int
pqov_neon_keypair_from_seed(std::uint8_t* cpk, const std::uint8_t* seed);
int
pqov_neon_sign_from_seed(
    std::uint8_t* sig,
    const std::uint8_t* seed,
    const std::uint8_t* msg,
    std::size_t mlen);
#endif
}

namespace ripple {
namespace pq {

namespace {

// OV(256,119,45)-pkc
constexpr std::size_t uovIpCpkBytes = 46591;
constexpr std::size_t uovIpPkBytes = 321300;
constexpr std::size_t uovIpSigBytes = 135;
constexpr std::size_t seedBytes = 32;

// Number of expanded public keys (321,300 bytes each) kept in memory.
// 64 entries is ~20 MB.
constexpr std::size_t expandedCacheEntries = 64;

struct Impl
{
    char const* name;
    int (*verifyCpk)(
        const std::uint8_t*,
        std::size_t,
        const std::uint8_t*,
        const std::uint8_t*);
    int (*expand)(std::uint8_t*, const std::uint8_t*);
    int (*verifyPk)(
        const std::uint8_t*,
        std::size_t,
        const std::uint8_t*,
        const std::uint8_t*);
    int (*keypair)(std::uint8_t*, const std::uint8_t*);
    int (*sign)(
        std::uint8_t*,
        const std::uint8_t*,
        const std::uint8_t*,
        std::size_t);
};

Impl const&
impl() noexcept
{
    // Chosen once per process. Every variant computes the same function;
    // only the speed differs.
    static Impl const chosen = []() -> Impl {
#if PQOV_HAVE_AVX2
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("aes") &&
            __builtin_cpu_supports("ssse3"))
            return Impl{
                "avx2",
                pqov_avx2_verify_cpk,
                pqov_avx2_expand_public_key,
                pqov_avx2_verify_pk,
                pqov_avx2_keypair_from_seed,
                pqov_avx2_sign_from_seed};
#endif
#endif
#if PQOV_HAVE_NEON
        return Impl{
            "neon",
            pqov_neon_verify_cpk,
            pqov_neon_expand_public_key,
            pqov_neon_verify_pk,
            pqov_neon_keypair_from_seed,
            pqov_neon_sign_from_seed};
#endif
        return Impl{
            "portable",
            pqov_portable_verify_cpk,
            pqov_portable_expand_public_key,
            pqov_portable_verify_pk,
            pqov_portable_keypair_from_seed,
            pqov_portable_sign_from_seed};
    }();
    return chosen;
}

/** LRU cache of expanded public keys, keyed by the key's AccountID.

    Expanding a compressed UOV key is most of the cost of a verification
    (roughly 180k of 250k cycles with AVX2, and nearly all of it on the
    portable build). Validators and active accounts verify against the same
    few keys over and over, so caching the expanded form makes those
    verifications run at the classic-key speed. */
class ExpandedKeyCache
{
    using Entry = std::pair<AccountID, std::unique_ptr<std::uint8_t[]>>;
    std::mutex mutex_;
    std::list<Entry> lru_;
    std::unordered_map<AccountID, std::list<Entry>::iterator, hardened_hash<>>
        index_;

public:
    // Returns a shared view of the expanded key, expanding (and caching) it
    // on a miss. Copies out of the cache to avoid holding the lock during
    // verification; 321 KB memcpy is far cheaper than a key expansion.
    bool
    verify(
        AccountID const& id,
        Slice const& cpk,
        Slice const& msg,
        Slice const& sig)
    {
        auto pk = std::make_unique<std::uint8_t[]>(uovIpPkBytes);
        bool hit = false;
        {
            std::lock_guard lock(mutex_);
            if (auto it = index_.find(id); it != index_.end())
            {
                lru_.splice(lru_.begin(), lru_, it->second);
                std::memcpy(pk.get(), it->second->second.get(), uovIpPkBytes);
                hit = true;
            }
        }
        if (!hit)
        {
            if (impl().expand(pk.get(), cpk.data()) != 0)
                return false;
            auto copy = std::make_unique<std::uint8_t[]>(uovIpPkBytes);
            std::memcpy(copy.get(), pk.get(), uovIpPkBytes);
            std::lock_guard lock(mutex_);
            if (index_.find(id) == index_.end())
            {
                lru_.emplace_front(id, std::move(copy));
                index_[id] = lru_.begin();
                while (lru_.size() > expandedCacheEntries)
                {
                    index_.erase(lru_.back().first);
                    lru_.pop_back();
                }
            }
        }
        return impl().verifyPk(msg.data(), msg.size(), sig.data(), pk.get()) ==
            0;
    }
};

ExpandedKeyCache&
cache()
{
    static ExpandedKeyCache c;
    return c;
}

}  // namespace

std::optional<KeyType>
keyTypeFromByte(std::uint8_t b) noexcept
{
    switch (b)
    {
        case static_cast<std::uint8_t>(KeyType::uovIp):
            return KeyType::uovIp;
        default:
            return std::nullopt;
    }
}

std::size_t
publicKeyBytes(KeyType t) noexcept
{
    switch (t)
    {
        case KeyType::uovIp:
            return uovIpCpkBytes;
    }
    return 0;
}

std::size_t
signatureBytes(KeyType t) noexcept
{
    switch (t)
    {
        case KeyType::uovIp:
            return uovIpSigBytes;
    }
    return 0;
}

std::size_t
expandedPublicKeyBytes(KeyType t) noexcept
{
    switch (t)
    {
        case KeyType::uovIp:
            return uovIpPkBytes;
    }
    return 0;
}

std::optional<KeyType>
inlineKeyType(Slice const& key) noexcept
{
    if (key.size() < 2 || key[0] != inlinePrefix)
        return std::nullopt;
    auto const t = keyTypeFromByte(key[1]);
    if (!t || key.size() != 2 + publicKeyBytes(*t))
        return std::nullopt;
    return t;
}

bool
isKeyReference(Slice const& key) noexcept
{
    return key.size() == referenceBytes && key[0] == referencePrefix;
}

AccountID
referenceAccountID(Slice const& ref) noexcept
{
    AccountID id;
    std::memcpy(id.data(), ref.data() + 1, AccountID::bytes);
    return id;
}

AccountID
calcAccountID(Slice const& inlineKey) noexcept
{
    static_assert(AccountID::bytes == sizeof(ripesha_hasher::result_type));
    ripesha_hasher rsh;
    rsh(inlineKey.data(), inlineKey.size());
    return AccountID{static_cast<ripesha_hasher::result_type>(rsh)};
}

Blob
makeInlineKey(KeyType t, Slice const& rawKey)
{
    Blob b;
    b.reserve(2 + rawKey.size());
    b.push_back(inlinePrefix);
    b.push_back(static_cast<std::uint8_t>(t));
    b.insert(b.end(), rawKey.data(), rawKey.data() + rawKey.size());
    return b;
}

Blob
makeReference(AccountID const& id)
{
    Blob b;
    b.reserve(referenceBytes);
    b.push_back(referencePrefix);
    b.insert(b.end(), id.data(), id.data() + AccountID::bytes);
    return b;
}

bool
verify(Slice const& inlineKey, Slice const& msg, Slice const& sig) noexcept
{
    auto const t = inlineKeyType(inlineKey);
    if (!t || sig.size() != signatureBytes(*t))
        return false;
    try
    {
        switch (*t)
        {
            case KeyType::uovIp:
                return cache().verify(
                    calcAccountID(inlineKey), rawKey(inlineKey), msg, sig);
        }
    }
    catch (std::exception const&)
    {
    }
    return false;
}

std::optional<Blob>
publicKeyFromSeed(KeyType t, Slice const& seed)
{
    if (t != KeyType::uovIp || seed.size() != seedBytes)
        return std::nullopt;
    Blob raw(uovIpCpkBytes);
    if (impl().keypair(raw.data(), seed.data()) != 0)
        return std::nullopt;
    return makeInlineKey(t, makeSlice(raw));
}

std::optional<Blob>
sign(KeyType t, Slice const& seed, Slice const& msg)
{
    if (t != KeyType::uovIp || seed.size() != seedBytes)
        return std::nullopt;
    Blob sig(uovIpSigBytes);
    if (impl().sign(sig.data(), seed.data(), msg.data(), msg.size()) != 0)
        return std::nullopt;
    return sig;
}

char const*
implementationName() noexcept
{
    return impl().name;
}

}  // namespace pq
}  // namespace ripple
