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

#ifndef RIPPLE_PROTOCOL_DIGEST_H_INCLUDED
#define RIPPLE_PROTOCOL_DIGEST_H_INCLUDED

#include <ripple/basics/base_uint.h>
#include <ripple/crypto/secure_erase.h>
#include <boost/endian/conversion.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <optional>

namespace ripple {

// Hash context classification - used to identify the purpose of a hash
// operation
enum HashContext : std::uint32_t {
    // Special values
    LEDGER_INDEX_UNKNOWN =
        0,  // DANGEROUS - indicates we don't know the ledger context yet

    // Non-ledger hash contexts (will never need migration)
    LEDGER_INDEX_UNNEEDED = 1,        // Generic non-ledger hashes
    CRYPTO_KEYS_GENERATION_HASH = 2,  // Key derivation and crypto operations
    CRYPTO_SIGNATURE_HASH = 3,        // Signature generation/verification
    FEATURE_HASH = 4,                 // Feature name hashing
    VALIDATOR_LIST_HASH = 5,          // Validator list hashing
    NETWORK_HANDSHAKE_HASH = 6,       // Network protocol handshakes
    SERIALIZER_STOBJECT_HASH = 7,  // STObject hashing (txn IDs, signing hashes)
    CONSENSUS_PROPOSAL_HASH = 8,   // Consensus proposal signing
    PEER_VALIDATION_HASH = 9,      // Peer validation suppression/routing
    FETCH_PACK_CACHE_KEY_HASH = 10,  // Fetch pack cache key verification
    IMPORT_VLCHAIN_HASH =
        11,  // Import from other chain (network 0) - ledger hash
    IMPORT_SHAMAP_TXN_NODE_HASH =
        12,                         // Import VL chain transaction node hashing
    IMPORT_SHAMAP_INNER_HASH = 13,  // Import VL chain inner node hashing
    HOOK_EMITTED_TXN_NONCE = 14,    // Hook emitted transaction nonce generation
    HOOK_LEDGER_NONCE = 15,         // Hook ledger-based nonce generation
    HOOK_UTIL_SHA512H = 16,         // Hook utility sha512h function
    HOOK_DEFINITION = 17,

    // Keylet-specific hash contexts
    KEYLET_ACCOUNT = 18,
    KEYLET_AMENDMENTS = 19,
    KEYLET_BOOK = 20,
    KEYLET_BOOK_BASE = 21,  // For getBookBase function
    KEYLET_CHECK = 22,
    KEYLET_CHILD = 23,
    KEYLET_DEPOSIT_PREAUTH = 24,
    KEYLET_DIR_PAGE = 25,
    KEYLET_EMITTED_DIR = 26,
    KEYLET_EMITTED_TXN = 27,
    KEYLET_ESCROW = 28,
    KEYLET_FEES = 29,
    KEYLET_HOOK = 30,
    KEYLET_HOOK_DEFINITION = 31,
    KEYLET_HOOK_STATE = 32,
    KEYLET_HOOK_STATE_DIR = 33,
    KEYLET_IMPORT_VLSEQ = 34,
    KEYLET_NEGATIVE_UNL = 35,
    KEYLET_NFT_BUYS = 36,
    KEYLET_NFT_OFFER = 37,
    KEYLET_NFT_PAGE = 38,
    KEYLET_NFT_SELLS = 39,
    KEYLET_OFFER = 40,
    KEYLET_OWNER_DIR = 41,
    KEYLET_PAYCHAN = 42,
    KEYLET_SIGNERS = 43,
    KEYLET_SKIP_LIST = 44,
    KEYLET_TICKET = 45,
    KEYLET_TRUSTLINE = 46,
    KEYLET_UNCHECKED = 47,
    KEYLET_UNL_REPORT = 48,
    KEYLET_URI_TOKEN = 49,

    // Ledger-specific hash contexts (will need migration at activation ledger)
    LEDGER_HEADER_HASH = 50,      // Ledger header hash calculation
    SHAMAP_TXN_NODE_HASH = 51,    // SHAMap transaction node hashing
    SHAMAP_INNER_NODE_HASH = 52,  // SHAMap inner node hashing
    SHAMAP_LEAF_NODE_HASH = 53,   // SHAMap leaf node hashing
    TRANSACTION_ID_HASH = 54,     // Transaction ID calculation
    SHARD_NODE_OBJECT_VERIFICATION_HASH =
        55,  // Node object verification in nodestore
    SHAMAP_PROOF_PATH_ANY_TREENODE_HASH =
        56,  // Proof path verification for any tree node type
};

// Options for hash functions (allows future expansion)
struct hash_options
{
    std::optional<std::uint32_t> ledger_index;
    HashContext classifier;

    // Constructor for classifier only (no ledger index)
    explicit hash_options(HashContext ctx)
        : ledger_index(std::nullopt), classifier(ctx)
    {
    }

