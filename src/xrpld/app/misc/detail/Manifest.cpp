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

#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/ManifestLedger.h>
#include <xrpld/app/rdb/Wallet.h>
#include <xrpld/core/DatabaseCon.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Sign.h>

#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <numeric>
#include <stdexcept>
#include <vector>

namespace ripple {

std::string
to_string(Manifest const& m)
{
    auto const mk = toBase58(TokenType::NodePublic, m.masterKey);

    if (m.revoked())
        return "Revocation Manifest " + mk;

    if (!m.signingKey)
        Throw<std::runtime_error>("No SigningKey in manifest " + mk);

    return "Manifest " + mk + " (" + std::to_string(m.sequence) + ": " +
        toBase58(TokenType::NodePublic, *m.signingKey) + ")";
}

std::optional<Manifest>
deserializeManifest(Slice s, beast::Journal journal)
{
    if (s.empty())
        return std::nullopt;
    static SOTemplate const manifestFormat{
        // A manifest must include:
        // - the master public key
        {sfPublicKey, soeREQUIRED},

        // - a signature with that public key
        {sfMasterSignature, soeREQUIRED},

        // - a sequence number
        {sfSequence, soeREQUIRED},

        // It may, optionally, contain:
        // - a version number which defaults to 0
        {sfVersion, soeDEFAULT},

        // - a domain name
        {sfDomain, soeOPTIONAL},

        // - an ephemeral signing key that can be changed as necessary
        {sfSigningPubKey, soeOPTIONAL},

        // - a signature using the ephemeral signing key, if it is present
        {sfSignature, soeOPTIONAL},
    };

    try
    {
        SerialIter sit{s};
        STObject st{sit, sfGeneric};

        st.applyTemplate(manifestFormat);

        // We only understand "version 0" manifests at this time:
        if (st.isFieldPresent(sfVersion) && st.getFieldU16(sfVersion) != 0)
            return std::nullopt;

        auto const pk = st.getFieldVL(sfPublicKey);

        if (!publicKeyType(makeSlice(pk)))
            return std::nullopt;

        PublicKey const masterKey = PublicKey(makeSlice(pk));
        std::uint32_t const seq = st.getFieldU32(sfSequence);

        std::string domain;

        std::optional<PublicKey> signingKey;

        if (st.isFieldPresent(sfDomain))
        {
            auto const d = st.getFieldVL(sfDomain);

            domain.assign(reinterpret_cast<char const*>(d.data()), d.size());

            if (!isProperlyFormedTomlDomain(domain))
                return std::nullopt;
        }

        bool const hasEphemeralKey = st.isFieldPresent(sfSigningPubKey);
        bool const hasEphemeralSig = st.isFieldPresent(sfSignature);

        if (Manifest::revoked(seq))
        {
            // Revocation manifests should not specify a new signing key
            // or a signing key signature.
            if (hasEphemeralKey)
                return std::nullopt;

            if (hasEphemeralSig)
                return std::nullopt;
        }
        else
        {
            // Regular manifests should contain a signing key and an
            // associated signature.
            if (!hasEphemeralKey)
                return std::nullopt;

            if (!hasEphemeralSig)
                return std::nullopt;

            auto const spk = st.getFieldVL(sfSigningPubKey);

            if (!publicKeyType(makeSlice(spk)))
                return std::nullopt;

            signingKey.emplace(makeSlice(spk));

            // The signing and master keys can't be the same
            if (*signingKey == masterKey)
                return std::nullopt;
        }

        std::string const serialized(
            reinterpret_cast<char const*>(s.data()), s.size());

        // If the manifest is revoked, then the signingKey will be unseated
        return Manifest(serialized, masterKey, signingKey, seq, domain);
    }
    catch (std::exception const& ex)
    {
        JLOG(journal.error())
            << "Exception in " << __func__ << ": " << ex.what();
        return std::nullopt;
    }
}

// Helper macros to format manifest log messages while preserving line numbers
#define LOG_MANIFEST_ACTION(stream, action, pk, seq)                   \
    do                                                                 \
    {                                                                  \
        JLOG(stream) << "Manifest: " << action                         \
                     << ";Pk: " << toBase58(TokenType::NodePublic, pk) \
                     << ";Seq: " << seq << ";";                        \
    } while (0)

#define LOG_MANIFEST_ACTION_WITH_OLD(stream, action, pk, seq, oldSeq)    \
    do                                                                   \
    {                                                                    \
        JLOG(stream) << "Manifest: " << action                           \
                     << ";Pk: " << toBase58(TokenType::NodePublic, pk)   \
                     << ";Seq: " << seq << ";OldSeq: " << oldSeq << ";"; \
    } while (0)

bool
Manifest::verify() const
{
    STObject st(sfGeneric);
    SerialIter sit(serialized.data(), serialized.size());
    st.set(sit);

    // The manifest must either have a signing key or be revoked.  This check
    // prevents us from accessing an unseated signingKey in the next check.
    if (!revoked() && !signingKey)
        return false;

    // Signing key and signature are not required for
    // master key revocations
    if (!revoked() && !ripple::verify(st, HashPrefix::manifest, *signingKey))
        return false;

    return ripple::verify(
        st, HashPrefix::manifest, masterKey, sfMasterSignature);
}

uint256
Manifest::hash() const
{
    STObject st(sfGeneric);
    SerialIter sit(serialized.data(), serialized.size());
    st.set(sit);
    return st.getHash(HashPrefix::manifest);
}

bool
Manifest::revoked() const
{
    /*
        The maximum possible sequence number means that the master key
        has been revoked.
    */
    return revoked(sequence);
}

bool
Manifest::revoked(std::uint32_t sequence)
{
    // The maximum possible sequence number means that the master key has
    // been revoked.
    return sequence == std::numeric_limits<std::uint32_t>::max();
}

std::optional<Blob>
Manifest::getSignature() const
{
    STObject st(sfGeneric);
    SerialIter sit(serialized.data(), serialized.size());
    st.set(sit);
    if (!get(st, sfSignature))
        return std::nullopt;
    return st.getFieldVL(sfSignature);
}

Blob
Manifest::getMasterSignature() const
{
    STObject st(sfGeneric);
    SerialIter sit(serialized.data(), serialized.size());
    st.set(sit);
    return st.getFieldVL(sfMasterSignature);
}

std::optional<ValidatorToken>
loadValidatorToken(std::vector<std::string> const& blob, beast::Journal journal)
{
    try
    {
        std::string tokenStr;

        tokenStr.reserve(std::accumulate(
            blob.cbegin(),
            blob.cend(),
            std::size_t(0),
            [](std::size_t init, std::string const& s) {
                return init + s.size();
            }));

        for (auto const& line : blob)
            tokenStr += boost::algorithm::trim_copy(line);

        tokenStr = base64_decode(tokenStr);

        Json::Reader r;
        Json::Value token;

        if (r.parse(tokenStr, token))
        {
            auto const m = token.get("manifest", Json::Value{});
            auto const k = token.get("validation_secret_key", Json::Value{});

            if (m.isString() && k.isString())
            {
                auto const key = strUnHex(k.asString());

                if (key && key->size() == 32)
                    return ValidatorToken{m.asString(), makeSlice(*key)};
            }
        }

        return std::nullopt;
    }
    catch (std::exception const& ex)
    {
        JLOG(journal.error())
            << "Exception in " << __func__ << ": " << ex.what();
        return std::nullopt;
    }
}

namespace {

constexpr std::size_t
tierIndex(ManifestSource s)
{
    return static_cast<std::size_t>(s);
}

constexpr std::size_t
capacity(ManifestSource tier)
{
    switch (tier)
    {
        case ManifestSource::gossip:
            return ManifestCache::gossipCapacity;
        case ManifestSource::ledger:
            return ManifestCache::ledgerCapacity;
        default:
            return std::numeric_limits<std::size_t>::max();
    }
}

/** Half-lives after which weights are rebased when convenient. */
constexpr std::int64_t rebaseAfter = 64;

/** Half-lives after which a rebase is forced, even from a path that holds
    only a shared lock. Far below the 1023 half-lives a double can span.
*/
constexpr std::int64_t rebaseOverdue = 256;

Manifest
duplicate(Manifest const& m)
{
    return Manifest{
        m.serialized, m.masterKey, m.signingKey, m.sequence, m.domain};
}

void
addWeight(std::atomic<double>& weight, double amount)
{
    auto current = weight.load(std::memory_order_relaxed);
    while (!weight.compare_exchange_weak(
        current, current + amount, std::memory_order_relaxed))
        ;
}

/** Rebuild the manifest a ltMANIFEST object was written from.

    The object is a lossless mirror written by SetManifest::doApply, so this
    round-trip is byte-identical to the blob the master key signed and verify()
    succeeds, or the manifest is discarded. Presence matters: sfVersion is
    soeDEFAULT in the manifest format and must not be materialised.
*/
std::optional<Manifest>
manifestFromSLE(SLE const& sle, beast::Journal j)
{
    STObject st{sfGeneric};
    st.setFieldU32(sfSequence, sle.getFieldU32(sfSequence));
    st.setFieldVL(sfPublicKey, sle.getFieldVL(sfPublicKey));
    st.setFieldVL(sfMasterSignature, sle.getFieldVL(sfMasterSignature));
    for (auto const& sf :
         {std::cref(sfSigningPubKey),
          std::cref(sfSignature),
          std::cref(sfDomain)})
        if (sle.isFieldPresent(sf.get()))
            st.setFieldVL(sf.get(), sle.getFieldVL(sf.get()));
    if (sle.isFieldPresent(sfVersion))
        st.setFieldU16(sfVersion, sle.getFieldU16(sfVersion));

    return deserializeManifest(st, j);
}

}  // namespace

ManifestCache::ManifestCache(beast::Journal j, Stopwatch& clock)
    : j_(j), clock_(clock), weightBase_(clock.now())
{
}

std::optional<PublicKey>
ManifestCache::getSigningKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end() && !iter->second.revoked())
        return iter->second.signingKey;

    return pk;
}

