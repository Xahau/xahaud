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

#ifndef RIPPLE_PROTOCOL_PQPUBLICKEY_H_INCLUDED
#define RIPPLE_PROTOCOL_PQPUBLICKEY_H_INCLUDED

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/AccountID.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ripple {
namespace pq {

/** Post-quantum public keys (featurePQSignatures).

    Classical keys are 33 bytes and fit inside every transaction. A UOV
    public key is 46,591 bytes, so it is sent once and stored in the ledger
    as an ltPUBLIC_KEY object keyed by the key's AccountID; later
    transactions refer to it by that AccountID.

    Two new encodings may appear wherever a classical public key is accepted
    (SigningPubKey of a transaction or of a Signers entry, the key argument
    of the util_verify Hook API):

      inline:    0xF0 | type (1 byte) | raw public key (publicKeyBytes(type))
      reference: 0xF1 | AccountID (20 bytes)

    The AccountID of a key is calcAccountID() of its *inline* encoding, so a
    master-key signer's Account is derived from the key exactly as for
    classical keys, and the type byte is part of what is hashed.

    Signatures are the scheme's raw signature bytes: there is no prefix, the
    scheme is known from the key.
*/

constexpr std::uint8_t inlinePrefix = 0xF0;
constexpr std::uint8_t referencePrefix = 0xF1;

/** Scheme + parameter set. Values are never reused: a parameter tweak is a
    new type, so existing ledger objects and signatures stay verifiable. */
enum class KeyType : std::uint8_t {
    /// UOV, OV(256,119,45), compressed public key (pqov round-3 parameters)
    uovIp = 0x01,
};

constexpr std::size_t referenceBytes = 1 + AccountID::bytes;  // 21

std::optional<KeyType>
keyTypeFromByte(std::uint8_t b) noexcept;

/// Size of the raw public key for a type (46,591 for uovIp)
std::size_t
publicKeyBytes(KeyType t) noexcept;

/// Size of a signature for a type (135 for uovIp)
std::size_t
signatureBytes(KeyType t) noexcept;

/// Size of the expanded public key kept in the verification cache
std::size_t
expandedPublicKeyBytes(KeyType t) noexcept;

/** Returns the key type if `key` is a well-formed inline encoding:
    prefix, known type, and exactly 2 + publicKeyBytes(type) bytes. */
std::optional<KeyType>
inlineKeyType(Slice const& key) noexcept;

inline bool
isInlineKey(Slice const& key) noexcept
{
    return inlineKeyType(key).has_value();
}

/// True if `key` is exactly 0xF1 followed by 20 bytes
bool
isKeyReference(Slice const& key) noexcept;

/// The AccountID named by a well-formed reference (precondition: isKeyReference)
AccountID
referenceAccountID(Slice const& ref) noexcept;

/// The AccountID of a well-formed inline key (precondition: isInlineKey)
AccountID
calcAccountID(Slice const& inlineKey) noexcept;

/// Build the inline encoding from a type and the raw key bytes
Blob
makeInlineKey(KeyType t, Slice const& rawKey);

/// Build a 21 byte reference
Blob
makeReference(AccountID const& id);

/// The raw key bytes of a well-formed inline key
inline Slice
rawKey(Slice const& inlineKey) noexcept
{
    return Slice(inlineKey.data() + 2, inlineKey.size() - 2);
}

/** Verify `sig` over `msg` with a well-formed inline key.

    Expanded forms of recently used keys are cached process-wide (keyed by
    the key's AccountID), so repeated verifications against the same key run
    at the expanded-key speed. Returns false for malformed input. */
bool
verify(Slice const& inlineKey, Slice const& msg, Slice const& sig) noexcept;

/** Derive the inline public key from a 32 byte secret seed. Returns
    std::nullopt on failure. Used by tooling and tests; xahaud never signs. */
std::optional<Blob>
publicKeyFromSeed(KeyType t, Slice const& seed);

/** Sign `msg` with a 32 byte secret seed. UOV signatures are salted and are
    therefore not deterministic. */
std::optional<Blob>
sign(KeyType t, Slice const& seed, Slice const& msg);

/// Name of the SIMD variant in use ("avx2", "neon" or "portable")
char const*
implementationName() noexcept;

}  // namespace pq
}  // namespace ripple

#endif