    // Constructor for both ledger sequence and classifier
    hash_options(std::uint32_t li, HashContext ctx)
        : ledger_index(li), classifier(ctx)
    {
    }
};

// Global stats for hash function usage
struct HashStats
{
    std::atomic<uint64_t> totalSha512HalfCount{0};
    // Track index hashes by namespace (256 possible values for uint8_t space)
    std::array<std::atomic<uint64_t>, 256> indexSha512HalfBySpace{};

    // Timing statistics (in nanoseconds)
    std::atomic<uint64_t> totalSha512HalfTimeNs{0};
    std::atomic<uint64_t> indexSha512HalfTimeNs{0};

    // Computed property for total index sha512Half calls
    uint64_t
    indexSha512HalfCount() const
    {
        uint64_t total = 0;
        for (const auto& count : indexSha512HalfBySpace)
        {
            total += count.load(std::memory_order_relaxed);
        }
        return total;
    }

    // Computed property for non-index sha512Half calls
    uint64_t
    nonIndexSha512HalfCount() const
    {
        return totalSha512HalfCount.load(std::memory_order_relaxed) -
            indexSha512HalfCount();
    }

    // Computed property for non-index sha512Half time
    uint64_t
    nonIndexSha512HalfTimeNs() const
    {
        return totalSha512HalfTimeNs.load(std::memory_order_relaxed) -
            indexSha512HalfTimeNs.load(std::memory_order_relaxed);
    }
};

// Global instance
inline HashStats&
getHashStats()
{
    static HashStats stats;
    return stats;
}

// Get namespace name from index (for reporting)
inline const char*
getLedgerNameSpaceName(std::uint16_t space)
{
    switch (space)
    {
        case 'a':
            return "ACCOUNT";
        case 'd':
            return "DIR_NODE";
        case 'r':
            return "TRUST_LINE";
        case 'o':
            return "OFFER";
        case 'O':
            return "OWNER_DIR";
        case 'B':
            return "BOOK_DIR";
        case 's':
            return "SKIP_LIST";
        case 'u':
            return "ESCROW";
        case 'f':
            return "AMENDMENTS";
        case 'e':
            return "FEE_SETTINGS";
        case 'T':
            return "TICKET";
        case 'S':
            return "SIGNER_LIST";
        case 'x':
            return "PAYMENT_CHANNEL";
        case 'C':
            return "CHECK";
        case 'p':
            return "DEPOSIT_PREAUTH";
        case 'N':
            return "NEGATIVE_UNL";
        case 'H':
            return "HOOK";
        case 'J':
            return "HOOK_STATE_DIR";
        case 'v':
            return "HOOK_STATE";
        case 'D':
            return "HOOK_DEFINITION";
        case 'E':
            return "EMITTED_TXN";
        case 'F':
            return "EMITTED_DIR";
        case 'q':
            return "NFTOKEN_OFFER";
        case 'h':
            return "NFTOKEN_BUY_OFFERS";
        case 'i':
            return "NFTOKEN_SELL_OFFERS";
        case 'U':
            return "URI_TOKEN";
        case 'I':
            return "IMPORT_VLSEQ";
        case 'R':
            return "UNL_REPORT";
        case 'c':
            return "CONTRACT_DEPRECATED";
        case 'g':
            return "GENERATOR_DEPRECATED";
        case 'n':
            return "NICKNAME_DEPRECATED";
        default:
            return nullptr;
    }
}

/** Message digest functions used in the codebase

    @note These are modeled to meet the requirements of `Hasher` in the
          `hash_append` interface, discussed in proposal:

          N3980 "Types Don't Know #"
          http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n3980.html
*/

//------------------------------------------------------------------------------

/** RIPEMD-160 digest

    @note This uses the OpenSSL implementation
*/
struct openssl_ripemd160_hasher
{
public:
    static constexpr auto const endian = boost::endian::order::native;

    using result_type = std::array<std::uint8_t, 20>;

    openssl_ripemd160_hasher();

    void
    operator()(void const* data, std::size_t size) noexcept;

    explicit operator result_type() noexcept;

private:
    char ctx_[96];
};

/** SHA-512 digest

    @note This uses the OpenSSL implementation
*/
struct openssl_sha512_hasher
{
public:
    static constexpr auto const endian = boost::endian::order::native;

    using result_type = std::array<std::uint8_t, 64>;

    openssl_sha512_hasher();

    void
    operator()(void const* data, std::size_t size) noexcept;

    explicit operator result_type() noexcept;

private:
    char ctx_[216];
};

/** SHA-256 digest

    @note This uses the OpenSSL implementation
*/
struct openssl_sha256_hasher
{
public:
    static constexpr auto const endian = boost::endian::order::native;

    using result_type = std::array<std::uint8_t, 32>;

    openssl_sha256_hasher();

    void
    operator()(void const* data, std::size_t size) noexcept;

