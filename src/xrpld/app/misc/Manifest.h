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

#include <xrpld/app/misc/RotatingBloomFilter.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ripple {

class ReadView;
class STLedgerEntry;
using SLE = STLedgerEntry;

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
    added to the manifest cache.  Other manifests arrive in published validator
    lists, from ltMANIFEST ledger objects, and as "gossip" received from
    rippled peers; see ManifestCache for how each is retained.

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

    /// Gossip introducing a master key not held, whose ephemeral key has not
    /// recently signed a validation this node verified, or which names no
    /// ephemeral key at all. Not admitted and not relayed; if it names an
    /// ephemeral key it is held pending, unverified, until that key is seen.
    unseen,

    /// Gossip with nowhere to go: every manifest held from gossip has been
    /// more active than this one could claim to be.
    full
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
        case ManifestDisposition::unseen:
            return "unseen";
        case ManifestDisposition::full:
            return "full";
        default:
            return "unknown";
    }
}

/** Where a held manifest came from, in increasing order of precedence.

    The source never makes a manifest valid. Whatever its source, a manifest
    is accepted only if its signatures verify and its sequence is newer than
    the one held. The source decides how long the cache keeps it, and which
    side loses when two held manifests claim the same key.
*/
enum class ManifestSource : std::uint8_t {
    /** Relayed by a peer: the legacy transport, open to anyone.

        A master key not already held from gossip is admitted only if its
        ephemeral key has recently signed a validation this node verified, and
        only into one of ManifestCache::gossipCapacity slots, which go to the
        most active.
    */
    gossip = 0,

    /** Read from an ltMANIFEST ledger object.

        The ledger is the store and this is a lookup cache over it, of
        ManifestCache::ledgerCapacity entries: dropping one costs a SHAMap
        read the next time it is wanted, and loses nothing. Revocations are
        expected to live here, since a validator revokes on-ledger.
    */
    ledger = 1,

    /** Carried in a validator list, the config, or the wallet those were
        saved to. Never evicted.
    */
    list = 2,
};

inline std::string
to_string(ManifestSource s)
{
    switch (s)
    {
        case ManifestSource::gossip:
            return "gossip";
        case ManifestSource::ledger:
            return "ledger";
        case ManifestSource::list:
            return "list";
        default:
            return "unknown";
    }
}

class DatabaseCon;

/** Remembers manifests with the highest sequence number.

    Every master key held is retained in one of three tiers:

    - list: the master key is pinned (trusted), configured, or a validator
      list or the wallet carried a manifest for it. Never evicted, whatever
      transport later delivered a newer manifest for it.

    - ledger: otherwise, if the manifest held was read from the ledger. A
      lookup cache over the ledger, capped at ledgerCapacity; when full the
      least active entry is dropped, to be read again on demand.

    - gossip: otherwise. Capped at gossipCapacity. A master key enters only
      if its ephemeral key is in seen_, a rotating bloom filter of ephemeral
      keys that recently signed a validation this node verified, and only by
      displacing an entry less active than a single fresh sighting.

    Activity is the number of validations seen from the held ephemeral key,
    decayed exponentially with a half-life of halfLife. It is what decides
    eviction in both capped tiers.

    Gossip that fails the bloom filter is held, unverified, in a fixed ring of
    pendingCapacity entries keyed by ephemeral key. The first validation
    signed by that key releases it into the cache. Without this a validator's
    manifest, which normally arrives before its first validation, would be
    refused everywhere and never offered again.

    A held manifest that conflicts with a new one over a key, and is held in a
    lower tier than the new one would be, is evicted rather than the new one
    refused. A manifest is never evicted by one ranked at or below it.

    Ledger access goes through readLedger() alone, so a source other than a
    full SHAMap can be substituted there.
*/
class ManifestCache
{
public:
    /** Manifests held for gossip-tier master keys. */
    static constexpr std::size_t gossipCapacity = 64;

