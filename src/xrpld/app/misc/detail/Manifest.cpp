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

#include <numeric>
#include <stdexcept>

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

std::optional<PublicKey>
ManifestCache::getSigningKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end() && !iter->second.revoked())
    {
        return iter->second.signingKey;
    }

    return pk;
}

PublicKey
ManifestCache::getMasterKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};

    if (auto const iter = signingToMasterKeys_.find(pk);
        iter != signingToMasterKeys_.end())
    {
        return iter->second;
    }

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
    {
        return iter->second.serialized;
    }

    return std::nullopt;
}

bool
ManifestCache::revoked(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);

    if (iter != map_.end())
    {
        return iter->second.revoked();
    }

    return false;
}

std::optional<std::pair<std::uint32_t, std::string>>
ManifestCache::getRawManifest(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};

    if (auto const iter = map_.find(pk); iter != map_.end())
    {
        return std::make_pair(iter->second.sequence, iter->second.serialized);
    }

    return std::nullopt;
}

void
ManifestCache::pin(hash_set<PublicKey> keys)
{
    hash_set<PublicKey> added;
    {
        std::shared_lock lock{mutex_};
        for (auto const& key : keys)
            if (!pinned_.contains(key))
                added.insert(key);
    }
    // Publisher membership arrives after wallet startup. Recover its saved
    // high waters before exposing the new gossip set or admitting the list's
    // possibly older embedded manifests. No database lock spans cache writes.
    restoreListed(added);

    hash_map<PublicKey, Manifest> departing;
    DatabaseCon* wallet;
    {
        std::lock_guard lock{mutex_};
        if (keys == pinned_ && pendingSave_.empty())
            return;
        wallet = wallet_;
        if (wallet)
            for (auto const& key : pinned_)
                if (!keys.contains(key) && !configured_.contains(key))
                    if (auto const it = map_.find(key); it != map_.end())
                        pendingSave_.insert_or_assign(key, it->second.sequence);
        for (auto const& [key, sequence] : pendingSave_)
            if (auto const it = map_.find(key); it != map_.end())
            {
                auto const& m = it->second;
                departing.emplace(
                    key,
                    Manifest{
                        m.serialized,
                        m.masterKey,
                        m.signingKey,
                        m.sequence,
                        m.domain});
            }
        if (keys != pinned_)
        {
            pinned_ = std::move(keys);
            ++seq_;
        }
    }
    // Capture history as gossip eligibility is withdrawn. Publisher/ledger
    // updates may not yet have reached SQLite, and shutdown now sees this
    // identity as unlisted. No cache lock is held during the database write.
    if (wallet && !departing.empty())
    {
        try
        {
            auto db = wallet->checkoutDb();
            saveManifests(
                *db,
                "ValidatorManifests",
                [](PublicKey const&) { return true; },
                departing,
                j_,
                true);
        }
        catch (soci::soci_error const& e)
        {
            // List changes also run on consensus jobs. Keep the pending save
            // for retry instead of letting a wallet write failure escape.
            JLOG(j_.error())
                << "Failed to save departing validator manifests: " << e.what();
            return;
        }
        std::lock_guard lock{mutex_};
        for (auto const& [key, manifest] : departing)
            if (auto const it = pendingSave_.find(key);
                it != pendingSave_.end() && it->second <= manifest.sequence)
                pendingSave_.erase(it);
    }
}

namespace {

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

std::size_t
ManifestCache::applyLedger(
    ReadView const& view,
    hash_set<PublicKey> const& masterKeys)
{
    std::size_t accepted = 0;

    for (auto const& pk : masterKeys)
    {
        auto const sle = view.read(keylet::manifest(pk));
        if (!sle)
            continue;

        // Cheap reject before rebuilding: applyManifest() would call this
        // stale anyway, and the signature check is the expensive part.
        if (auto const seq = getSequence(pk);
            seq && *seq >= sle->getFieldU32(sfSequence))
            continue;

        if (auto mo = manifestFromSLE(*sle, j_); mo &&
            applyManifest(std::move(*mo)) == ManifestDisposition::accepted)
            ++accepted;
    }

    return accepted;
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
        {
            return iter->second;
        }

        return std::nullopt;
    };

    // A manifest for this key may have arrived by gossip or in a published
    // list while the caller was deciding to ask.
    if (auto const known = held())
        return known;

