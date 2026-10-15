//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <xrpld/app/ledger/LiteWatch.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STAccount.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

LiteWatch::LiteWatch(Config const& config, beast::Journal journal)
    : enabled_(config.LITE_MODE)
    , discoverEnabled_(config.LITE_DISCOVER)
    , j_(journal)
{
    if (!enabled_)
        return;

    // Read by Ledger::setup(), AmendmentTable::doValidatedLedger(),
    // hashOfSeq() and LedgerMaster::setValidLedger() on every ledger: without
    // them a ledger cannot even be accepted.
    addLocked(keylet::fees().key, Tier::system, "system fees");
    addLocked(keylet::amendments().key, Tier::system, "system amendments");
    addLocked(keylet::skip().key, Tier::system, "system skip list");
    addLocked(keylet::negativeUNL().key, Tier::system, "system negative UNL");

    for (auto const& line : config.LITE_WATCH)
    {
        std::string err;
        auto const parsed = parseLiteWatchLine(line, err);
        if (!parsed)
        {
            // Config::loadFromString() has already rejected these.
            JLOG(j_.warn()) << "[" SECTION_LITE_WATCH "] " << err;
            continue;
        }

        if (parsed->first)
        {
            accounts_.insert(*parsed->first);
            auto const reason = "account " + toBase58(*parsed->first);
            addLocked(parsed->second, Tier::configured, reason);
            addLocked(
                keylet::hook(*parsed->first).key, Tier::configured, reason);
        }
        else
        {
            addLocked(parsed->second, Tier::configured, "keylet");
        }
    }

    JLOG(j_.info()) << "Lite mode: watching " << keys_.size() << " keys ("
                    << accounts_.size() << " accounts)";
}

bool
LiteWatch::addLocked(uint256 const& key, Tier tier, std::string reason)
{
    auto it = keys_.find(key);
    if (it != keys_.end())
    {
        // Promote: a discovered object the operator then asks for by name
        // must survive the deletion of whatever it was discovered through.
        if (static_cast<int>(tier) < static_cast<int>(it->second.tier))
        {
            it->second.tier = tier;
            it->second.reason = std::move(reason);
        }
        return false;
    }

    if (keys_.size() >= maxKeys)
    {
        JLOG(j_.warn()) << "Lite watch set is full (" << maxKeys
                        << " keys); not watching " << key;
        return false;
    }

    keys_.emplace(key, Entry{key, tier, std::move(reason)});
    return true;
}

bool
LiteWatch::watchAccount(AccountID const& id, Tier tier)
{
    std::lock_guard lock(mutex_);
    bool const added = accounts_.insert(id).second;
    auto const reason = "account " + toBase58(id);

    // The root, and the Hook object that lists the hooks installed on it:
    // the base fee of anything the account sends depends on those, and a
    // submit autofilling its fee reads them.
    bool const root = addLocked(keylet::account(id).key, tier, reason);
    bool const hook = addLocked(keylet::hook(id).key, tier, reason);
    return root || hook || added;
}

bool
LiteWatch::watchKey(uint256 const& key, Tier tier, std::string reason)
{
    std::lock_guard lock(mutex_);
    return addLocked(key, tier, reason.empty() ? "keylet" : std::move(reason));
}

bool
LiteWatch::unwatchKey(uint256 const& key)
{
    std::lock_guard lock(mutex_);
    auto it = keys_.find(key);
    if (it == keys_.end() || it->second.tier == Tier::system)
        return false;
    keys_.erase(it);
    return true;
}

bool
LiteWatch::unwatchAccount(AccountID const& id)
{
    std::lock_guard lock(mutex_);
    bool removed = accounts_.erase(id) != 0;

    for (auto const& k : {keylet::account(id).key, keylet::hook(id).key})
    {
        auto it = keys_.find(k);
        if (it != keys_.end() && it->second.tier != Tier::system)
        {
            keys_.erase(it);
            removed = true;
        }
    }

    // Objects only known through this account go with it. Everything an
    // operator asked for by key stays.
    for (auto i = keys_.begin(); i != keys_.end();)
    {
        if (i->second.tier == Tier::discovered)
        {
            i = keys_.erase(i);
            removed = true;
        }
        else
            ++i;
    }

    return removed;
}

