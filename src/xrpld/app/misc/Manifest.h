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

#ifndef RIPPLE_APP_MISC_MANIFEST_H_INCLUDED
#define RIPPLE_APP_MISC_MANIFEST_H_INCLUDED

#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/base64.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

namespace ripple {

/*
    Validator key manifests
    -----------------------

    Suppose the secret keys installed on a Ripple validator are compromised. Not
    only do you have to generate and install new key pairs on each validator,
    EVERY rippled needs to have its config updated with the new public keys, and
    is vulnerable to forged validation signatures until this is done.  The
    solution is a new layer of indirection: A master secret key under
    restrictive access control is used to sign a "manifest": essentially, a
    certificate including the master public key, an ephemeral public key for
    verifying validations (which will be signed by its secret counterpart), a
    sequence number, and a digital signature.

    The manifest has two serialized forms: one which includes the digital
    signature and one which doesn't.  There is an obvious causal dependency
    relationship between the (latter) form with no signature, the signature
    of that form, and the (former) form which includes that signature.  In
    other words, a message can't contain a signature of itself.  The code
    below stores a serialized manifest which includes the signature, and
    dynamically generates the signatureless form when it needs to verify
    the signature.

    An instance of ManifestCache stores, for each known validator, (a) its
    master public key, and (b) the most senior of all valid manifests it has
    seen for that validator, if any.  On startup, the [validator_token] config
    entry (which contains the manifest for this validator) is decoded and
    added to the manifest cache.  Other manifests are added as "gossip"
    received from rippled peers, including ones for validators this node does
    not trust. Manifests for untrusted validators are capped
    (kMaxUntrustedCount) so peer gossip cannot grow the cache without bound.
    Entries admitted as trusted, or later promoted to trusted, are not capped
    or evicted. Promotion is one-way. At capacity, an untrusted entry is
    evicted to admit a new valid manifest; entries whose signing keys have
    current validations are avoided while a dormant victim exists.

    When an ephemeral key is compromised, a new signing key pair is created,
    along with a new manifest vouching for it (with a higher sequence number),
    signed by the master key.  When a rippled peer receives the new manifest,
    it verifies it with the master key and (assuming it's valid) discards the
    old ephemeral key and stores the new one.  If the master key itself gets
    compromised, a manifest with sequence number 0xFFFFFFFF will supersede a
    prior manifest and discard any existing ephemeral key without storing a
    new one.  These revocation manifests are loaded from the
    [validator_key_revocation] config entry as well as received as gossip from
    peers.  Since no further manifests for this master key will be accepted
    (since no higher sequence number is possible), and no signing key is on
    record, no validations will be accepted from the compromised validator.
*/

//------------------------------------------------------------------------------

struct Manifest
{
    /// The manifest in serialized form.
    std::string serialized;

    /// The master key associated with this manifest.
    PublicKey masterKey;

    /// The ephemeral key associated with this manifest.
    // A revoked manifest does not have a signingKey
    // This field is specified as "optional" in manifestFormat's
    // SOTemplate
    std::optional<PublicKey> signingKey;

    /// The sequence number of this manifest.
    std::uint32_t sequence = 0;

    /// The domain, if one was specified in the manifest; empty otherwise.
    std::string domain;

    Manifest() = delete;

    Manifest(
        std::string const& serialized_,
        PublicKey const& masterKey_,
        std::optional<PublicKey> const& signingKey_,
        std::uint32_t seq,
        std::string const& domain_)
        : serialized(serialized_)
        , masterKey(masterKey_)
        , signingKey(signingKey_)
        , sequence(seq)
        , domain(domain_)
    {
    }

    Manifest(Manifest const& other) = delete;
    Manifest&
    operator=(Manifest const& other) = delete;
    Manifest(Manifest&& other) = default;
    Manifest&
    operator=(Manifest&& other) = default;

    /// Returns `true` if manifest signature is valid
    bool
    verify() const;

    /// Returns hash of serialized manifest data
    uint256
    hash() const;

    /// Returns `true` if manifest revokes master key
    // The maximum possible sequence number means that the master key has
    // been revoked
    static bool
    revoked(std::uint32_t sequence);