    {
        std::lock_guard lock{mutex_};

        auto const seq = view.info().seq;
        // A delayed job holding an older view must not reset a newer budget.
        if (seq < probedLedger_)
            return std::nullopt;
        if (probedLedger_ != seq)
        {
            probed_.clear();
            probedLedger_ = seq;
        }

        // Do not clear a full negative cache: cycling keys would reset the
        // budget and allow unlimited reads during the same ledger.
        if (probed_.size() >= probeLimit)
            return std::nullopt;
        if (!probed_.insert(signingKey).second)
            return std::nullopt;
    }

    auto const sle = view.read(keylet::manifest(signingKey));
    if (!sle)
        return std::nullopt;

    // Every manifest is written at both its master and its ephemeral keylet,
    // so an object here is either the manifest naming signingKey as its
    // ephemeral key -- the case worth having -- or the manifest of a master
    // key that is what was asked about. Ingesting either is correct, and
    // applyManifest() verifies both signatures, so nothing found here can
    // assert a binding its key holder did not sign for.
    if (auto mo = manifestFromSLE(*sle, j_))
        applyManifest(std::move(*mo));

    // Only a signing key resolves: a master key is its own master.
    return held();
}

ManifestDisposition
ManifestCache::applyManifest(Manifest m)
{
    return applyManifest(std::move(m), Admission::normal);
}

ManifestDisposition
ManifestCache::applyGossipManifest(Manifest m)
{
    return applyManifest(std::move(m), Admission::gossip);
}

bool
ManifestCache::isGossipCandidate(Manifest const& m) const
{
    std::shared_lock lock{mutex_};
    if (!pinned_.contains(m.masterKey) && !configured_.contains(m.masterKey))
        return false;
    auto const it = map_.find(m.masterKey);
    return it == map_.end() || m.sequence > it->second.sequence;
}