PublicKey
ManifestCache::getMasterKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};

    if (auto const iter = signingToMasterKeys_.find(pk);
        iter != signingToMasterKeys_.end())
        return iter->second;

    return pk;
}

std::optional<std::uint32_t>
ManifestCache::getSequence(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end() && !iter->second.revoked())
        return iter->second.sequence;

    return std::nullopt;
}

std::optional<std::string>
ManifestCache::getDomain(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end() && !iter->second.revoked())
        return iter->second.domain;

    return std::nullopt;
}

std::optional<std::string>
ManifestCache::getManifest(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end() && !iter->second.revoked())
        return iter->second.serialized;

    return std::nullopt;
}

bool
ManifestCache::revoked(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end())
        return iter->second.revoked();

    return false;
}

std::optional<ManifestSource>
ManifestCache::getTier(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};

    if (auto const iter = slots_.find(pk); iter != slots_.end())
        return iter->second.tier;

    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::string>>
ManifestCache::getRawManifest(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};

    if (auto const iter = map_.find(pk); iter != map_.end())
        return std::make_pair(iter->second.sequence, iter->second.serialized);

    return std::nullopt;
}

//------------------------------------------------------------------------------

ManifestSource
ManifestCache::tierOf(
    PublicKey const& master,
    ManifestSource source,
    bool vouched) const
{
    if (vouched || source == ManifestSource::list || pinned_.contains(master) ||
        configured_.contains(master))
        return ManifestSource::list;

    return source;
}