    /// Returns `true` if manifest revokes master key
    bool
    revoked() const;

    /// Returns manifest signature
    std::optional<Blob>
    getSignature() const;

    /// Returns manifest master key signature
    Blob
    getMasterSignature() const;
};

/** Format the specified manifest to a string for debugging purposes. */
std::string
to_string(Manifest const& m);

/** Largest a valid manifest can be, in decoded bytes. */
constexpr std::size_t kMaxManifestBytes = 358;

/** Largest a valid manifest can be, in base64 characters. */
constexpr std::size_t kMaxManifestBase64 =
    base64::encoded_size(kMaxManifestBytes);

/** Maximum number of untrusted manifests processed from one message. */
constexpr std::size_t kMaxManifestsPerMessage = 200;

/** Maximum total manifest entries processed from one message.

    This is separate from the wire-byte limit and the valid-untrusted limit:
    malformed entries can be only a few bytes each and never reach the latter.
 */
constexpr std::size_t kMaxManifestEntriesPerMessage = 1000;

/** Constructs Manifest from serialized string

    @param s Serialized manifest string

    @return `std::nullopt` if string is invalid

    @note This does not verify manifest signatures.
          `Manifest::verify` should be called after constructing manifest.
*/
/** @{ */
std::optional<Manifest>
deserializeManifest(Slice s, beast::Journal journal);

inline std::optional<Manifest>
deserializeManifest(
    std::string const& s,
    beast::Journal journal = beast::Journal(beast::Journal::getNullSink()))
{
    return deserializeManifest(makeSlice(s), journal);
}

template <
    class T,
    class = std::enable_if_t<
        std::is_same<T, char>::value || std::is_same<T, unsigned char>::value>>
std::optional<Manifest>
deserializeManifest(
    std::vector<T> const& v,
    beast::Journal journal = beast::Journal(beast::Journal::getNullSink()))
{
    return deserializeManifest(makeSlice(v), journal);
}
/** @} */

inline bool
operator==(Manifest const& lhs, Manifest const& rhs)
{
    // In theory, comparing the two serialized strings should be
    // sufficient.
    return lhs.sequence == rhs.sequence && lhs.masterKey == rhs.masterKey &&
        lhs.signingKey == rhs.signingKey && lhs.domain == rhs.domain &&
        lhs.serialized == rhs.serialized;
}

inline bool
operator!=(Manifest const& lhs, Manifest const& rhs)
{
    return !(lhs == rhs);
}

struct ValidatorToken
{
    std::string manifest;
    SecretKey validationSecret;
};

std::optional<ValidatorToken>
loadValidatorToken(
    std::vector<std::string> const& blob,
    beast::Journal journal = beast::Journal(beast::Journal::getNullSink()));

enum class ManifestDisposition {
    /// Manifest is valid
    accepted = 0,

    /// Sequence is too old
    stale,

    /// The master key is not acceptable to us
    badMasterKey,

    /// The ephemeral key is not acceptable to us
    badEphemeralKey,

    /// Timely, but invalid signature
    invalid,