ManifestDisposition
ManifestCache::applyManifest(Manifest m, Admission admission)
{
    // Check the manifest against the conditions that do not require a
    // `unique_lock` (write lock) on the `mutex_`. Since the signature can be
    // relatively expensive, the `checkSignature` parameter determines if the
    // signature should be checked. Since `prewriteCheck` is run twice (see
    // comment below), `checkSignature` only needs to be set to true on the
    // first run.
    auto prewriteCheck =
        [this, &m, admission](
            auto const& iter,
            bool checkSignature,
            auto const& lock) -> std::optional<ManifestDisposition> {
        XRPL_ASSERT(
            lock.owns_lock(),
            "ripple::ManifestCache::applyManifest::prewriteCheck : locked");
        (void)lock;  // not used. parameter is present to ensure the mutex is
                     // locked when the lambda is called.
        auto const listed =
            pinned_.contains(m.masterKey) || configured_.contains(m.masterKey);
        // A cached ledger result is not permission to accept its gossip.
        // This also rejects unlisted rotations/revocations without crypto.
        if (admission == Admission::gossip && !listed)
            return ManifestDisposition::unlisted;

        if (iter != map_.end() && m.sequence <= iter->second.sequence)
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

        // Keep admitted history instead of cycling it through an LRU. This
        // cheap gate runs under both locks, before crypto on the first pass.
        if (admission != Admission::listedHistory && iter == map_.end() &&
            map_.size() >= cacheLimit_ && !listed)
            return ManifestDisposition::full;

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
        bool const revoked = m.revoked();

        if (auto stream = j_.warn(); stream && revoked)
            LOG_MANIFEST_ACTION(stream, "Revoked", m.masterKey, m.sequence);

        // Sanity check: the master key of this manifest should not be used as
        // the ephemeral key of another manifest:
        if (auto const x = signingToMasterKeys_.find(m.masterKey);
            x != signingToMasterKeys_.end())
        {
            JLOG(j_.warn()) << to_string(m)
                            << ": Master key already used as ephemeral key for "
                            << toBase58(TokenType::NodePublic, x->second);

            return ManifestDisposition::badMasterKey;
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
                JLOG(j_.warn())
                    << to_string(m)
                    << ": Ephemeral key already used as ephemeral key for "
                    << toBase58(TokenType::NodePublic, x->second);

                return ManifestDisposition::badEphemeralKey;
            }

            if (auto const x = map_.find(*m.signingKey); x != map_.end())
            {
                JLOG(j_.warn())
                    << to_string(m) << ": Ephemeral key used as master key for "
                    << to_string(x->second);

                return ManifestDisposition::badEphemeralKey;
            }
        }

        return std::nullopt;
    };

    {
        std::shared_lock sl{mutex_};
        if (auto d =
                prewriteCheck(map_.find(m.masterKey), /*checkSig*/ true, sl))
            return *d;
    }

    std::unique_lock sl{mutex_};
    auto const iter = map_.find(m.masterKey);
    // Since we released the previously held read lock, it's possible that the
    // collections have been written to. This means we need to run
    // `prewriteCheck` again. This re-does work, but `prewriteCheck` is
    // relatively inexpensive to run, and doing it this way allows us to run
    // `prewriteCheck` under a `shared_lock` above.
    // Note, the signature has already been checked above, so it
    // doesn't need to happen again (signature checks are somewhat expensive).
    // Note: It's a mistake to use an upgradable lock. This is a recipe for
    // deadlock.
    if (auto d = prewriteCheck(iter, /*checkSig*/ false, sl))
        return *d;

    bool const revoked = m.revoked();
    // This is the first manifest we are seeing for a master key.
    if (iter == map_.end())
    {
        if (auto stream = j_.info())
            LOG_MANIFEST_ACTION(stream, "AcceptedNew", m.masterKey, m.sequence);

        if (!revoked)
            signingToMasterKeys_.emplace(*m.signingKey, m.masterKey);

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

    signingToMasterKeys_.erase(*iter->second.signingKey);

    if (!revoked)
        signingToMasterKeys_.emplace(*m.signingKey, m.masterKey);

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

void
ManifestCache::loadListed(DatabaseCon& dbCon)
{
    hash_set<PublicKey> keys;
    {
        std::unique_lock lock{mutex_};
        wallet_ = &dbCon;
        keys = pinned_;
        keys.insert(configured_.begin(), configured_.end());
    }
    restoreListed(keys);
}

void
ManifestCache::restoreListed(hash_set<PublicKey> const& keys)
{
    DatabaseCon* wallet;
    {
        std::shared_lock lock{mutex_};
        wallet = wallet_;
    }
    if (!wallet || keys.empty())
        return;

    auto restored = [&]() {
        auto db = wallet->checkoutDb();
        return getManifestsForKeys(*db, "ValidatorManifests", keys, j_);
    }();
    for (auto& [key, manifest] : restored)
        applyManifest(std::move(manifest), Admission::listedHistory);
}

bool
ManifestCache::loadConfig(
    std::string const& configManifest,
    std::vector<std::string> const& configRevocation)
{
    if (!configManifest.empty())
    {
        auto mo = deserializeManifest(base64_decode(configManifest));
        if (!mo || !mo->verify())
        {
            JLOG(j_.error()) << "Malformed validator_token in config";
            return false;
        }

        if (mo->revoked())
        {
            JLOG(j_.warn()) << "Configured manifest revokes public key";
        }

        {
            std::unique_lock lock{mutex_};
            if (configured_.insert(mo->masterKey).second)
                ++seq_;
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

        if (!mo || !mo->revoked() || !mo->verify())
        {
            JLOG(j_.error()) << "Invalid validator key revocation in config";
            return false;
        }
        {
            std::unique_lock lock{mutex_};
            if (configured_.insert(mo->masterKey).second)
                ++seq_;
        }
        if (applyManifest(std::move(*mo)) == ManifestDisposition::invalid)
            return false;
    }

    return true;
}

void
ManifestCache::save(
    DatabaseCon& dbCon,
    std::string const& dbTable,
    std::function<bool(PublicKey const&)> const& isTrusted)
{
    std::shared_lock lock{mutex_};
    auto db = dbCon.checkoutDb();

    saveManifests(
        *db,
        dbTable,
        [this, &isTrusted](PublicKey const& key) {
            // Membership is already mirrored here. Do not take ValidatorList's
            // lock while holding the cache lock (pin() takes them in reverse).
            return wallet_ ? pinned_.contains(key) ||
                    configured_.contains(key) || pendingSave_.contains(key)
                           : isTrusted(key);
        },
        map_,
        j_,
        wallet_ != nullptr);
}

// Clean up macros to avoid namespace pollution
#undef LOG_MANIFEST_ACTION
#undef LOG_MANIFEST_ACTION_WITH_OLD

}  // namespace ripple
