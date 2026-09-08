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
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ripple {

class ReadView;

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

    An instance of ManifestCache stores, for each trusted validator, (a) its
    master public key, and (b) the most senior of all valid manifests it has
    seen for that validator, if any.  On startup, the [validator_token] config
    entry (which contains the manifest for this validator) is decoded and
    added to the manifest cache.  Other manifests are added as "gossip"
    received from rippled peers.

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

inline std::optional<Manifest>
deserializeManifest(
    STObject const& st,
    beast::Journal journal = beast::Journal(beast::Journal::getNullSink()))
{
    Serializer s;
    st.add(s);
    return deserializeManifest(makeSlice(s.peekData()), journal);
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

    /// No room for a previously unseen unlisted identity
    full,

    /// Peer gossip is only accepted for locally listed/configured identities
    unlisted
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
        case ManifestDisposition::full:
            return "full";
        case ManifestDisposition::unlisted:
            return "unlisted";
        default:
            return "unknown";
    }
}

class DatabaseCon;

/** Remembers manifests with the highest sequence number. */
class ManifestCache
{
private:
    beast::Journal j_;
    std::shared_mutex mutable mutex_;

    /** Active manifests stored by master public key. */
    hash_map<PublicKey, Manifest> map_;

    /** Master public keys stored by current ephemeral public key. */
    hash_map<PublicKey, PublicKey> signingToMasterKeys_;

    /** Master keys eligible for peer gossip.

        Set by pin(); in practice the master keys on the configured validator
        lists, which are the manifests consensus actually depends on.
    */
    hash_set<PublicKey> pinned_;

    // Configured identities remain admissible independently of publisher lists.
    hash_set<PublicKey> configured_;
    std::size_t const cacheLimit_;

    // Attached at startup; the application owns the database for our lifetime.
    DatabaseCon* wallet_ = nullptr;

    enum class Admission { normal, gossip, listedHistory };

    ManifestDisposition
    applyManifest(Manifest m, Admission admission);

    void
    restoreListed(hash_set<PublicKey> const& keys);

    /** Ephemeral keys already probed against the ledger, and where.

        Bounds the reads driven by incoming validations to one per key per
        ledger. A key that resolves does not come back here: the mapping then
        lives in signingToMasterKeys_ and applyLedgerSigningKey() answers from
        it before reaching this map.

        Guarded by mutex_ in exclusive mode.
    */
    hash_set<PublicKey> probed_;
    std::uint32_t probedLedger_ = 0;

    std::atomic<std::uint32_t> seq_{0};

public:
    /** Stop admitting new unlisted identities at this total cache size.

        Peer gossip admits only listed/configured identities. This threshold
        bounds additional cold ledger discoveries. Existing entries are never
        evicted; listed/configured identities and their restored history may
        exceed it. Delisting does not erase a previously admitted identity.
    */
    static constexpr std::size_t cacheLimit = 1024;

    /** Ceiling on distinct cold ledger probes per ledger. */
    static constexpr std::size_t probeLimit = 4096;

    explicit ManifestCache(
        beast::Journal j = beast::Journal(beast::Journal::getNullSink()),
        std::size_t maxEntries = cacheLimit)
        : j_(j), cacheLimit_(maxEntries)
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

    /** Returns `true` if master key has been revoked in a manifest.

        @param pk Master public key

        @par Thread Safety

        May be called concurrently
    */
    bool
    revoked(PublicKey const& pk) const;

    /** Add manifest to cache.

        @param m Manifest to add

        @return `ManifestDisposition::accepted` if successful, or
                the disposition explaining why admission was refused

        @par Thread Safety

        May be called concurrently
    */
    ManifestDisposition
    applyManifest(Manifest m);

    /** Apply peer gossip only for locally listed/configured master keys.
        Membership is checked before signature work and again before insertion.
    */
    ManifestDisposition
    applyGossipManifest(Manifest m);