double
ManifestCache::unit(Stopwatch::time_point now) const
{
    using seconds = std::chrono::duration<double>;
    return std::exp2(seconds(now - weightBase_) / seconds(halfLife));
}

bool
ManifestCache::overdue(Stopwatch::time_point now) const
{
    return now - weightBase_ >= rebaseOverdue * halfLife;
}

void
ManifestCache::rebase(Stopwatch::time_point now)
{
    auto const halfLives = (now - weightBase_) / halfLife;
    if (halfLives < rebaseAfter)
        return;

    // Scaling every weight by the same factor leaves their order and ratios
    // alone, which is all anything compares.
    auto const scale = std::exp2(-static_cast<double>(halfLives));
    for (auto& entry : slots_)
    {
        auto& weight = entry.second.weight;
        weight.store(
            weight.load(std::memory_order_relaxed) * scale,
            std::memory_order_relaxed);
    }

    weightBase_ += halfLives * halfLife;
}

std::optional<std::pair<PublicKey, double>>
ManifestCache::coldest(ManifestSource tier, PublicKey const* exclude) const
{
    std::optional<std::pair<PublicKey, double>> found;

    for (auto const& [key, slot] : slots_)
    {
        if (slot.tier != tier || (exclude && key == *exclude))
            continue;

        auto const weight = slot.weight.load(std::memory_order_relaxed);
        if (!found || weight < found->second)
            found.emplace(key, weight);
    }

    return found;
}

bool
ManifestCache::admissible(
    ManifestSource tier,
    PublicKey const& master,
    double weight) const
{
    if (tierSize_[tierIndex(tier)] < capacity(tier))
        return true;

    // The lookup cache always makes room: whatever it drops is read back from
    // the ledger when next wanted.
    if (tier != ManifestSource::gossip)
        return true;

    // Gossip has no such fallback, so an entry is only displaced by one that
    // is at least as active. A newcomer competes as a single fresh sighting,
    // so it can only displace an entry that has not been validating.
    auto const victim = coldest(tier, &master);
    return victim && victim->second < weight;
}

bool
ManifestCache::makeRoom(
    ManifestSource tier,
    PublicKey const& master,
    double weight)
{
    while (tierSize_[tierIndex(tier)] >= capacity(tier))
    {
        auto const victim = coldest(tier, &master);
        if (!victim ||
            (tier == ManifestSource::gossip && !(victim->second < weight)))
            return false;

        JLOG(j_.debug()) << "Manifest: Evicted;Pk: "
                         << toBase58(TokenType::NodePublic, victim->first)
                         << ";Tier: " << to_string(tier) << ";";

        erase(victim->first);
    }

    return true;
}

void
ManifestCache::erase(PublicKey const& master)
{
    auto const iter = map_.find(master);
    if (iter == map_.end())
        return;

    if (auto const& sk = iter->second.signingKey; sk && !iter->second.revoked())
    {
        if (auto const s = signingToMasterKeys_.find(*sk);
            s != signingToMasterKeys_.end() && s->second == master)
            signingToMasterKeys_.erase(s);
    }

    if (auto const slot = slots_.find(master); slot != slots_.end())
    {
        --tierSize_[tierIndex(slot->second.tier)];
        slots_.erase(slot);
    }

    map_.erase(iter);

    // What a gossip message would contain may have changed.
    ++seq_;
}

