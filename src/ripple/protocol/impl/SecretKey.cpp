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

#include <ripple/basics/contract.h>
#include <ripple/basics/strHex.h>
#include <ripple/beast/utility/rngfill.h>
#include <ripple/crypto/csprng.h>
#include <ripple/crypto/secure_erase.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/digest.h>
#include <ripple/protocol/impl/secp256k1.h>
#include <cstring>
#include <ed25519-donna/ed25519.h>
#include <cstddef>
#include <cstdint>

#pragma push_macro("L")
#pragma push_macro("K")
#pragma push_macro("N")
#pragma push_macro("S")
#pragma push_macro("U")
#pragma push_macro("D")
#undef L
#undef K
#undef N
#undef S
#undef U
#undef D

extern "C" {
#include "api.h"
#include "fips202.h"
#include "packing.h"
#include "params.h"
#include "poly.h"
#include "polyvec.h"
#include "sign.h"
}

#include <iostream>
#include <iterator>
#include <ostream>
#include <stdexcept>
#include <iomanip>
#include <sstream>


//Define the dilithium functions and sizes with respect to functions named here
#ifndef CRYPTO_PUBLICKEYBYTES
#define CRYPTO_PUBLICKEYBYTES pqcrystals_dilithium2_PUBLICKEYBYTES 
#endif

#ifndef CRYPTO_SECRETKEYBYTES
#define CRYPTO_SECRETKEYBYTES pqcrystals_dilithium2_SECRETKEYBYTES 
#endif

#ifndef CRYPTO_BYTES
#define CRYPTO_BYTES pqcrystals_dilithium2_BYTES 
#endif

#ifndef crypto_sign_keypair
#define crypto_sign_keypair pqcrystals_dilithium2_ref_keypair 
#endif

#ifndef crypto_sign_signature
#define crypto_sign_signature pqcrystals_dilithium2_ref_signature 
#endif

#ifndef crypto_sign_verify
#define crypto_sign_verify pqcrystals_dilithium2_ref_verify 
#endif

#ifndef crypto_sign_open
#define crypto_sign_open pqcrystals_dilithium2_ref_open 
#endif

namespace ripple {

SecretKey::~SecretKey()
{
    secure_erase(buf_, sizeof(buf_));
}

SecretKey::SecretKey(std::array<std::uint8_t, 32> const& key)
{
    std::memcpy(buf_, key.data(), key.size());
}

SecretKey::SecretKey(std::array<std::uint8_t, 2528> const& key)
{
    std::memcpy(buf_, key.data(), key.size());
}

SecretKey::SecretKey(Slice const& slice)
{
    assert(size_ <= max_size);
    if (slice.size() != 32 && slice.size() != 2528) {
        LogicError("SecretKey::SecretKey: invalid size");
    }
    size_ = slice.size();
    std::memcpy(buf_, slice.data(), size_);
}

std::string
SecretKey::to_string() const
{
    return strHex(*this);
}

namespace detail {

void
copy_uint32(std::uint8_t* out, std::uint32_t v)
{
    *out++ = v >> 24;
    *out++ = (v >> 16) & 0xff;
    *out++ = (v >> 8) & 0xff;
    *out = v & 0xff;
}

uint256
deriveDeterministicRootKey(Seed const& seed)
{
    // We fill this buffer with the seed and append a 32-bit "counter"
    // that counts how many attempts we've had to make to generate a
    // non-zero key that's less than the curve's order:
    //
    //                       1    2
    //      0                6    0
    // buf  |----------------|----|
    //      |      seed      | seq|

    std::array<std::uint8_t, 20> buf;
    std::copy(seed.begin(), seed.end(), buf.begin());

    // The odds that this loop executes more than once are neglible
    // but *just* in case someone managed to generate a key that required
    // more iterations loop a few times.
    for (std::uint32_t seq = 0; seq != 128; ++seq)
    {
        copy_uint32(buf.data() + 16, seq);

        auto const ret = sha512Half(buf);

        if (secp256k1_ec_seckey_verify(secp256k1Context(), ret.data()) == 1)
        {
            secure_erase(buf.data(), buf.size());
            return ret;
        }
    }

    Throw<std::runtime_error>("Unable to derive generator from seed");
}

//------------------------------------------------------------------------------
/** Produces a sequence of secp256k1 key pairs.

    The reference implementation of the XRP Ledger uses a custom derivation
    algorithm which enables the derivation of an entire family of secp256k1
    keypairs from a single 128-bit seed. The algorithm predates widely-used
    standards like BIP-32 and BIP-44.

    Important note to implementers:

        Using this algorithm is not required: all valid secp256k1 keypairs will
        work correctly. Third party implementations can use whatever mechanisms
        they prefer. However, implementers of wallets or other tools that allow
        users to use existing accounts should consider at least supporting this
        derivation technique to make it easier for users to 'import' accounts.

    For more details, please check out:
        https://xrpl.org/cryptographic-keys.html#secp256k1-key-derivation
 */
class Generator
{
private:
    uint256 root_;
    std::array<std::uint8_t, 33> generator_;