    /** Manifests held for ledger-tier master keys: a lookup cache. */
    static constexpr std::size_t ledgerCapacity = 512;

    /** Gossip manifests waiting, unverified, for their ephemeral key. */
    static constexpr std::size_t pendingCapacity = 64;

    /** Ceiling on remembered ephemeral key probes before they are dropped.

        A cache of negatives, so dropping it costs at most one extra ledger
        read per key.
    */
    static constexpr std::size_t probeLimit = 4096;

    /** Time over which a master key's validation activity halves. */
    static constexpr std::chrono::seconds halfLife{300};

    /** Epoch of seen_. A key is remembered for three to four of them. */
    static constexpr std::chrono::seconds seenInterval{120};

    /** Ephemeral keys recently seen signing a verified validation.

        2 KiB (16 Kibit) per generation, 4 generations: 8 KiB in all. Around
       0.2% false positives per live generation with a thousand distinct keys in
       it.
    */
    using SeenFilter = RotatingBloomFilter<std::size_t{1} << 14, 4, 4>;

private:
    struct Slot
    {
        Slot(
            ManifestSource source_,
            ManifestSource tier_,
            bool vouched_,
            double weight_)
            : source(source_), tier(tier_), vouched(vouched_), weight(weight_)
        {
        }

        /// Where the manifest held for this master key came from.
        ManifestSource source;

        /// What it is retained as; see tierOf().
        ManifestSource tier;

        /// A validator list, the config or the wallet once carried a manifest
        /// for this master key. Survives later updates from other sources.
        bool vouched;

        /** Validations seen from the held ephemeral key, decayed.

            Stored as the sum, over the times t it was seen, of
            2^((t - weightBase_) / halfLife). Every slot shares weightBase_, so
            comparing two stored weights compares their decayed values at any
            common instant, without computing a decay.

            Atomic so that a sighting is recorded under a shared lock.
            Everything else in a slot changes only under an exclusive one.
        */
        std::atomic<double> weight;
    };

    struct Pending
    {
        /// Arrival order; the lowest is replaced first.
        std::uint64_t order = 0;
        std::optional<Manifest> manifest;
    };

    beast::Journal j_;
    Stopwatch& clock_;
    std::shared_mutex mutable mutex_;

    /** Active manifests stored by master public key. */
    hash_map<PublicKey, Manifest> map_;

    /** Master public keys stored by current ephemeral public key. */
    hash_map<PublicKey, PublicKey> signingToMasterKeys_;

    /** Retention state, by master public key. In step with map_. */
    hash_map<PublicKey, Slot> slots_;

    /** Number of slots_ in each tier, indexed by ManifestSource. */
    std::array<std::size_t, 3> tierSize_{};

    /** Master keys retained as list regardless of source; set by pin(). */
    hash_set<PublicKey> pinned_;

    /** Master keys of the configured validator token and revocation. */
    hash_set<PublicKey> configured_;

    std::array<Pending, pendingCapacity> pending_;
    std::uint64_t pendingOrder_ = 0;
    std::size_t pendingSize_ = 0;

    /** Ephemeral keys already probed against the ledger, and where.

        Bounds the reads driven by incoming validations to one per key per
        ledger. Guarded by mutex_ in exclusive mode.
    */
    hash_map<PublicKey, std::uint32_t> probed_;

    /** Reference instant for Slot::weight. Moved forward by rebase(). */
    Stopwatch::time_point weightBase_;

    SeenFilter seen_{seenInterval};

    std::atomic<std::uint32_t> seq_{0};

    /** Tier a master key is retained in, given where its manifest came from.

        @pre The caller holds mutex_, shared or exclusive.
    */
    ManifestSource
    tierOf(PublicKey const& master, ManifestSource source, bool vouched) const;

    /** Weight of a single sighting at now. @pre mutex_ held. */
    double
    unit(Stopwatch::time_point now) const;