void
ManifestCache::retier(Slot& slot, ManifestSource tier)
{
    if (slot.tier == tier)
        return;

    --tierSize_[tierIndex(slot.tier)];
    ++tierSize_[tierIndex(tier)];
    slot.tier = tier;

    // Which tier an entry is in decides whether it is gossiped.
    ++seq_;
}

void
ManifestCache::addPending(Manifest m)
{
    Pending* target = nullptr;

    for (auto& entry : pending_)
    {
        // Keyed by ephemeral key, since that is what a validation names. A
        // newer manifest for a key already waiting replaces it in place.
        if (entry.manifest && entry.manifest->signingKey == m.signingKey)
        {
            // Only a manifest that verifies may displace one already waiting:
            // the ephemeral key is public, so anyone can name it.
            if (entry.manifest->sequence < m.sequence && m.verify())
                entry.manifest = std::move(m);
            return;
        }

        // An empty entry if there is one, else the one waiting longest.
        if (!target || (target->manifest && !entry.manifest) ||
            (target->manifest && entry.manifest && entry.order < target->order))
            target = &entry;
    }

    if (!target->manifest)
        ++pendingSize_;

    target->order = ++pendingOrder_;
    target->manifest = std::move(m);
}

bool
ManifestCache::hasPending(PublicKey const& signingKey) const
{
    if (pendingSize_ == 0)
        return false;

    for (auto const& entry : pending_)
        if (entry.manifest && entry.manifest->signingKey == signingKey)
            return true;

    return false;
}

std::optional<Manifest>
ManifestCache::takePending(PublicKey const& signingKey)
{
    for (auto& entry : pending_)
    {
        if (entry.manifest && entry.manifest->signingKey == signingKey)
        {
            std::optional<Manifest> taken = std::move(entry.manifest);
            entry.manifest.reset();
            --pendingSize_;
            return taken;
        }
    }

    return std::nullopt;
}

void
ManifestCache::adopt(Manifest const& m)
{
    auto const iter = map_.find(m.masterKey);
    if (iter == map_.end() || iter->second.serialized != m.serialized)
        return;

    auto& slot = slots_.find(m.masterKey)->second;
    if (slot.source != ManifestSource::gossip)
        return;

    slot.source = ManifestSource::ledger;

    auto const tier = tierOf(m.masterKey, slot.source, slot.vouched);
    if (tier != slot.tier && makeRoom(tier, m.masterKey, 0))
        retier(slot, tier);
}

std::shared_ptr<SLE const>
ManifestCache::readLedger(ReadView const& view, PublicKey const& key) const
{
    return view.read(keylet::manifest(key));
}

//------------------------------------------------------------------------------

void
ManifestCache::pin(hash_set<PublicKey> keys)
{
    std::lock_guard lock{mutex_};

    // Called every consensus round, so a convenient place to keep the weight
    // base current.
    rebase(clock_.now());

    if (keys == pinned_)
        return;

    pinned_ = std::move(keys);

    for (auto& [key, slot] : slots_)
        retier(slot, tierOf(key, slot.source, slot.vouched));

    // A key that left falls back to its source's tier, which can put that
    // tier over capacity. Nothing is being admitted in exchange, so the least
    // active go unconditionally.
    for (auto const tier : {ManifestSource::gossip, ManifestSource::ledger})
    {
        while (tierSize_[tierIndex(tier)] > capacity(tier))
        {
            auto const victim = coldest(tier, nullptr);
            if (!victim)
                break;
            erase(victim->first);
        }
    }

    // The pinned set is part of what a gossip message contains, so a change to
    // it has to invalidate any message cached against this sequence.
    ++seq_;
}

std::optional<Manifest>
ManifestCache::noteValidation(PublicKey const& signingKey, ReadView const* view)
{
    auto const now = clock_.now();

    // Before anything else, so that a manifest released below passes the
    // filter it was held back by.
    seen_.insert(signingKey, now);

    {
        std::shared_lock lock{mutex_};

        if (overdue(now))
        {
            lock.unlock();
            {
                std::lock_guard exclusive{mutex_};
                rebase(now);
            }
            lock.lock();
        }

        if (auto const iter = signingToMasterKeys_.find(signingKey);
            iter != signingToMasterKeys_.end())
        {
            if (auto const slot = slots_.find(iter->second);
                slot != slots_.end())
                addWeight(slot->second.weight, unit(now));
            return std::nullopt;
        }

        if (!hasPending(signingKey))
            return std::nullopt;
    }

    std::optional<Manifest> released;
    {
        std::lock_guard lock{mutex_};
        released = takePending(signingKey);
    }

    if (!released)
        return std::nullopt;

    auto copy = duplicate(*released);

    if (applyManifest(std::move(*released), ManifestSource::gossip, view) !=
        ManifestDisposition::accepted)
        return std::nullopt;

    return copy;
}

