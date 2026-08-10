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
#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/random.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Sign.h>

#include <boost/algorithm/string/trim.hpp>

#include <iterator>
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

    if (s.size() > kMaxManifestBytes)
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
    auto const authoritative = map_.find(pk);
    if (authoritative != map_.end() && !authoritative->second.revoked())
        return authoritative->second.signingKey;

    auto const candidate = publisherCandidates_.find(pk);
    if (candidate != publisherCandidates_.end() && !candidate->second.revoked())
        return candidate->second.signingKey;

    return pk;
}

PublicKey
ManifestCache::getMasterKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};

    if (auto const authoritative = signingToMasterKeys_.find(pk);
        authoritative != signingToMasterKeys_.end())
        return authoritative->second;

    if (auto const candidate = candidateSigningToMasterKeys_.find(pk);
        candidate != candidateSigningToMasterKeys_.end())
        return candidate->second;

    return pk;
}

PublicKey
ManifestCache::getAuthoritativeMasterKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    if (auto const iter = signingToMasterKeys_.find(pk);
        iter != signingToMasterKeys_.end())
        return iter->second;
    return pk;
}

std::optional<PublicKey>
ManifestCache::getAuthoritativeSigningKey(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);
    if (iter != map_.end() && !iter->second.revoked())
        return iter->second.signingKey;
    return pk;
}

bool
ManifestCache::authoritativeRevoked(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const iter = map_.find(pk);
    return iter != map_.end() && iter->second.revoked();
}

std::optional<std::uint32_t>
ManifestCache::getSequence(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const authoritative = map_.find(pk);
    if (authoritative != map_.end() && !authoritative->second.revoked())
        return authoritative->second.sequence;

    auto const candidate = publisherCandidates_.find(pk);
    if (candidate != publisherCandidates_.end() && !candidate->second.revoked())
        return candidate->second.sequence;

    return std::nullopt;
}

std::optional<std::string>
ManifestCache::getDomain(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const authoritative = map_.find(pk);
    if (authoritative != map_.end() && !authoritative->second.revoked())
        return authoritative->second.domain;

    auto const candidate = publisherCandidates_.find(pk);
    if (candidate != publisherCandidates_.end() && !candidate->second.revoked())
        return candidate->second.domain;

    return std::nullopt;
}

std::optional<std::string>
ManifestCache::getManifest(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const authoritative = map_.find(pk);
    if (authoritative != map_.end() && !authoritative->second.revoked())
        return authoritative->second.serialized;

    auto const candidate = publisherCandidates_.find(pk);
    if (candidate != publisherCandidates_.end() && !candidate->second.revoked())
        return candidate->second.serialized;

    return std::nullopt;
}

bool
ManifestCache::revoked(PublicKey const& pk) const
{
    std::shared_lock lock{mutex_};
    auto const authoritative = map_.find(pk);
    if (authoritative != map_.end())
        return authoritative->second.revoked();

    auto const candidate = publisherCandidates_.find(pk);
    if (candidate != publisherCandidates_.end())
        return candidate->second.revoked();

    return false;
}

ManifestDisposition
ManifestCache::applyManifest(Manifest m, ManifestRateLimitCapPolicy const cap)
{
    return applyManifestImpl(std::move(m), cap, nullptr);
}

ManifestDisposition
ManifestCache::applyManifestWithEviction(
    Manifest m,
    hash_set<PublicKey> const& currentValidationKeys)
{
    return applyManifestImpl(
        std::move(m),
        ManifestRateLimitCapPolicy::Capped,
        &currentValidationKeys);
}