    uint256
    calculateTweak(std::uint32_t seq) const
    {
        // We fill the buffer with the generator, the provided sequence
        // and a 32-bit counter tracking the number of attempts we have
        // already made looking for a non-zero key that's less than the
        // curve's order:
        //                                        3    3    4
        //      0          pubGen                 3    7    1
        // buf  |---------------------------------|----|----|
        //      |            generator            | seq| cnt|

        std::array<std::uint8_t, 41> buf;
        std::copy(generator_.begin(), generator_.end(), buf.begin());
        copy_uint32(buf.data() + 33, seq);

        // The odds that this loop executes more than once are neglible
        // but we impose a maximum limit just in case.
        for (std::uint32_t subseq = 0; subseq != 128; ++subseq)
        {
            copy_uint32(buf.data() + 37, subseq);

            auto const ret = sha512Half_s(buf);

            if (secp256k1_ec_seckey_verify(secp256k1Context(), ret.data()) == 1)
            {
                secure_erase(buf.data(), buf.size());
                return ret;
            }
        }

        Throw<std::runtime_error>("Unable to derive generator from seed");
    }

public:
    explicit Generator(Seed const& seed)
        : root_(deriveDeterministicRootKey(seed))
    {
        secp256k1_pubkey pubkey;
        if (secp256k1_ec_pubkey_create(
                secp256k1Context(), &pubkey, root_.data()) != 1)
            LogicError("derivePublicKey: secp256k1_ec_pubkey_create failed");

        auto len = generator_.size();

        if (secp256k1_ec_pubkey_serialize(
                secp256k1Context(),
                generator_.data(),
                &len,
                &pubkey,
                SECP256K1_EC_COMPRESSED) != 1)
            LogicError("derivePublicKey: secp256k1_ec_pubkey_serialize failed");
    }

    ~Generator()
    {
        secure_erase(root_.data(), root_.size());
        secure_erase(generator_.data(), generator_.size());
    }