std::size_t
ManifestCache::applyLedger(
    ReadView const& view,
    hash_set<PublicKey> const& masterKeys)
{
    std::size_t accepted = 0;

    for (auto const& pk : masterKeys)
    {
        auto const sle = readLedger(view, pk);
        if (!sle)
            continue;

        // Cheap reject before rebuilding: the signature check is the expensive
        // part, and most rounds nothing has changed. An equal sequence is
        // still wanted if the copy held came by gossip: it may be the same
        // manifest, which then belongs in the lookup cache instead.
        auto const wanted = [&]() {
            std::shared_lock lock{mutex_};
            auto const iter = map_.find(pk);
            if (iter == map_.end())
                return true;
            auto const onLedger = sle->getFieldU32(sfSequence);
            return iter->second.sequence < onLedger ||
                (iter->second.sequence == onLedger &&
                 slots_.find(pk)->second.source == ManifestSource::gossip);
        }();

        if (!wanted)
            continue;

        if (auto mo = manifestFromSLE(*sle, j_); mo &&
            applyManifest(std::move(*mo), ManifestSource::ledger) ==
                ManifestDisposition::accepted)
            ++accepted;
    }

    return accepted;
}

std::size_t
ManifestCache::applyLedger(ReadView const& view)
{
    hash_set<PublicKey> keys;

    {
        std::shared_lock lock{mutex_};
        keys.reserve(map_.size() + pinned_.size() + configured_.size());
        keys.insert(pinned_.begin(), pinned_.end());
        keys.insert(configured_.begin(), configured_.end());
        for (auto const& entry : map_)
            keys.insert(entry.first);
    }

    return applyLedger(view, keys);
}

std::optional<PublicKey>
ManifestCache::applyLedgerSigningKey(
    ReadView const& view,
    PublicKey const& signingKey)
{
    auto const held = [this, &signingKey]() -> std::optional<PublicKey> {
        std::shared_lock lock{mutex_};

        if (auto const iter = signingToMasterKeys_.find(signingKey);
            iter != signingToMasterKeys_.end())
            return iter->second;

        return std::nullopt;
    };

    // A manifest for this key may have arrived by gossip or in a published
    // list while the caller was deciding to ask.
    if (auto const known = held())
        return known;

    {
        std::lock_guard lock{mutex_};

        if (probed_.size() >= probeLimit)
            probed_.clear();

        // One read per key per ledger. Reaching here already cost the sender a
        // signature and this node a verification, so the read is not the
        // cheapest thing an unknown key can ask for; the cap is to stop
        // repeating it, not to stop anyone.
        auto const seq = view.info().seq;
        auto const [iter, inserted] = probed_.try_emplace(signingKey, seq);
        if (!inserted && iter->second == seq)
            return std::nullopt;

        iter->second = seq;
    }

    auto const sle = readLedger(view, signingKey);
    if (!sle)
        return std::nullopt;

    // Every manifest is written at both its master and its ephemeral keylet,
    // so an object here is either the manifest naming signingKey as its
    // ephemeral key -- the case worth having -- or the manifest of a master
    // key that is what was asked about. Ingesting either is correct, and
    // applyManifest() verifies both signatures, so nothing found here can
    // assert a binding its key holder did not sign for.
    if (auto mo = manifestFromSLE(*sle, j_))
        applyManifest(std::move(*mo), ManifestSource::ledger);

    // Only a signing key resolves: a master key is its own master.
    auto const resolved = held();

    // Resolved, the key is answered from the cache until it is evicted, and
    // a miss after that should read again rather than find a stale probe.
    if (resolved)
    {
        std::lock_guard lock{mutex_};
        probed_.erase(signingKey);
    }

    return resolved;
}

std::size_t
ManifestCache::applyLedgerDirectory(
    ReadView const& view,
    std::uint64_t maxPages)
{
    std::size_t accepted = 0;

    auto const visit = [&](std::shared_ptr<SLE const> const& sle) {
        auto const raw = sle->getFieldVL(sfPublicKey);
        if (!publicKeyType(makeSlice(raw)))
            return;

        PublicKey const master{makeSlice(raw)};

        // The directory lists master objects. Anything else listed there is
        // not this function's to interpret.
        if (sle->key() != keylet::manifest(master).key)
            return;

        auto const onLedger = sle->getFieldU32(sfSequence);

        bool const wanted = [&]() {
            std::shared_lock lock{mutex_};

            // As in applyLedger(): newer, or the same manifest held from
            // gossip, which then belongs in the lookup cache instead.
            if (auto const iter = map_.find(master); iter != map_.end())
                return iter->second.sequence < onLedger ||
                    (iter->second.sequence == onLedger &&
                     slots_.find(master)->second.source ==
                         ManifestSource::gossip);

            if (pinned_.contains(master) || configured_.contains(master))
                return true;

            return tierSize_[tierIndex(ManifestSource::ledger)] <
                ledgerCapacity;
        }();

        if (!wanted)
            return;

        if (auto mo = manifestFromSLE(*sle, j_); mo &&
            applyManifest(std::move(*mo), ManifestSource::ledger) ==
                ManifestDisposition::accepted)
            ++accepted;
    };

    forEachLedgerManifest(view, visit, maxPages);

    return accepted;
}