void
ManifestCache::replacePublisherCandidates(
    std::vector<Manifest> candidates,
    hash_set<PublicKey> const& listedMasterKeys)
{
    candidates.erase(
        std::remove_if(
            candidates.begin(),
            candidates.end(),
            [](Manifest const& m) { return !m.verify(); }),
        candidates.end());
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](Manifest const& lhs, Manifest const& rhs) {
            if (lhs.masterKey != rhs.masterKey)
                return lhs.masterKey < rhs.masterKey;
            if (lhs.sequence != rhs.sequence)
                return lhs.sequence > rhs.sequence;
            return lhs.serialized < rhs.serialized;
        });

    hash_map<PublicKey, Manifest> coalesced;
    for (std::size_t i = 0; i < candidates.size();)
    {
        auto const master = candidates[i].masterKey;
        auto const sequence = candidates[i].sequence;
        std::size_t next = i + 1;
        while (next < candidates.size() && candidates[next].masterKey == master)
            ++next;

        bool sameSequenceConflict = false;
        for (auto j = i + 1; j < next && candidates[j].sequence == sequence;
             ++j)
        {
            if (candidates[j].serialized != candidates[i].serialized)
            {
                sameSequenceConflict = true;
                break;
            }
        }
        if (!sameSequenceConflict)
            coalesced.emplace(master, std::move(candidates[i]));
        i = next;
    }

    std::unique_lock lock{mutex_};

    hash_set<PublicKey> protectedKeys = listedMasterKeys;
    for (auto const& master : listedMasterKeys)
    {
        if (auto const authoritative = map_.find(master);
            authoritative != map_.end() && authoritative->second.signingKey)
            protectedKeys.insert(*authoritative->second.signingKey);
    }

    hash_set<PublicKey> conflicts;
    hash_map<PublicKey, PublicKey> candidateSigningOwners;
    for (auto const& [master, candidate] : coalesced)
    {
        if (protectedKeys.contains(master) ||
            (candidate.signingKey &&
             protectedKeys.contains(*candidate.signingKey)))
        {
            conflicts.insert(master);
            continue;
        }

        // Main-cache state wins every cross-master collision. Sequence
        // numbers are comparable only for manifests with the same master.
        if (auto const owner = signingToMasterKeys_.find(master);
            owner != signingToMasterKeys_.end() && owner->second != master)
            conflicts.insert(master);
        if (!candidate.signingKey)
            continue;
        if (auto const authoritativeMaster = map_.find(*candidate.signingKey);
            authoritativeMaster != map_.end() &&
            authoritativeMaster->first != master)
            conflicts.insert(master);
        if (auto const owner = signingToMasterKeys_.find(*candidate.signingKey);
            owner != signingToMasterKeys_.end() && owner->second != master)
            conflicts.insert(master);

        if (auto const [owner, inserted] =
                candidateSigningOwners.emplace(*candidate.signingKey, master);
            !inserted && owner->second != master)
        {
            conflicts.insert(master);
            conflicts.insert(owner->second);
        }
        if (auto const otherMaster = coalesced.find(*candidate.signingKey);
            otherMaster != coalesced.end() && otherMaster->first != master)
        {
            conflicts.insert(master);
            conflicts.insert(otherMaster->first);
        }
    }

    std::vector<PublicKey> acceptedMasters;
    acceptedMasters.reserve(coalesced.size());
    for (auto const& [master, _] : coalesced)
    {
        (void)_;
        if (!conflicts.contains(master))
            acceptedMasters.push_back(master);
    }
    std::sort(acceptedMasters.begin(), acceptedMasters.end());
    if (acceptedMasters.size() > maxPublisherCandidates)
        acceptedMasters.erase(
            acceptedMasters.begin() + maxPublisherCandidates,
            acceptedMasters.end());

    hash_map<PublicKey, Manifest> nextCandidates;
    hash_map<PublicKey, PublicKey> nextSigningToMaster;
    nextCandidates.reserve(acceptedMasters.size());
    nextSigningToMaster.reserve(acceptedMasters.size());
    for (auto const& master : acceptedMasters)
    {
        auto node = coalesced.extract(master);
        if (node.mapped().signingKey)
            nextSigningToMaster.emplace(*node.mapped().signingKey, master);
        nextCandidates.insert(std::move(node));
    }

    bool unchanged = nextCandidates.size() == publisherCandidates_.size();
    if (unchanged)
    {
        for (auto const& [master, candidate] : nextCandidates)
        {
            auto const old = publisherCandidates_.find(master);
            if (old == publisherCandidates_.end() || old->second != candidate)
            {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged)
        return;

    publisherCandidates_ = std::move(nextCandidates);
    candidateSigningToMasterKeys_ = std::move(nextSigningToMaster);
    ++seq_;
}

ManifestDisposition
ManifestCache::applyManifestImpl(
    Manifest m,
    ManifestRateLimitCapPolicy const cap,
    hash_set<PublicKey> const* const currentValidationKeys)
{
    bool const uncapped = cap == ManifestRateLimitCapPolicy::Uncapped;
    bool checkSignature = true;

    // Check the manifest against the conditions that do not require a
    // `unique_lock` (write lock) on the `mutex_`. Since the signature can be
    // relatively expensive, the `checkSignature` parameter determines if the
    // signature should be checked. Since `prewriteCheck` is run twice (see
    // comment below), `checkSignature` only needs to be set to true on the
    // first run.
    auto prewriteCheck =
        [this, &m, &checkSignature](
            auto const& iter,
            auto const& lock) -> std::optional<ManifestDisposition> {
        XRPL_ASSERT(
            lock.owns_lock(),
            "ripple::ManifestCache::applyManifest::prewriteCheck : locked");
        (void)lock;  // not used. parameter is present to ensure the mutex is
                     // locked when the lambda is called.
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

        if (checkSignature)
        {
            checkSignature = false;
            if (!m.verify())
            {
                if (auto stream = j_.warn())
                    LOG_MANIFEST_ACTION(
                        stream, "Invalid", m.masterKey, m.sequence);
                return ManifestDisposition::invalid;
            }
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

    auto atUntrustedCap = [this, uncapped](auto const& iter, auto const& lock) {
        XRPL_ASSERT(
            lock.owns_lock(),
            "ripple::ManifestCache::applyManifest::atUntrustedCap : locked");
        (void)lock;
        return iter == map_.end() && !uncapped &&
            untrustedKeys_.size() >= kMaxUntrustedCount;
    };

    auto rejectAtUntrustedCap = [this, &m]() {
        if (auto stream = j_.debug())
            LOG_MANIFEST_ACTION(
                stream, "UntrustedCapacity", m.masterKey, m.sequence);
        if (auto const n = untrustedRejectCount_.fetch_add(1) + 1;
            n % kUntrustedRejectCount == 0)
        {
            JLOG(j_.warn()) << "Untrusted manifest cap reached; " << n
                            << " manifests rejected so far";
        }
        return ManifestDisposition::untrustedCapacity;
    };

    auto evictionPermitAvailable = [this](auto const& lock) {
        XRPL_ASSERT(
            lock.owns_lock(),
            "ripple::ManifestCache::applyManifestImpl : eviction budget "
            "locked");
        (void)lock;
        return evictionPermits_ > 0 ||
            now_() - evictionBudgetUpdated_ >= kEvictionPermitInterval;
    };

    {
        std::shared_lock sl{mutex_};
        auto const iter = map_.find(m.masterKey);
        if (atUntrustedCap(iter, sl))
        {
            if (!currentValidationKeys || !evictionPermitAvailable(sl))
                return rejectAtUntrustedCap();
        }
        if (auto d = prewriteCheck(iter, sl); d.has_value())
        {
            // An uncapped application also carries retention policy. Defer a
            // stale result to the write lock so an existing entry can be
            // promoted atomically instead of racing eviction.
            if (!uncapped || *d != ManifestDisposition::stale ||
                !untrustedKeys_.contains(m.masterKey))
                return *d;
        }
    }

    std::unique_lock sl{mutex_};
    auto const iter = map_.find(m.masterKey);

    bool const needsEviction = atUntrustedCap(iter, sl);
    if (needsEviction && !currentValidationKeys)
        return rejectAtUntrustedCap();

    if (uncapped && iter != map_.end() && m.sequence <= iter->second.sequence)
    {
        if (untrustedKeys_.erase(m.masterKey) != 0)
            ++seq_;
        return ManifestDisposition::stale;
    }

    // Since we released the previously held read lock, it's possible that the
    // collections have been written to. This means we need to run
    // `prewriteCheck` again. This re-does work, but `prewriteCheck` is
    // relatively inexpensive to run, and doing it this way allows us to run
    // `prewriteCheck` under a `shared_lock` above.
    // Note, the signature has already been checked above, so it
    // doesn't need to happen again (signature checks are somewhat expensive).
    // Note: It's a mistake to use an upgradable lock. This is a recipe for
    // deadlock.
    if (auto d = prewriteCheck(iter, sl); d.has_value())
        return *d;

    if (needsEviction)
    {
        XRPL_ASSERT(
            currentValidationKeys && !untrustedKeys_.empty(),
            "ripple::ManifestCache::applyManifestImpl : eviction inputs");

        auto const now = now_();
        auto const elapsed = now - evictionBudgetUpdated_;
        auto const refillIntervals = elapsed / kEvictionPermitInterval;
        if (refillIntervals > 0)
        {
            auto const refill = refillIntervals >= kMaxEvictionPermits
                ? kMaxEvictionPermits
                : static_cast<std::size_t>(refillIntervals);
            evictionPermits_ =
                std::min(kMaxEvictionPermits, evictionPermits_ + refill);
            evictionBudgetUpdated_ += refillIntervals * kEvictionPermitInterval;
        }
        if (evictionPermits_ == 0)
            return rejectAtUntrustedCap();
        --evictionPermits_;

        std::vector<PublicKey> dormant;
        dormant.reserve(untrustedKeys_.size());
        for (auto const& master : untrustedKeys_)
        {
            auto const victim = map_.find(master);
            XRPL_ASSERT(
                victim != map_.end(),
                "ripple::ManifestCache::applyManifestImpl : untrusted key "
                "retained");
            if (victim == map_.end())
                continue;
            if (!victim->second.signingKey ||
                !currentValidationKeys->contains(*victim->second.signingKey))
            {
                dormant.push_back(master);
            }
        }

        PublicKey const victimMaster = [&]() {
            if (!dormant.empty())
            {
                if (dormant.size() == 1)
                    return dormant.front();
                return dormant[rand_int(dormant.size() - 1)];
            }
            auto victim = untrustedKeys_.begin();
            if (untrustedKeys_.size() > 1)
                std::advance(victim, rand_int(untrustedKeys_.size() - 1));
            return *victim;
        }();

        auto const victim = map_.find(victimMaster);
        XRPL_ASSERT(
            victim != map_.end(),
            "ripple::ManifestCache::applyManifestImpl : victim retained");
        if (victim == map_.end())
            return rejectAtUntrustedCap();

        if (victim->second.signingKey)
            signingToMasterKeys_.erase(*victim->second.signingKey);
        map_.erase(victim);
        untrustedKeys_.erase(victimMaster);
    }

    bool const revoked = m.revoked();
    // This is the first manifest we are seeing for a master key. This should
    // only ever happen once per validator run.
    if (iter == map_.end())
    {
        if (auto stream = j_.info())
            LOG_MANIFEST_ACTION(stream, "AcceptedNew", m.masterKey, m.sequence);

        if (!revoked)
            signingToMasterKeys_.emplace(*m.signingKey, m.masterKey);

        auto masterKey = m.masterKey;
        if (!uncapped)
            untrustedKeys_.insert(masterKey);
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

    if (uncapped)
        untrustedKeys_.erase(m.masterKey);

    signingToMasterKeys_.erase(*iter->second.signingKey);

    if (!revoked)
        signingToMasterKeys_.emplace(*m.signingKey, m.masterKey);

    iter->second = std::move(m);

    // Something has changed. Keep track of it.
    seq_++;

    return ManifestDisposition::accepted;
}

void
ManifestCache::promoteToTrusted(PublicKey const& pk)
{
    std::unique_lock sl{mutex_};
    if (untrustedKeys_.erase(pk) != 0)
    {
        // Trust classification affects which manifests are selected for the
        // cached peer snapshot, even though the retained manifest is unchanged.
        ++seq_;
    }
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

        if (applyManifest(
                std::move(*mo), ManifestRateLimitCapPolicy::Uncapped) ==
            ManifestDisposition::invalid)
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

        if (!mo || !mo->revoked() ||
            applyManifest(
                std::move(*mo), ManifestRateLimitCapPolicy::Uncapped) ==
                ManifestDisposition::invalid)
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
    std::shared_lock lock{mutex_};
    auto db = dbCon.checkoutDb();

    saveManifests(*db, dbTable, isTrusted, map_, j_);
}

// Clean up macros to avoid namespace pollution
#undef LOG_MANIFEST_ACTION
#undef LOG_MANIFEST_ACTION_WITH_OLD

}  // namespace ripple