    /** Whether weightBase_ is far enough behind to need moving now.

        @pre mutex_ held.
    */
    bool
    overdue(Stopwatch::time_point now) const;

    /** Move weightBase_ toward now, rescaling every weight to match.

        @pre mutex_ held exclusively.
    */
    void
    rebase(Stopwatch::time_point now);

    /** The least active entry in a tier, other than exclude.

        @pre mutex_ held.
    */
    std::optional<std::pair<PublicKey, double>>
    coldest(ManifestSource tier, PublicKey const* exclude) const;

    /** Whether master could enter tier, competing with weight.

        A capped tier that is full admits by evicting its least active entry.
        The lookup cache always does; the gossip tier only if that entry is
        less active than weight.

        @pre mutex_ held.
    */
    bool
    admissible(ManifestSource tier, PublicKey const& master, double weight)
        const;

    /** Evict what admissible() said could be evicted.

        @pre mutex_ held exclusively.
    */
    bool
    makeRoom(ManifestSource tier, PublicKey const& master, double weight);

    /** Forget a master key entirely. @pre mutex_ held exclusively. */
    void
    erase(PublicKey const& master);

    /** Move a slot to another tier. @pre mutex_ held exclusively. */
    void
    retier(Slot& slot, ManifestSource tier);

    /** Hold gossip until its ephemeral key is seen.

        @pre mutex_ held exclusively; m names an ephemeral key.
    */
    void
    addPending(Manifest m);

    /** @pre mutex_ held. */
    bool
    hasPending(PublicKey const& signingKey) const;

    /** @pre mutex_ held exclusively. */
    std::optional<Manifest>
    takePending(PublicKey const& signingKey);

    /** Adopt a gossip entry the ledger turns out to hold verbatim.

        It is then a ledger lookup cache entry, and frees its gossip slot.

        @pre mutex_ held exclusively.
    */
    void
    adopt(Manifest const& m);

    /** The ledger object for a manifest, keyed by its master or ephemeral key.

        The one place the cache reads the ledger.
    */
    std::shared_ptr<SLE const>
    readLedger(ReadView const& view, PublicKey const& key) const;

public:
    explicit ManifestCache(
        beast::Journal j = beast::Journal(beast::Journal::getNullSink()),
        Stopwatch& clock = stopwatch());

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

    /** Returns the tier a master key is held in, if it is held.

        @par Thread Safety

        May be called concurrently
    */
    std::optional<ManifestSource>
    getTier(PublicKey const& pk) const;

    /** Add manifest to cache.

        @param m Manifest to add

        @param source Where it came from. The default is the trusted path
            taken by validator lists, the config and the wallet.

        @param view For gossip, a ledger to consult first. A master key whose
            ledger object is at the same or a later sequence -- above all, a
            revocation -- has that object ingested instead, and the gossip is
            stale. Ignored for other sources.

        @return `ManifestDisposition::accepted` if successful, or
                the disposition explaining why admission was refused

        @par Thread Safety

        May be called concurrently
    */
    ManifestDisposition
    applyManifest(
        Manifest m,
        ManifestSource source = ManifestSource::list,
        ReadView const* view = nullptr);

    /** Record a verified, current validation signed by an ephemeral key.

        Marks the key as seen for gossip admission, adds to the activity of the
        master key it belongs to, and releases a pending manifest for it.

        @param signingKey Ephemeral key that signed the validation

        @param view Passed on to applyManifest() for a released manifest

        @return the manifest released and accepted, if any, for the caller to
            relay

        @par Thread Safety

        May be called concurrently
    */
    std::optional<Manifest>
    noteValidation(PublicKey const& signingKey, ReadView const* view = nullptr);