bool
LiteWatch::isWatched(uint256 const& key) const
{
    std::lock_guard lock(mutex_);
    return keys_.contains(key);
}

bool
LiteWatch::isWatchedAccount(AccountID const& id) const
{
    std::lock_guard lock(mutex_);
    return accounts_.contains(id);
}

std::vector<uint256>
LiteWatch::keys() const
{
    std::lock_guard lock(mutex_);
    std::vector<uint256> ret;
    ret.reserve(keys_.size());
    for (auto const& [key, entry] : keys_)
        ret.push_back(key);
    return ret;
}

std::vector<LiteWatch::Entry>
LiteWatch::entries() const
{
    std::lock_guard lock(mutex_);
    std::vector<Entry> ret;
    ret.reserve(keys_.size());
    for (auto const& [key, entry] : keys_)
        ret.push_back(entry);
    return ret;
}

std::size_t
LiteWatch::size() const
{
    std::lock_guard lock(mutex_);
    return keys_.size();
}

std::vector<std::pair<SHAMapNodeID, uint256>>
LiteWatch::missingNodes(SHAMap const& stateMap, std::size_t max) const
{
    std::vector<std::pair<SHAMapNodeID, uint256>> missing;

    // Snapshot the keys: the walk reads the map, not the set, and a
    // discovery running on another ledger must not wait on it.
    auto const wanted = keys();

    // By hash: several keys blocked by the same node report it once. Near
    // the root that is every key.
    hash_set<uint256> reported;

    for (auto const& key : wanted)
    {
        if (missing.size() >= max)
            break;

        std::optional<std::pair<SHAMapNodeID, uint256>> gap;
        stateMap.peekItemPartial(key, gap);
        if (gap && reported.insert(gap->second).second)
            missing.push_back(*gap);
    }

    return missing;
}