    explicit operator result_type() noexcept;

private:
    char ctx_[112];
};

//------------------------------------------------------------------------------

using ripemd160_hasher = openssl_ripemd160_hasher;
using sha256_hasher = openssl_sha256_hasher;
using sha512_hasher = openssl_sha512_hasher;

//------------------------------------------------------------------------------

/** Returns the RIPEMD-160 digest of the SHA256 hash of the message.

    This operation is used to compute the 160-bit identifier
    representing a Ripple account, from a message. Typically the
    message is the public key of the account - which is not
    stored in the account root.

    The same computation is used regardless of the cryptographic
    scheme implied by the public key. For example, the public key
    may be an ed25519 public key or a secp256k1 public key. Support
    for new cryptographic systems may be added, using the same
    formula for calculating the account identifier.

    Meets the requirements of Hasher (in hash_append)
*/
struct ripesha_hasher
{
private:
    sha256_hasher h_;

public:
    static constexpr auto const endian = boost::endian::order::native;

    using result_type = std::array<std::uint8_t, 20>;

    void
    operator()(void const* data, std::size_t size) noexcept
    {
        h_(data, size);
    }

    explicit operator result_type() noexcept
    {
        auto const d0 = sha256_hasher::result_type(h_);
        ripemd160_hasher rh;
        rh(d0.data(), d0.size());
        return ripemd160_hasher::result_type(rh);
    }
};

//------------------------------------------------------------------------------

namespace detail {

/** Returns the SHA512-Half digest of a message.

    The SHA512-Half is the first 256 bits of the
    SHA-512 digest of the message.
*/
template <bool Secure>
struct basic_sha512_half_hasher
{
private:
    sha512_hasher h_;
    hash_options opts_;

public:
    static constexpr auto const endian = boost::endian::order::big;

    using result_type = uint256;

    // // Default constructor for backward compatibility
    // basic_sha512_half_hasher() : opts_(LEDGER_INDEX_UNKNOWN)
    // {
    // }

    // Constructor with hash_options for context-aware hashing
    explicit basic_sha512_half_hasher(hash_options const& opts) : opts_(opts)
    {
    }

    ~basic_sha512_half_hasher()
    {
        erase(std::integral_constant<bool, Secure>{});
    }

    void
    operator()(void const* data, std::size_t size) noexcept
    {
        // TODO: When BLAKE3 is added, check opts_.ledger_index (if present) and
        // classifier to determine which hash algorithm to use For now, always
        // use SHA512
        h_(data, size);
    }

    explicit operator result_type() noexcept
    {
        // TODO: When BLAKE3 is added, check opts_.ledger_index (if present) and
        // classifier to determine which hash algorithm to use For now, always
        // use SHA512
        auto const digest = sha512_hasher::result_type(h_);
        return result_type::fromVoid(digest.data());
    }

private:
    inline void erase(std::false_type)
    {
    }

    inline void erase(std::true_type)
    {
        secure_erase(&h_, sizeof(h_));
    }
};

}  // namespace detail

using sha512_half_hasher = detail::basic_sha512_half_hasher<false>;

// secure version
using sha512_half_hasher_s = detail::basic_sha512_half_hasher<true>;

//------------------------------------------------------------------------------

/** Returns the SHA512-Half of a series of objects (with options). */
template <class... Args>
sha512_half_hasher::result_type
sha512Half(hash_options const& opts, Args const&... args)
{
    auto start = std::chrono::high_resolution_clock::now();

    getHashStats().totalSha512HalfCount.fetch_add(1, std::memory_order_relaxed);

    // TODO: Use opts.ledger_index to potentially switch to blake3 at certain
    // ledger For now, still use sha512_half_hasher
    sha512_half_hasher h(opts);
    using beast::hash_append;
    hash_append(h, args...);
    auto result = static_cast<typename sha512_half_hasher::result_type>(h);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    getHashStats().totalSha512HalfTimeNs.fetch_add(
        duration, std::memory_order_relaxed);

    return result;
}

// Removed backward compatibility overload - all callers must provide
// hash_options to ensure proper hash algorithm selection based on ledger
// sequence

/** Returns the SHA512-Half of a series of objects (with options).

    Postconditions:
        Temporary memory storing copies of
        input messages will be cleared.
*/
template <class... Args>
sha512_half_hasher_s::result_type
sha512Half_s(hash_options const& opts, Args const&... args)
{
    auto start = std::chrono::high_resolution_clock::now();

    getHashStats().totalSha512HalfCount.fetch_add(1, std::memory_order_relaxed);

    // TODO: Use opts.ledger_index to potentially switch to blake3 at certain
    // ledger For now, still use sha512_half_hasher_s
    sha512_half_hasher_s h(opts);
    using beast::hash_append;
    hash_append(h, args...);
    auto result = static_cast<typename sha512_half_hasher_s::result_type>(h);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count();
    getHashStats().totalSha512HalfTimeNs.fetch_add(
        duration, std::memory_order_relaxed);

    return result;
}

}  // namespace ripple

#endif