    /** Cheap admission hint; applyGossipManifest rechecks after queueing. */
    bool
    isGossipCandidate(Manifest const& m) const;

    /** Recover listed master keys' saved history and offer them to peers.

        Replaces any previous set. Bumps sequence() when the set actually
        changes, so a cached gossip message built from it is rebuilt.

        @param keys Master public keys to pin

        @par Thread Safety

        May be called concurrently
    */
    void
    pin(hash_set<PublicKey> keys);

    /** Returns the sequence and serialized form of a held manifest.

        Unlike getManifest() and getSequence(), a revoked master key is
        reported rather than skipped. Those two answer "what should I trust",
        for which a revocation is correctly nothing; a caller republishing what
        it holds needs the revocation most of all.

        @param pk Master public key

        @par Thread Safety

        May be called concurrently
    */
    std::optional<std::pair<std::uint32_t, std::string>>
    getRawManifest(PublicKey const& pk) const;

    /** Ingest manifests published on-ledger.

        Reads keylet::manifest() for each supplied master key, reconstructs any
        manifest found and feeds it through applyManifest(), so an on-chain
        manifest is subject to exactly the same staleness, revocation and
        key-reuse rules -- and the same signature check -- as one arriving by
        peer gossip or in a published list. It is a third source of manifests,
        not a more trusted one.

        Probes a known key set rather than scanning the ledger's transactions:
        this costs one SHAMap read per key, and picks up manifests published in
        ledgers this node never saw.

        @param view Ledger to read from
        @param masterKeys Master public keys to probe for

        @return the number of manifests newly accepted

        @par Thread Safety

        May be called concurrently
    */
    std::size_t
    applyLedger(ReadView const& view, hash_set<PublicKey> const& masterKeys);

    /** Resolve an ephemeral signing key against a manifest published on-ledger.

        applyLedger() probes a known master key set, which cannot help a key
        this node has no manifest for: the master key is exactly what is
        missing. SetManifest writes a second copy of every manifest keyed by
        its ephemeral key, so that case is one read rather than a search.

        Anything found is fed through applyManifest(), so an on-chain manifest
        faces the same signature check and the same staleness, revocation and
        key-reuse rules as one arriving by gossip. The ledger is a transport
        here, not an authority: the answer is read back out of the cache rather
        than taken from the ledger object, because applyManifest() may decline
        it.

        A key is probed at most once per ledger, and a key that resolves is
        answered from the cache thereafter without any ledger read.

        @param view Ledger to read from
        @param signingKey Ephemeral public key to resolve

        @return the master key now associated with signingKey, if any

        @par Thread Safety

        May be called concurrently
    */
    std::optional<PublicKey>
    applyLedgerSigningKey(ReadView const& view, PublicKey const& signingKey);

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

    /** Load local identities before lists are pinned and wallet rows restored.
     */
    bool
    loadConfig(
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

    /** Restore only local identities, and recover newly listed masters before
        their publisher's embedded manifests are accepted. Unrelated old wallet
        rows remain on disk; they are not a source of unlisted cache entries.
    */
    void
    loadListed(DatabaseCon& dbCon);

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

    /** Invokes the callback for the manifests worth offering a new peer.

        Gossip follows local list/configuration membership, not cache contents.
        A cold ledger lookup must not turn into permissionless gossip merely
        because its result was cached.

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
    for_each_gossip_manifest(PreFun&& pf, EachFun&& f) const
    {
        std::shared_lock lock{mutex_};

        // An upper bound: a pinned key need not have a manifest yet.
        pf(pinned_.size() + configured_.size());

        for (auto const& key : pinned_)
            if (auto const iter = map_.find(key); iter != map_.end())
                f(iter->second);

        for (auto const& key : configured_)
            if (!pinned_.contains(key))
                if (auto const iter = map_.find(key); iter != map_.end())
                    f(iter->second);
    }
};

}  // namespace ripple

#endif