    /** Set the master keys retained as list, whatever their source.

        Replaces any previous set. A key that leaves falls back to the tier of
        its source, which sheds its least active entries if that puts it over
        capacity. Bumps sequence() when the set actually changes, so a cached
        gossip message built from it is rebuilt.

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
        peer gossip or in a published list.

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

    /** Refresh from the ledger everything held or pinned.

        Probes every pinned and configured master key, and every master key
        held in any tier. This is what brings in a revocation, which is
        published on-ledger by master key and nowhere else, for a master key
        whose ephemeral key the cache already resolves and so never misses on.

        @return the number of manifests newly accepted

        @par Thread Safety

        May be called concurrently
    */
    std::size_t
    applyLedger(ReadView const& view);

    /** Resolve an ephemeral signing key against a manifest published on-ledger.

        The miss path of the ledger tier. applyLedger() probes a known master
        key set, which cannot help a key this node has no manifest for: the
        master key is exactly what is missing. SetManifest writes a second copy
        of every manifest keyed by its ephemeral key, so that case is one read
        rather than a search.

        Anything found is fed through applyManifest(), so an on-chain manifest
        faces the same signature check and the same staleness, revocation and
        key-reuse rules as one arriving by gossip. The answer is read back out
        of the cache rather than taken from the ledger object, because
        applyManifest() may decline it.

        A key is probed at most once per ledger, and a key that resolves is
        answered from the cache thereafter without any ledger read, until it
        is evicted.

        @param view Ledger to read from
        @param signingKey Ephemeral public key to resolve

        @return the master key now associated with signingKey, if any

        @par Thread Safety

        May be called concurrently
    */
    std::optional<PublicKey>
    applyLedgerSigningKey(ReadView const& view, PublicKey const& signingKey);

    /** Ingest the manifests listed in the ledger's manifest directory.

        Every master key with a manifest on-ledger is listed in
        keylet::manifestDir(), so unlike applyLedger() this needs no key to
        start from. Pinned, configured and held master keys are refreshed as
        applyLedger() would; any other is admitted to the ledger tier only
        while it has room, so a bulk read fills the lookup cache without
        churning it.

        The view need not be complete: one in which the directory and the
        objects it lists can be read will do. That is what lets a node read
        them during sync, before it holds the rest of the state map. The
        ledger need not be validated either: every manifest read is verified
        as from any other source, and the cache only ever moves forward, so
        the worst an unvalidated ledger can do is leave something out.

        @param view Ledger to read from
        @param maxPages Most directory pages to read; see
            forEachLedgerManifest()

        @return the number of manifests newly accepted

        @par Thread Safety

        May be called concurrently
    */
    std::size_t
    applyLedgerDirectory(
        ReadView const& view,
        std::uint64_t maxPages = std::numeric_limits<std::uint64_t>::max());

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

    /** Save list-tier manifests to database.

        Ledger-tier manifests are on the ledger already, revocations included,
        and gossip-tier ones are not worth carrying across a restart.

        @param dbCon Database connection with `ValidatorManifests` table

        @param isTrusted Function that returns true if manifest is trusted.
            Called without the cache locked, so it may call back into it.

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

        The list and gossip tiers: at most the listed validators plus
        gossipCapacity. The ledger tier is left out, since any peer that is
        not amendment blocked can read it from the ledger.

        @note Do not call ManifestCache member functions from within the
        callback. This can re-lock the mutex from the same thread, which is UB.
        @note Do not write ManifestCache member variables from
        within the callback. This can lead to data races.

        @param pf Pre-function called with the number of times f will be called

        @param f Function called for each manifest

        @par Thread Safety

        May be called concurrently
    */
    template <class PreFun, class EachFun>
    void
    for_each_gossip_manifest(PreFun&& pf, EachFun&& f) const
    {
        std::shared_lock lock{mutex_};

        pf(tierSize_[static_cast<std::size_t>(ManifestSource::list)] +
           tierSize_[static_cast<std::size_t>(ManifestSource::gossip)]);

        for (auto const& [key, slot] : slots_)
            if (slot.tier != ManifestSource::ledger)
                f(map_.find(key)->second);
    }
};

}  // namespace ripple

#endif