    /// Unlisted and limit reached
    untrustedCapacity
};

/** Result of bounded untrusted-manifest admission.

    `acceptedUpdate` is meaningful only when `disposition` is `accepted`. It is
    computed under the cache write lock and distinguishes an update of a
    retained identity from admission as a new identity.
*/
struct ManifestApplyResult
{
    ManifestDisposition disposition;
    bool acceptedUpdate;
};

inline std::string
to_string(ManifestDisposition m)
{
    switch (m)
    {
        case ManifestDisposition::accepted:
            return "accepted";
        case ManifestDisposition::stale:
            return "stale";
        case ManifestDisposition::badMasterKey:
            return "badMasterKey";
        case ManifestDisposition::badEphemeralKey:
            return "badEphemeralKey";
        case ManifestDisposition::invalid:
            return "invalid";
        case ManifestDisposition::untrustedCapacity:
            return "untrustedCapacity";
        default:
            return "unknown";
    }
}

/** Whether a manifest counts against the untrusted cache cap. */
enum class ManifestRateLimitCapPolicy : std::uint8_t { Capped, Uncapped };

class DatabaseCon;

/** Remembers manifests with the highest sequence number.

    This is protocol state, not merely a payload cache. While an entry remains
    resident, it supplies the sequence high-water mark, revocation state, and
    master/signing-key collision checks for that validator. Evicting an
    untrusted entry necessarily forgets those facts and can make an old
    manifest cache-new again.

    Entries admitted uncapped, or later promoted, are outside the eviction
    population. For unlisted validators, recent validation activity is only an
    eviction preference; it does not confer trust and cannot prevent eviction
    when every candidate is active.

    This is not complete adversarial containment. Once full, the cache gives
    valid novel identities a small eviction budget, bounding admitted identity
    churn. Capacity rejection normally avoids verification when no permit is
    available, although concurrent callers may race on observed availability.
    Updates to an already retained identity do not consume the budget.
    First-seen unlisted identities are not relayed live. A bounded selected
    subset propagates through the cached connection snapshot.
    Later rotations and revocations relay while the identity remains resident.
    If eviction forgets one, its reappearance is first-seen again and therefore
    does not immediately relay. A sustained sender can monopolize the eviction
    budget and delay a legitimate novel untrusted validator; protected
    validators are unaffected.
*/
class ManifestCache
{
private:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Now = std::function<TimePoint()>;

    beast::Journal j_;
    Now now_;
    std::shared_mutex mutable mutex_;

    /** Active manifests stored by master public key. */
    hash_map<PublicKey, Manifest> map_;

    /** Master public keys stored by current ephemeral public key. */
    hash_map<PublicKey, PublicKey> signingToMasterKeys_;

    std::atomic<std::uint32_t> seq_{0};

    /** Master keys currently counted against the untrusted cache cap. */
    hash_set<PublicKey> untrustedKeys_;

    /** Maximum number of untrusted master keys retained in memory. */
    static constexpr std::size_t kMaxUntrustedCount = 1000;

    /** Burst and refill rate for evictions after the untrusted cache fills. */
    static constexpr std::size_t kMaxEvictionPermits = 10;
    static constexpr std::chrono::seconds kEvictionPermitInterval{1};
    std::size_t evictionPermits_ = kMaxEvictionPermits;
    TimePoint evictionBudgetUpdated_;

    /** Number of manifests rejected because the untrusted cache was full. */
    std::atomic<std::uint64_t> untrustedRejectCount_{0};

    /** Number of capacity rejections between warning summaries. */
    static constexpr std::uint64_t kUntrustedRejectCount = 10000;

    ManifestDisposition
    applyManifestImpl(
        Manifest m,
        ManifestRateLimitCapPolicy cap,
        hash_set<PublicKey> const* currentValidationKeys,
        bool* acceptedUpdate);

public:
    explicit ManifestCache(
        beast::Journal j = beast::Journal(beast::Journal::getNullSink()),
        Now now = [] { return std::chrono::steady_clock::now(); })
        : j_(j), now_(std::move(now)), evictionBudgetUpdated_(now_())
    {
    }

    /** A monotonically increasing number used to detect new manifests. */
    std::uint32_t
    sequence() const
    {
        return seq_.load();
    }

    /** Returns master key's current signing key.

        @param pk Master public key

        @return pk if no known signing key from a manifest

        @par Thread Safety

        May be called concurrently
    */
    std::optional<PublicKey>
    getSigningKey(PublicKey const& pk) const;

    /** Returns ephemeral signing key's master public key.

        @param pk Ephemeral signing public key

        @return pk if signing key is not in a valid manifest

        @par Thread Safety

        May be called concurrently
    */
    PublicKey
    getMasterKey(PublicKey const& pk) const;

    /** Returns master key's current manifest sequence.

        @return sequence corresponding to Master public key
          if configured or std::nullopt otherwise
    */
    std::optional<std::uint32_t>
    getSequence(PublicKey const& pk) const;

    /** Returns domain claimed by a given public key

        @return domain corresponding to Master public key
          if present, otherwise std::nullopt
    */
    std::optional<std::string>
    getDomain(PublicKey const& pk) const;