ManifestDisposition
ManifestCache::applyManifest(
    Manifest m,
    ManifestSource source,
    ReadView const* view)
{
    auto const now = clock_.now();

    // How this manifest would be held, as worked out by check().
    struct Admission
    {
        /// Source recorded for it. A revocation never lowers this: it can only
        /// take trust away, so it is accepted from anywhere for a master key
        /// already held, and leaves that key where it was.
        ManifestSource source = ManifestSource::gossip;
        bool vouched = false;
        ManifestSource tier = ManifestSource::gossip;
        /// It takes a place in a tier it does not already occupy.
        bool entering = false;
        /// What it competes for that place with.
        double weight = 0;
    };

    // The conditions that need no write. Run under a shared lock before the
    // exclusive one is taken, so the expensive parts -- the signature, and for
    // gossip the ledger read between the two runs -- do not block readers;
    // and again under the exclusive lock, because the collections may have
    // been written in between. Held manifests that conflict with this one over
    // a key but rank below it are collected in `evict` rather than refusing
    // this one.
    auto check = [this, &m, source, now](
                     bool checkSignature,
                     auto const& lock,
                     Admission& admission,
                     std::vector<PublicKey>& evict)
        -> std::optional<ManifestDisposition> {
        XRPL_ASSERT(
            lock.owns_lock(),
            "ripple::ManifestCache::applyManifest::check : locked");
        (void)lock;  // not used. parameter is present to ensure the mutex is
                     // locked when the lambda is called.

        auto const iter = map_.find(m.masterKey);
        Slot const* const slot =
            iter == map_.end() ? nullptr : &slots_.find(m.masterKey)->second;

        if (slot && m.sequence <= iter->second.sequence)
        {
            // We received a manifest whose sequence number is not strictly
            // greater than the one we already know about. This can happen in
            // several cases including when we receive manifests from a peer who
            // doesn't have the latest data.
            if (auto stream = j_.debug())
                LOG_MANIFEST_ACTION_WITH_OLD(
                    stream,
                    "Stale",
                    m.masterKey,
                    m.sequence,
                    iter->second.sequence);
            return ManifestDisposition::stale;
        }

        bool const revoked = m.revoked();

        admission.source =
            (slot && revoked && tierIndex(source) < tierIndex(slot->source))
            ? slot->source
            : source;
        admission.vouched =
            (slot && slot->vouched) || source == ManifestSource::list;
        admission.tier =
            tierOf(m.masterKey, admission.source, admission.vouched);
        admission.entering = !slot || slot->tier != admission.tier;
        admission.weight = unit(now);
        if (slot)
            admission.weight = std::max(
                admission.weight, slot->weight.load(std::memory_order_relaxed));

        if (admission.entering && admission.tier == ManifestSource::gossip)
        {
            // The gate on the legacy transport, and cheap, so it comes before
            // the signature: a master key not already held from gossip is let
            // in on the strength of its ephemeral key having just signed a
            // validation that verified. A revocation names no ephemeral key,
            // and for a master key not held there is nothing for it to revoke.
            if (revoked || !m.signingKey || !seen_.contains(*m.signingKey, now))
            {
                if (auto stream = j_.debug())
                    LOG_MANIFEST_ACTION(
                        stream, "Unseen", m.masterKey, m.sequence);
                return ManifestDisposition::unseen;
            }

            if (!admissible(admission.tier, m.masterKey, admission.weight))
            {
                if (auto stream = j_.debug())
                    LOG_MANIFEST_ACTION(
                        stream, "Full", m.masterKey, m.sequence);
                return ManifestDisposition::full;
            }
        }

        if (checkSignature && !m.verify())
        {
            if (auto stream = j_.warn())
                LOG_MANIFEST_ACTION(stream, "Invalid", m.masterKey, m.sequence);
            return ManifestDisposition::invalid;
        }

        // If the master key associated with a manifest is or might be
        // compromised and is, therefore, no longer trustworthy.
        //
        // A manifest revocation essentially marks a manifest as compromised. By
        // setting the sequence number to the highest value possible, the
        // manifest is effectively neutered and cannot be superseded by a forged
        // one.
        if (auto stream = j_.warn(); stream && revoked && checkSignature)
            LOG_MANIFEST_ACTION(stream, "Revoked", m.masterKey, m.sequence);

        // A held manifest in a lower tier than this one would be loses a key
        // conflict; anything at or above it wins it, as before.
        auto const outranks = [this, &admission](PublicKey const& other) {
            auto const s = slots_.find(other);
            return s != slots_.end() &&
                tierIndex(s->second.tier) < tierIndex(admission.tier);
        };

        // Sanity check: the master key of this manifest should not be used as
        // the ephemeral key of another manifest:
        if (auto const x = signingToMasterKeys_.find(m.masterKey);
            x != signingToMasterKeys_.end())
        {
            if (x->second == m.masterKey || !outranks(x->second))
            {
                JLOG(j_.warn())
                    << to_string(m)
                    << ": Master key already used as ephemeral key for "
                    << toBase58(TokenType::NodePublic, x->second);

                return ManifestDisposition::badMasterKey;
            }

            evict.push_back(x->second);
        }

        if (!revoked)
        {
            if (!m.signingKey)
            {
                JLOG(j_.warn()) << to_string(m)
                                << ": is not revoked and the manifest has no "
                                   "signing key. Hence, the manifest is "
                                   "invalid";
                return ManifestDisposition::invalid;
            }

            // Sanity check: the ephemeral key of this manifest should not be
            // used as the master or ephemeral key of another manifest:
            if (auto const x = signingToMasterKeys_.find(*m.signingKey);
                x != signingToMasterKeys_.end())
            {
                if (x->second == m.masterKey || !outranks(x->second))
                {
                    JLOG(j_.warn())
                        << to_string(m)
                        << ": Ephemeral key already used as ephemeral key for "
                        << toBase58(TokenType::NodePublic, x->second);

                    return ManifestDisposition::badEphemeralKey;
                }

                evict.push_back(x->second);
            }

            if (auto const x = map_.find(*m.signingKey); x != map_.end())
            {
                if (!outranks(x->first))
                {
                    JLOG(j_.warn()) << to_string(m)
                                    << ": Ephemeral key used as master key for "
                                    << to_string(x->second);

                    return ManifestDisposition::badEphemeralKey;
                }

                evict.push_back(x->first);
            }
        }

        return std::nullopt;
    };

    // Gossip held back by the filter waits for its ephemeral key, unverified:
    // checking it now would let anyone spend this node's time on keys that
    // never validate.
    auto const held = [this, source](Manifest& mm) {
        if (source == ManifestSource::gossip && mm.signingKey && !mm.revoked())
            addPending(std::move(mm));
        return ManifestDisposition::unseen;
    };

    {
        Admission admission;
        std::vector<PublicKey> evict;
        std::shared_lock sl{mutex_};
        if (auto const d = check(false, sl, admission, evict))
        {
            sl.unlock();

            if (*d == ManifestDisposition::unseen)
            {
                std::lock_guard lock{mutex_};
                return held(m);
            }

            // The ledger confirming verbatim what gossip delivered earlier.
            if (*d == ManifestDisposition::stale &&
                source == ManifestSource::ledger)
            {
                std::lock_guard lock{mutex_};
                adopt(m);
            }

            return *d;
        }
    }

    // The ledger outranks gossip. Before taking a peer's word for a master
    // key, see whether the ledger holds its manifest at the same or a later
    // sequence: above all a revocation, which a validator publishes on-ledger
    // and which the cache may never have had cause to read. If so the ledger's
    // copy goes in, and this is stale. A ledger object that fails to verify is
    // ignored rather than allowed to block gossip.
    if (source == ManifestSource::gossip && view)
    {
        if (auto const sle = readLedger(*view, m.masterKey);
            sle && sle->getFieldU32(sfSequence) >= m.sequence)
        {
            if (auto mo = manifestFromSLE(*sle, j_))
            {
                auto const r =
                    applyManifest(std::move(*mo), ManifestSource::ledger);
                if (r == ManifestDisposition::accepted ||
                    r == ManifestDisposition::stale)
                    return ManifestDisposition::stale;
            }
        }
    }

    {
        Admission admission;
        std::vector<PublicKey> evict;
        std::shared_lock sl{mutex_};
        if (auto const d = check(true, sl, admission, evict))
        {
            sl.unlock();
            if (*d == ManifestDisposition::unseen)
            {
                std::lock_guard lock{mutex_};
                return held(m);
            }
            return *d;
        }
    }

    std::unique_lock sl{mutex_};

    // Since we released the previously held read lock, it's possible that the
    // collections have been written to. This means we need to run `check`
    // again. This re-does work, but it is relatively inexpensive to run, and
    // doing it this way allows us to run it under a `shared_lock` above.
    // Note, the signature has already been checked above, so it doesn't need
    // to happen again (signature checks are somewhat expensive).
    // Note: It's a mistake to use an upgradable lock. This is a recipe for
    // deadlock.
    Admission admission;
    std::vector<PublicKey> evict;
    if (auto const d = check(false, sl, admission, evict))
    {
        if (*d == ManifestDisposition::unseen)
            return held(m);
        return *d;
    }

    for (auto const& key : evict)
    {
        JLOG(j_.info()) << to_string(m) << ": supersedes conflicting "
                        << toBase58(TokenType::NodePublic, key);
        erase(key);
    }

    if (admission.entering &&
        !makeRoom(admission.tier, m.masterKey, admission.weight))
        return ManifestDisposition::full;

    bool const revoked = m.revoked();
    auto const iter = map_.find(m.masterKey);

    // This is the first manifest we are seeing for a master key.
    if (iter == map_.end())
    {
        if (auto stream = j_.info())
            LOG_MANIFEST_ACTION(stream, "AcceptedNew", m.masterKey, m.sequence);

        if (!revoked)
            signingToMasterKeys_.emplace(*m.signingKey, m.masterKey);

        // A newcomer starts as a single sighting at now.
        slots_.try_emplace(
            m.masterKey,
            admission.source,
            admission.tier,
            admission.vouched,
            unit(now));
        ++tierSize_[tierIndex(admission.tier)];

        auto masterKey = m.masterKey;
        map_.emplace(std::move(masterKey), std::move(m));

        // Increment sequence to invalidate cached manifest messages
        seq_++;

        return ManifestDisposition::accepted;
    }

    // An ephemeral key was revoked and superseded by a new key. This is
    // expected, but should happen infrequently.
    if (auto stream = j_.info())
        LOG_MANIFEST_ACTION_WITH_OLD(
            stream,
            "AcceptedUpdate",
            m.masterKey,
            m.sequence,
            iter->second.sequence);

    if (auto const& old = iter->second.signingKey;
        old && !iter->second.revoked())
        signingToMasterKeys_.erase(*old);

    if (!revoked)
        signingToMasterKeys_.emplace(*m.signingKey, m.masterKey);

    // The activity recorded for the master key carries over: the new
    // ephemeral key adds to it as it validates.
    auto& slot = slots_.find(m.masterKey)->second;
    slot.source = admission.source;
    slot.vouched = admission.vouched;
    retier(slot, admission.tier);

    iter->second = std::move(m);

    // Something has changed. Keep track of it.
    seq_++;

    return ManifestDisposition::accepted;
}