std::size_t
LiteWatch::discover(
    STTx const& tx,
    STObject const& meta,
    std::uint32_t ledgerSeq)
{
    if (!discoverEnabled_ || !meta.isFieldPresent(sfAffectedNodes))
        return 0;

    std::lock_guard lock(mutex_);

    auto const watchedAccount = [&](AccountID const& id) {
        return accounts_.contains(id);
    };

    // Is this transaction about a watched account? Sent by one, run a hook
    // of one, or wrote an object that names one. Merely modifying a key
    // already watched is not enough: a trust line set against a watched
    // issuer touches the issuer's owner directory, and that must not pull
    // in every stranger's line.
    bool touches = watchedAccount(tx.getAccountID(sfAccount));

    if (!touches && meta.isFieldPresent(sfHookExecutions))
    {
        for (auto const& exec : meta.getFieldArray(sfHookExecutions))
        {
            if (exec.isFieldPresent(sfHookAccount) &&
                watchedAccount(exec.getAccountID(sfHookAccount)))
            {
                touches = true;
                break;
            }
        }
    }

    auto const& nodes = meta.getFieldArray(sfAffectedNodes);

    // Pass 1: nodes that name a watched account, and whether anything
    // watched was touched.
    std::vector<uint256> wanted;
    std::vector<uint256> deleted;

    for (auto const& node : nodes)
    {
        if (!node.isFieldPresent(sfLedgerIndex))
            continue;

        auto const index = node.getFieldH256(sfLedgerIndex);
        bool const isDeleted = node.getFName() == sfDeletedNode;

        if (keys_.contains(index))
        {
            if (isDeleted)
                deleted.push_back(index);
            continue;
        }

        if (isDeleted)
            continue;

        int const fieldsIndex = node.getFieldIndex(
            node.getFName() == sfCreatedNode ? sfNewFields : sfFinalFields);
        if (fieldsIndex == -1)
            continue;

        auto const inner =
            dynamic_cast<STObject const*>(&node.peekAtIndex(fieldsIndex));
        if (!inner)
            continue;

        bool const isLine = node.isFieldPresent(sfLedgerEntryType) &&
            node.getFieldU16(sfLedgerEntryType) == ltRIPPLE_STATE;

        bool names = false;
        for (auto const& field : *inner)
        {
            if (auto const sa = dynamic_cast<STAccount const*>(&field))
            {
                if (!sa->isDefault() && watchedAccount(sa->value()))
                {
                    names = true;
                    break;
                }
            }
            else if (
                isLine &&
                (field.getFName() == sfLowLimit ||
                 field.getFName() == sfHighLimit))
            {
                // The two parties of a trust line are the issuers of its
                // limits. Not done for offers: an issuer's every order book
                // entry is not what "my account" means.
                if (auto const lim = dynamic_cast<STAmount const*>(&field);
                    lim && watchedAccount(lim->getIssuer()))
                {
                    names = true;
                    break;
                }
            }
        }

        if (names)
        {
            touches = true;
            wanted.push_back(index);
        }
    }

    // Pass 2: a transaction that touched something watched created these.
    if (touches)
    {
        for (auto const& node : nodes)
        {
            if (node.getFName() == sfCreatedNode &&
                node.isFieldPresent(sfLedgerIndex))
                wanted.push_back(node.getFieldH256(sfLedgerIndex));
        }
    }

    std::size_t added = 0;
    std::string const reason = "tx " + to_string(tx.getTransactionID()) +
        " ledger " + std::to_string(ledgerSeq);
    for (auto const& key : wanted)
        if (addLocked(key, Tier::discovered, reason))
            ++added;

    for (auto const& key : deleted)
    {
        auto it = keys_.find(key);
        if (it != keys_.end() && it->second.tier == Tier::discovered)
            keys_.erase(it);
    }

    if (added != 0)
    {
        JLOG(j_.debug()) << "Lite mode: " << reason << " adds " << added
                         << " keys (" << keys_.size() << " watched)";
    }

    return added;
}

std::size_t
LiteWatch::discover(ReadView const& ledger)
{
    if (!discoverEnabled_)
        return 0;

    std::size_t added = 0;
    for (auto const& [tx, meta] : ledger.txs)
    {
        if (tx && meta)
            added += discover(*tx, *meta, ledger.seq());
    }
    return added;
}

Json::Value
LiteWatch::getJson() const
{
    Json::Value ret{Json::objectValue};
    ret[jss::enabled] = enabled_;

    std::lock_guard lock(mutex_);
    ret["discover"] = discoverEnabled_;
    ret["key_count"] = static_cast<Json::UInt>(keys_.size());
    ret["max_keys"] = static_cast<Json::UInt>(maxKeys);

    Json::Value accounts{Json::arrayValue};
    for (auto const& id : accounts_)
        accounts.append(toBase58(id));
    ret[jss::accounts] = std::move(accounts);

    std::size_t system = 0, configured = 0, discovered = 0;
    Json::Value keys{Json::arrayValue};
    for (auto const& [key, entry] : keys_)
    {
        switch (entry.tier)
        {
            case Tier::system:
                ++system;
                break;
            case Tier::configured:
                ++configured;
                break;
            case Tier::discovered:
                ++discovered;
                break;
        }
        Json::Value k{Json::objectValue};
        k[jss::key] = to_string(key);
        k["tier"] = entry.tier == Tier::system ? "system"
            : entry.tier == Tier::configured   ? "configured"
                                               : "discovered";
        k["reason"] = entry.reason;
        keys.append(std::move(k));
    }
    ret["system"] = static_cast<Json::UInt>(system);
    ret["configured"] = static_cast<Json::UInt>(configured);
    ret["discovered"] = static_cast<Json::UInt>(discovered);
    ret["keys"] = std::move(keys);
    return ret;
}

}  // namespace ripple