    /** Returns mainfest corresponding to a given public key

        @return manifest corresponding to Master public key
          if present, otherwise std::nullopt
    */
    std::optional<std::string>
    getManifest(PublicKey const& pk) const;

    /** Return a copy of the ordinary manifest for a master key.

        Unlike `getManifest`, this includes terminal revocations. The returned
        copy can safely be composed with monitoring-only publisher state after
        the cache lock is released.
    */
    std::shared_ptr<Manifest const>
    getManifestByMaster(PublicKey const& pk) const;

    /** Returns `true` if master key has been revoked in a manifest.

        @param pk Master public key

        @par Thread Safety

        May be called concurrently
    */
    bool
    revoked(PublicKey const& pk) const;

    /** Add manifest to cache.

        @param m Manifest to add

        @param cap Whether a new master key counts against the untrusted cap

        @return `ManifestDisposition::accepted` if successful, or
                `stale` or `invalid` otherwise

        @par Thread Safety

        May be called concurrently
    */
    ManifestDisposition
    applyManifest(Manifest m, ManifestRateLimitCapPolicy cap);

    /** Add an untrusted manifest, evicting another at capacity.

        A dormant untrusted entry is chosen at random when possible. If all
        retained untrusted signing keys have current validations, any
        untrusted entry may be chosen. The candidate is fully verified before
        eviction.

        @param m Manifest to add
        @param currentValidationKeys Signing keys with current validations;
               these are eviction preferences, not trusted identities

        @return disposition and an atomic indication that an accepted manifest
                updated an identity retained at admission time
    */
    ManifestApplyResult
    applyManifestWithEviction(
        Manifest m,
        hash_set<PublicKey> const& currentValidationKeys);

    /** Stop counting a cached master key against the untrusted cap. */
    void
    promoteToTrusted(PublicKey const& pk);

    /** Populate manifest cache with manifests in database and config.

        @param dbCon Database connection with dbTable

        @param dbTable Database table

        @param configManifest Base64 encoded manifest for local node's
            validator keys

        @param configRevocation Base64 encoded validator key revocation
            from the config

        @par Thread Safety

        May be called concurrently
    */
    bool
    load(
        DatabaseCon& dbCon,
        std::string const& dbTable,
        std::string const& configManifest,
        std::vector<std::string> const& configRevocation);

    /** Populate manifest cache with manifests in database.

        @param dbCon Database connection with dbTable

        @param dbTable Database table

        @par Thread Safety

        May be called concurrently
    */
    void
    load(DatabaseCon& dbCon, std::string const& dbTable);

    /** Save cached manifests to database.

        @param dbCon Database connection with `ValidatorManifests` table

        @param isTrusted Function that returns true if manifest is trusted

        @par Thread Safety

        May be called concurrently
    */
    void
    save(
        DatabaseCon& dbCon,
        std::string const& dbTable,
        std::function<bool(PublicKey const&)> const& isTrusted);

    /** Invokes the callback once for every populated manifest.

        @note Do not call ManifestCache member functions from within the
        callback. This can re-lock the mutex from the same thread, which is UB.
        @note Do not write ManifestCache member variables from within the
        callback. This can lead to data races.

        @param f Function called for each manifest

        @par Thread Safety

        May be called concurrently
    */
    template <class Function>
    void
    for_each_manifest(Function&& f) const
    {
        std::shared_lock lock{mutex_};
        for (auto const& [_, manifest] : map_)
        {
            (void)_;
            f(manifest);
        }
    }

    /** Invokes the callback once for every populated manifest.

        @note Do not call ManifestCache member functions from within the
        callback. This can re-lock the mutex from the same thread, which is UB.
        @note Do not write ManifestCache member variables from
        within the callback. This can lead to data races.

        @param pf Pre-function called with the maximum number of times f will be
            called (useful for memory allocations)

        @param f Function called for each manifest

        @par Thread Safety

        May be called concurrently
    */
    template <class PreFun, class EachFun>
    void
    for_each_manifest(PreFun&& pf, EachFun&& f) const
    {
        std::shared_lock lock{mutex_};
        pf(map_.size());
        for (auto const& [_, manifest] : map_)
        {
            (void)_;
            f(manifest);
        }
    }
};

}  // namespace ripple

#endif