    /** Generate the nth key pair. */
    std::pair<PublicKey, SecretKey>
    operator()(std::size_t ordinal) const
    {
        // Generates Nth secret key:
        auto gsk = [this, tweak = calculateTweak(ordinal)]() {
            auto rpk = root_;

            if (secp256k1_ec_privkey_tweak_add(
                    secp256k1Context(), rpk.data(), tweak.data()) == 1)
            {
                SecretKey sk{Slice{rpk.data(), rpk.size()}};
                secure_erase(rpk.data(), rpk.size());
                return sk;
            }

            LogicError("Unable to add a tweak!");
        }();

        return {derivePublicKey(KeyType::secp256k1, gsk), gsk};
    }
};

}  // namespace detail

Buffer
signDigest(PublicKey const& pk, SecretKey const& sk, uint256 const& digest)
{
    if (publicKeyType(pk.slice()) != KeyType::secp256k1)
        LogicError("sign: secp256k1 required for digest signing");

    BOOST_ASSERT(sk.size() == 32);
    secp256k1_ecdsa_signature sig_imp;
    if (secp256k1_ecdsa_sign(
            secp256k1Context(),
            &sig_imp,
            reinterpret_cast<unsigned char const*>(digest.data()),
            reinterpret_cast<unsigned char const*>(sk.data()),
            secp256k1_nonce_function_rfc6979,
            nullptr) != 1)
        LogicError("sign: secp256k1_ecdsa_sign failed");

    unsigned char sig[72];
    size_t len = sizeof(sig);
    if (secp256k1_ecdsa_signature_serialize_der(
            secp256k1Context(), sig, &len, &sig_imp) != 1)
        LogicError("sign: secp256k1_ecdsa_signature_serialize_der failed");

    return Buffer{sig, len};
}

Buffer
sign(PublicKey const& pk, SecretKey const& sk, Slice const& m)
{
    auto const type = publicKeyType(pk.slice());
    if (!type)
        LogicError("sign: invalid type");
    switch (*type)
    {
        case KeyType::ed25519: {
            Buffer b(64);
            ed25519_sign(
                m.data(), m.size(), sk.data(), pk.data() + 1, b.data());
            return b;
        }
        case KeyType::secp256k1: {
            sha512_half_hasher h;
            h(m.data(), m.size());
            auto const digest = sha512_half_hasher::result_type(h);

            secp256k1_ecdsa_signature sig_imp;
            if (secp256k1_ecdsa_sign(
                    secp256k1Context(),
                    &sig_imp,
                    reinterpret_cast<unsigned char const*>(digest.data()),
                    reinterpret_cast<unsigned char const*>(sk.data()),
                    secp256k1_nonce_function_rfc6979,
                    nullptr) != 1)
                LogicError("sign: secp256k1_ecdsa_sign failed");

            unsigned char sig[72];
            size_t len = sizeof(sig);
            if (secp256k1_ecdsa_signature_serialize_der(
                    secp256k1Context(), sig, &len, &sig_imp) != 1)
                LogicError(
                    "sign: secp256k1_ecdsa_signature_serialize_der failed");

            return Buffer{sig, len};
        }
        case KeyType::dilithium: {
            std::cout << "Dilithium key type supported" << std::endl;
            std::cout << "Signing message using dilithium" << std::endl;
            uint8_t sig[CRYPTO_BYTES];
            size_t len;
            crypto_sign_signature(sig, &len, m.data(), m.size(), sk.data());

            // Debugging statements
            // std::cout << "Signature Dilthium: " << toHexString(sig, len) << std::endl;
            std::cout << "Signature Length (Dilithium): " << len << " bytes" << std::endl;

            // Verify the Signature
            // int verify_result = crypto_sign_verify(sig, len, m.data(), m.size(), pk.data());
            // if (verify_result != 0) {
            //     std::cerr << "Dilithium signature verification failed with error code: " << verify_result << std::endl;
            //     LogicError("sign: Dilithium Signature Verification failed");
            // }
            return Buffer{sig, len};
        }
        default:
            LogicError("sign: invalid type");
    }
}

SecretKey
randomSecretKey()
{
    std::uint8_t buf[32];
    beast::rngfill(buf, sizeof(buf), crypto_prng());
    SecretKey sk(Slice{buf, sizeof(buf)});
    secure_erase(buf, sizeof(buf));
    return sk;

    // std::cout << "randomDilithiumSecretKey() called" << std::endl;
    // uint8_t pk[CRYPTO_PUBLICKEYBYTES];
    // uint8_t sk[CRYPTO_SECRETKEYBYTES];
    // crypto_sign_keypair(pk, sk);

    // // std::cout << "Secret Key: " << toHexString(sk, CRYPTO_SECRETKEYBYTES) << std::endl;
    // std::cout << "Length of Secret Key: (dilithium) " << CRYPTO_SECRETKEYBYTES << " bytes" << std::endl;
    // return SecretKey(Slice{sk, CRYPTO_SECRETKEYBYTES});
}

void
expand_mat(polyvecl mat[K], const uint8_t rho[SEEDBYTES])
{
    unsigned int i, j;
    uint16_t nonce;

    for (i = 0; i < K; ++i)
    {
        for (j = 0; j < L; ++j)
        {
            nonce = (i << 8) + j;  // Combine indices i and j into a nonce
            poly_uniform(&mat[i].vec[j], rho, nonce);
        }
    }
}
int 
pqcrystals_dilithium2_ref_keypair_seed(
    uint8_t* pk,
    uint8_t* sk,
    const uint8_t* seed)
{
    uint8_t seedbuf[3 * SEEDBYTES];
    uint8_t tr[CRHBYTES];
    const uint8_t *rho, *rhoprime, *key;
    polyvecl mat[K];
    polyvecl s1;
    polyveck s2, t1, t0;

    // Step 1: Expand the provided seed to obtain rho, rhoprime, and key
    shake256(seedbuf, 3 * SEEDBYTES, seed, SEEDBYTES);
    rho = seedbuf;
    rhoprime = seedbuf + SEEDBYTES;
    key = seedbuf + 2 * SEEDBYTES;

    // Step 2: Generate the matrix A using rho
    expand_mat(mat, rho);

    // Step 3: Sample the short vector s1 using rhoprime and transform to NTT domain
    unsigned int nonce = 0;
    for (size_t i = 0; i < L; ++i)
    {
        poly_uniform_eta(&s1.vec[i], rhoprime, nonce++);
        poly_ntt(&s1.vec[i]);
    }

    // Step 4: Sample the short vector s2 using rhoprime and transform to NTT domain
    for (size_t i = 0; i < K; ++i)
    {
        poly_uniform_eta(&s2.vec[i], rhoprime, nonce++);
        poly_ntt(&s2.vec[i]);
    }

    // Step 5: Compute t = A * s1 + s2
    for (size_t i = 0; i < K; ++i)
    {
        poly t;
        // Matrix-vector multiplication
        polyvecl_pointwise_acc_montgomery(&t, &mat[i], &s1);
        poly_reduce(&t);
        // Inverse NTT for t
        poly_invntt_tomont(&t);
        // Add s2 and correct the result
        poly_add(&t, &t, &s2.vec[i]);
        poly_caddq(&t);
        // Step 6: Perform the power2round operation on t
        poly_power2round(&t1.vec[i], &t0.vec[i], &t);
    }

    // Step 7: Pack the public key (rho and t1)
    pack_pk(pk, rho, &t1);

    // Step 8: Compute the hash tr = CRH(rho | t1)
    shake256(tr, CRHBYTES, pk, CRYPTO_PUBLICKEYBYTES);

    // Step 9: Pack the secret key
    pack_sk(sk, rho, key, tr, &t0, &s1, &s2);

    // Optional: Clear sensitive data from memory
    // This step is important for security
    // memset(seedbuf, 0, sizeof(seedbuf));
    // polyvecl_free(&s1);
    // polyveck_free(&s2);
    // polyveck_free(&t0);

    return 0;
}

SecretKey
generateSecretKey(KeyType type, Seed const& seed)
{
    if (type == KeyType::ed25519)
    {
        auto key = sha512Half_s(Slice(seed.data(), seed.size()));
        SecretKey sk{Slice{key.data(), key.size()}};
        secure_erase(key.data(), key.size());
        return sk;
    }

    if (type == KeyType::secp256k1)
    {
        auto key = detail::deriveDeterministicRootKey(seed);
        SecretKey sk{Slice{key.data(), key.size()}};
        secure_erase(key.data(), key.size());
        return sk;
    }

    if (type == KeyType::dilithium)
    {
        std::string seedStr = toBase58(seed);
        std::cout << "Generating SecretKey using Dilithium: " << seedStr << std::endl;
        uint8_t pk[CRYPTO_PUBLICKEYBYTES];
        uint8_t sk_temp[CRYPTO_SECRETKEYBYTES];
        // Generate the key pair from the seed
        if (pqcrystals_dilithium2_ref_keypair_seed(pk, sk_temp, seed.data()) != 0) {
            throw std::runtime_error("Dilithium key pair generation failed");
        }
        SecretKey sk{Slice{sk_temp, CRYPTO_SECRETKEYBYTES}};
        // Debugging statements
        // std::cout << "Secret Key (dilithium): " << toHexString(sk, CRYPTO_SECRETKEYBYTES) << std::endl;
        std::cout << "Secret Key Size (dilithium): generateKeypair() " << CRYPTO_SECRETKEYBYTES << " bytes" << std::endl;
        secure_erase(pk, CRYPTO_PUBLICKEYBYTES);
        return sk;
    }

    LogicError("generateSecretKey: unknown key type");
}

PublicKey
derivePublicKey(KeyType type, SecretKey const& sk)
{
    switch (type)
    {
        case KeyType::secp256k1: {
            secp256k1_pubkey pubkey_imp;
            if (secp256k1_ec_pubkey_create(
                    secp256k1Context(),
                    &pubkey_imp,
                    reinterpret_cast<unsigned char const*>(sk.data())) != 1)
                LogicError(
                    "derivePublicKey: secp256k1_ec_pubkey_create failed");

            unsigned char pubkey[33];
            std::size_t len = sizeof(pubkey);
            if (secp256k1_ec_pubkey_serialize(
                    secp256k1Context(),
                    pubkey,
                    &len,
                    &pubkey_imp,
                    SECP256K1_EC_COMPRESSED) != 1)
                LogicError(
                    "derivePublicKey: secp256k1_ec_pubkey_serialize failed");

            return PublicKey{Slice{pubkey, len}};
        }
        case KeyType::ed25519: {
            unsigned char buf[33];
            buf[0] = 0xED;
            ed25519_publickey(sk.data(), &buf[1]);
            return PublicKey(Slice{buf, sizeof(buf)});
        }
        case KeyType::dilithium: {
             if (sk.size() != CRYPTO_SECRETKEYBYTES) {
                LogicError("derivePublicKey: invalid secret key size for Dilithium");
            }
            uint8_t const* sk_data = sk.data();
            uint8_t const* pk_data = sk_data + (CRYPTO_SECRETKEYBYTES - CRYPTO_PUBLICKEYBYTES);
            return PublicKey{Slice{pk_data, CRYPTO_PUBLICKEYBYTES}};
        }
        default:
            LogicError("derivePublicKey: bad key type");
    };
}

PublicKey derivePublicKey(KeyType type, SecretKey const& sk, Seed const& seed)
{
    if (type != KeyType::dilithium) {
        LogicError("derivePublicKey: unsupported key type with seed");
    }

    uint8_t pk[CRYPTO_PUBLICKEYBYTES];
    uint8_t sk_buffer[CRYPTO_SECRETKEYBYTES];


    // Debugging statement before key derivation
    std::cout << "derivePublicKey() using Dilithium..." << std::endl;

    // if (pqcrystals_dilithium2_ref_keypair_seed(pk, sk_buffer, seed.data()) != 0) {
    //     throw std::runtime_error("derivePublicKey: Dilithium public key derivation failed");
    // }

    // Debugging statements after key derivation
    // std::cout << "Public Key (Dilithium): " << toHexString(pk, CRYPTO_PUBLICKEYBYTES) << std::endl;
    // std::cout << "Public Key (Dilithium): " << toHexString(pk, CRYPTO_PUBLICKEYBYTES) << std::endl;
    std::cout << "derivePublicKey Length (Dilithium): " << CRYPTO_PUBLICKEYBYTES << " bytes" << std::endl;

    return PublicKey{Slice{pk, CRYPTO_PUBLICKEYBYTES}};
}

std::pair<PublicKey, SecretKey>
generateKeyPair(KeyType type, Seed const& seed)
{
    switch (type)
    {
        case KeyType::secp256k1: {
            detail::Generator g(seed);
            return g(0);
        }
        case KeyType::ed25519: {
            auto const sk = generateSecretKey(type, seed);
            return {derivePublicKey(type, sk), sk};
        }
        case KeyType::dilithium: {
            auto const sk = generateSecretKey(type, seed);
            return {derivePublicKey(type, sk), sk};
        }
        default:
            throw std::invalid_argument("Unsupported key type");
    }
}

std::pair<PublicKey, SecretKey>
randomKeyPair(KeyType type)
{
    auto const sk = randomSecretKey();
    return {derivePublicKey(type, sk), sk};
}

template <>
std::optional<SecretKey>
parseBase58(TokenType type, std::string const& s)
{
    auto const result = decodeBase58Token(s, type);
    if (result.empty())
        return std::nullopt;
    if (result.size() != 32 && result.size() != 2528)
        return std::nullopt;
    return SecretKey(makeSlice(result));
}

}  // namespace ripple

#pragma pop_macro("K")
#pragma pop_macro("L")
#pragma pop_macro("N")
#pragma pop_macro("S")
#pragma pop_macro("U")
#pragma pop_macro("D")
