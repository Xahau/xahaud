//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#include <xrpld/rpc/PWAStats.h>

#include <algorithm>
#include <vector>

namespace ripple {

namespace {

std::int64_t
epochSeconds(std::chrono::system_clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               t.time_since_epoch())
        .count();
}

}  // namespace

PWAStats::PWAStats() : since_(std::chrono::system_clock::now())
{
}

char const*
PWAStats::to_string(Outcome o)
{
    switch (o)
    {
        case Outcome::served:
            return "served";
        case Outcome::notModified:
            return "not_modified";
        case Outcome::notFound:
            return "not_found";
        case Outcome::badTarget:
            return "bad_target";
        case Outcome::badMethod:
            return "bad_method";
        case Outcome::noForwardedFor:
            return "no_forwarded_for";
        case Outcome::directConnection:
            return "direct_connection";
        case Outcome::throttled:
            return "throttled";
        case Outcome::unavailable:
            return "unavailable";
        case Outcome::count_:
            break;
    }
    return "unknown";
}

void
PWAStats::record(
    Outcome outcome,
    std::optional<boost::asio::ip::address> const& client,
    std::optional<AccountID> const& account,
    std::size_t bytes,
    std::uint32_t ledgerSeq)
{
    auto const idx = static_cast<std::size_t>(outcome);
    if (idx >= outcomeCount)
        return;

    bool const ok =
        outcome == Outcome::served || outcome == Outcome::notModified;

    outcomes_[idx].fetch_add(1, std::memory_order_relaxed);
    if (outcome == Outcome::served)
        bytesServed_.fetch_add(bytes, std::memory_order_relaxed);
    if (ledgerSeq)
        lastLedger_.store(ledgerSeq, std::memory_order_relaxed);

    // Per-account rows only for loaders that exist; see the class comment.
    bool const trackAccount = ok && account.has_value();
    if (!trackAccount && !client)
        return;

    auto const now = epochSeconds(std::chrono::system_clock::now());

    std::lock_guard lock(mutex_);

    if (trackAccount)
    {
        auto it = accounts_.find(*account);
        if (it == accounts_.end() && accounts_.size() < maxTrackedAccounts)
            it = accounts_.emplace(*account, AccountEntry{}).first;

        if (it == accounts_.end())
        {
            ++untrackedAccountRequests_;
        }
        else
        {
            auto& e = it->second;
            if (outcome == Outcome::served)
            {
                ++e.served;
                e.bytes += bytes;
            }
            else
            {
                ++e.notModified;
            }
            if (ledgerSeq)
                e.lastLedger = ledgerSeq;
            e.lastSeen = now;
        }
    }

    if (client)
    {
        auto const key = client->to_string();
        auto it = clients_.find(key);
        if (it == clients_.end() && clients_.size() < maxTrackedClients)
            it = clients_.emplace(key, ClientEntry{}).first;

        if (it == clients_.end())
        {
            ++untrackedClientRequests_;
        }
        else
        {
            auto& e = it->second;
            ++e.requests;
            if (outcome == Outcome::served)
                ++e.served;
            else if (!ok)
                ++e.rejected;
            e.lastSeen = now;
        }
    }
}

Json::Value
PWAStats::getJson(std::size_t top) const
{
    top = std::min(top, maxTop);

    Json::Value ret(Json::objectValue);

    std::uint64_t total = 0;
    Json::Value outcomes(Json::objectValue);
    for (std::size_t i = 0; i < outcomeCount; ++i)
    {
        auto const n = outcomes_[i].load(std::memory_order_relaxed);
        total += n;
        outcomes[to_string(static_cast<Outcome>(i))] = std::to_string(n);
    }
    ret["requests"] = std::to_string(total);
    ret["outcomes"] = outcomes;
    ret["bytes_served"] =
        std::to_string(bytesServed_.load(std::memory_order_relaxed));
    ret["last_ledger_index"] = lastLedger_.load(std::memory_order_relaxed);

    std::lock_guard lock(mutex_);

    auto const now = std::chrono::system_clock::now();
    ret["since"] = static_cast<Json::UInt>(epochSeconds(since_));
    ret["window_seconds"] = static_cast<Json::UInt>(
        std::chrono::duration_cast<std::chrono::seconds>(now - since_).count());

    {
        std::vector<decltype(accounts_)::const_iterator> rows;
        rows.reserve(accounts_.size());
        for (auto it = accounts_.cbegin(); it != accounts_.cend(); ++it)
            rows.push_back(it);
        auto const n = std::min(top, rows.size());
        std::partial_sort(
            rows.begin(),
            rows.begin() + n,
            rows.end(),
            [](auto const& a, auto const& b) {
                auto const ha = a->second.served + a->second.notModified;
                auto const hb = b->second.served + b->second.notModified;
                if (ha != hb)
                    return ha > hb;
                return a->first < b->first;
            });

        Json::Value accounts(Json::objectValue);
        accounts["tracked"] = static_cast<Json::UInt>(accounts_.size());
        accounts["untracked_requests"] =
            std::to_string(untrackedAccountRequests_);
        Json::Value& list = accounts["top"] = Json::arrayValue;
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const& [id, e] = *rows[i];
            Json::Value& row = list.append(Json::objectValue);
            row["account"] = toBase58(id);
            row["served"] = std::to_string(e.served);
            row["not_modified"] = std::to_string(e.notModified);
            row["bytes"] = std::to_string(e.bytes);
            row["last_ledger_index"] = e.lastLedger;
            row["last_seen"] = static_cast<Json::UInt>(e.lastSeen);
        }
        ret["accounts"] = accounts;
    }

    {
        std::vector<decltype(clients_)::const_iterator> rows;
        rows.reserve(clients_.size());
        for (auto it = clients_.cbegin(); it != clients_.cend(); ++it)
            rows.push_back(it);
        auto const n = std::min(top, rows.size());
        std::partial_sort(
            rows.begin(),
            rows.begin() + n,
            rows.end(),
            [](auto const& a, auto const& b) {
                if (a->second.requests != b->second.requests)
                    return a->second.requests > b->second.requests;
                return a->first < b->first;
            });

        Json::Value clients(Json::objectValue);
        clients["tracked"] = static_cast<Json::UInt>(clients_.size());
        clients["untracked_requests"] =
            std::to_string(untrackedClientRequests_);
        Json::Value& list = clients["top"] = Json::arrayValue;
        for (std::size_t i = 0; i < n; ++i)
        {
            auto const& [ip, e] = *rows[i];
            Json::Value& row = list.append(Json::objectValue);
            row["ip"] = ip;
            row["requests"] = std::to_string(e.requests);
            row["served"] = std::to_string(e.served);
            row["rejected"] = std::to_string(e.rejected);
            row["last_seen"] = static_cast<Json::UInt>(e.lastSeen);
        }
        ret["clients"] = clients;
    }

    return ret;
}

void
PWAStats::reset()
{
    for (auto& c : outcomes_)
        c.store(0, std::memory_order_relaxed);
    bytesServed_.store(0, std::memory_order_relaxed);
    lastLedger_.store(0, std::memory_order_relaxed);

    std::lock_guard lock(mutex_);
    accounts_.clear();
    clients_.clear();
    untrackedAccountRequests_ = 0;
    untrackedClientRequests_ = 0;
    since_ = std::chrono::system_clock::now();
}

}  // namespace ripple