void
ManifestCache::load(DatabaseCon& dbCon, std::string const& dbTable)
{
    auto db = dbCon.checkoutDb();
    ripple::getManifests(*db, dbTable, *this, j_);
}

bool
ManifestCache::load(
    DatabaseCon& dbCon,
    std::string const& dbTable,
    std::string const& configManifest,
    std::vector<std::string> const& configRevocation)
{
    load(dbCon, dbTable);

    if (!configManifest.empty())
    {
        auto mo = deserializeManifest(base64_decode(configManifest));
        if (!mo)
        {
            JLOG(j_.error()) << "Malformed validator_token in config";
            return false;
        }

        if (mo->revoked())
        {
            JLOG(j_.warn()) << "Configured manifest revokes public key";
        }

        {
            std::lock_guard lock{mutex_};
            configured_.insert(mo->masterKey);
        }

        if (applyManifest(std::move(*mo)) == ManifestDisposition::invalid)
        {
            JLOG(j_.error()) << "Manifest in config was rejected";
            return false;
        }
    }

    if (!configRevocation.empty())
    {
        std::string revocationStr;
        revocationStr.reserve(std::accumulate(
            configRevocation.cbegin(),
            configRevocation.cend(),
            std::size_t(0),
            [](std::size_t init, std::string const& s) {
                return init + s.size();
            }));

        for (auto const& line : configRevocation)
            revocationStr += boost::algorithm::trim_copy(line);

        auto mo = deserializeManifest(base64_decode(revocationStr));

        if (mo && mo->revoked())
        {
            std::lock_guard lock{mutex_};
            configured_.insert(mo->masterKey);
        }

        if (!mo || !mo->revoked() ||
            applyManifest(std::move(*mo)) == ManifestDisposition::invalid)
        {
            JLOG(j_.error()) << "Invalid validator key revocation in config";
            return false;
        }
    }

    return true;
}

void
ManifestCache::save(
    DatabaseCon& dbCon,
    std::string const& dbTable,
    std::function<bool(PublicKey const&)> const& isTrusted)
{
    // Copied out first so isTrusted, which may call back into this cache,
    // runs without the lock held.
    std::vector<Manifest> listed;
    {
        std::shared_lock lock{mutex_};
        listed.reserve(tierSize_[tierIndex(ManifestSource::list)]);
        for (auto const& [key, slot] : slots_)
            if (slot.tier == ManifestSource::list)
                listed.push_back(duplicate(map_.find(key)->second));
    }

    auto db = dbCon.checkoutDb();
    saveManifests(*db, dbTable, isTrusted, listed, j_);
}

// Clean up macros to avoid namespace pollution
#undef LOG_MANIFEST_ACTION
#undef LOG_MANIFEST_ACTION_WITH_OLD

}  // namespace ripple
