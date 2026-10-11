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

#include <xrpld/rpc/PWADomainResolver.h>

#include <xrpl/basics/Log.h>
#include <xrpl/crypto/csprng.h>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <set>
#include <unordered_map>

namespace ripple {

namespace {

using clock_type = std::chrono::steady_clock;
using udp = boost::asio::ip::udp;

constexpr std::uint16_t typeTXT = 16;
constexpr std::size_t headerSize = 12;
constexpr std::size_t optSize = 11;  // the EDNS(0) OPT record in a query

std::uint16_t
get16(std::uint8_t const* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void
put16(std::vector<std::uint8_t>& v, std::uint16_t x)
{
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xff));
}

std::uint8_t
lowerAscii(std::uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c + 32) : c;
}

// Offset just past the (possibly compressed) name at `pos`, or nullopt.
std::optional<std::size_t>
skipName(std::uint8_t const* p, std::size_t size, std::size_t pos)
{
    for (int labels = 0; labels < 128 && pos < size; ++labels)
    {
        auto const len = p[pos];
        if ((len & 0xC0) == 0xC0)
            return pos + 2 <= size ? std::optional{pos + 2} : std::nullopt;
        if (len & 0xC0)
            return std::nullopt;
        if (len == 0)
            return pos + 1;
        pos += 1 + len;
    }
    return std::nullopt;
}

}  // namespace

bool
PWADomainResolver::isCustomDomainName(std::string_view host)
{
    if (host.empty() || host.size() + recordLabel.size() + 1 > 253)
        return false;
    std::size_t labels = 0;
    std::string_view label;
    for (std::string_view rest = host;; rest.remove_prefix(label.size() + 1))
    {
        label = rest.substr(0, rest.find('.'));
        if (label.empty() || label.size() > 63 || label.front() == '-' ||
            label.back() == '-' ||
            label.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") !=
                std::string_view::npos)
            return false;
        ++labels;
        if (label.size() == rest.size())
            break;
    }
    // An all-digit top label is an IPv4 literal.
    return labels >= 2 &&
        label.find_first_not_of("0123456789") != std::string_view::npos;
}

std::vector<std::uint8_t>
PWADomainResolver::encodeQuery(std::string_view host, std::uint16_t id)
{
    std::vector<std::uint8_t> q;
    put16(q, id);
    put16(q, 0x0100);  // RD
    put16(q, 1);       // QDCOUNT
    put16(q, 0);
    put16(q, 0);
    put16(q, 1);  // ARCOUNT: OPT
    auto label = [&](std::string_view l) {
        q.push_back(static_cast<std::uint8_t>(l.size()));
        q.insert(q.end(), l.begin(), l.end());
    };
    label(recordLabel);
    for (std::string_view rest = host; !rest.empty();)
    {
        auto const dot = rest.find('.');
        label(rest.substr(0, dot));
        rest.remove_prefix(
            dot == std::string_view::npos ? rest.size() : dot + 1);
    }
    q.push_back(0);
    put16(q, typeTXT);
    put16(q, 1);  // IN
    // OPT: 1232 byte UDP payload, so a sane answer never truncates.
    q.push_back(0);
    put16(q, 41);
    put16(q, 1232);
    put16(q, 0);
    put16(q, 0);
    put16(q, 0);
    return q;
}

PWADomainResolver::Parsed
PWADomainResolver::parseResponse(
    std::vector<std::uint8_t> const& query,
    std::uint8_t const* p,
    std::size_t size)
{
    Parsed out;  // mismatch until proven ours
    auto const qlen = query.size() - headerSize - optSize;  // name+type+class
    if (size < headerSize + qlen || p[0] != query[0] || p[1] != query[1] ||
        !(p[2] & 0x80) || ((p[2] >> 3) & 0x0F) != 0 || get16(p + 4) != 1)
        return out;
    for (std::size_t i = 0; i < qlen; ++i)  // the question, ignoring case
        if (lowerAscii(p[headerSize + i]) != lowerAscii(query[headerSize + i]))
            return out;

    auto fail = [&] {
        out.kind = Parsed::Kind::error;
        out.txt.clear();
        return out;
    };
    auto const rcode = p[3] & 0x0F;
    if (p[2] & 0x02)  // truncated
        return fail();
    if (rcode == 3)
    {
        out.kind = Parsed::Kind::nxdomain;
        return out;
    }
    if (rcode != 0)
        return fail();

    std::size_t pos = headerSize + qlen;
    std::optional<std::uint32_t> minTTL;
    for (std::uint16_t i = 0, n = get16(p + 6); i < n; ++i)
    {
        auto const after = skipName(p, size, pos);
        if (!after || *after + 10 > size)
            return fail();
        pos = *after;
        auto const type = get16(p + pos);
        auto const cls = get16(p + pos + 2);
        auto const ttl = (std::uint32_t{p[pos + 4]} << 24) |
            (std::uint32_t{p[pos + 5]} << 16) |
            (std::uint32_t{p[pos + 6]} << 8) | p[pos + 7];
        auto const rdlen = get16(p + pos + 8);
        pos += 10;
        if (pos + rdlen > size)
            return fail();
        if (type == typeTXT && cls == 1)  // CNAMEs on the way are skipped
        {
            std::string s;
            for (std::size_t i2 = 0; i2 < rdlen;)
            {
                auto const len = p[pos + i2];
                if (i2 + 1 + len > rdlen)
                    return fail();
                s.append(reinterpret_cast<char const*>(p + pos + i2 + 1), len);
                i2 += 1 + len;
            }
            out.txt.push_back(std::move(s));
            auto const t = (ttl & 0x80000000u) ? 0u : ttl;  // RFC 2181 8
            minTTL = minTTL ? std::min(*minTTL, t) : t;
        }
        pos += rdlen;
    }
    out.kind = Parsed::Kind::answer;
    out.ttl = minTTL.value_or(0);
    return out;
}

PWADomainResolver::Answer
PWADomainResolver::decide(std::vector<std::string> const& txt)
{
    std::set<AccountID> named;
    for (auto const& raw : txt)
    {
        std::string_view s = raw;
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix(1);
        if (auto const a = parseBase58<AccountID>(std::string(s));
            a && !a->isZero())
            named.insert(*a);
    }
    if (named.size() == 1)
        return {Status::found, *named.begin()};
    return {Status::none, {}};
}

//------------------------------------------------------------------------------

class PWADomainResolver::Impl : public std::enable_shared_from_this<Impl>
{
public:
    class Query;
    struct Pending
    {
        std::vector<Handler> handlers;
        std::weak_ptr<Query> query;
    };

    boost::asio::io_context& io_;
    beast::Journal const j_;
    std::mutex mutex_;
    bool stopped_ = false;
    std::unordered_map<std::string, std::pair<Answer, clock_type::time_point>>
        cache_;
    std::unordered_map<std::string, Pending> pending_;
    std::size_t waiters_ = 0;
    std::uint64_t hits_ = 0, queries_ = 0, found_ = 0, none_ = 0, failed_ = 0,
                  timeouts_ = 0;

    Impl(boost::asio::io_context& io, beast::Journal j) : io_(io), j_(j)
    {
    }

    // Cache a finished lookup and hand it to everyone waiting on it.
    void
    complete(
        std::string const& host,
        Answer const& answer,
        std::uint32_t ttl,
        std::uint64_t timeouts)
    {
        std::vector<Handler> handlers;
        {
            std::lock_guard lock(mutex_);
            timeouts_ += timeouts;
            (answer.status == Status::found      ? found_
                 : answer.status == Status::none ? none_
                                                 : failed_)++;
            if (!stopped_)
            {
                auto const now = clock_type::now();
                if (cache_.size() >= maxCacheEntries &&
                    cache_.find(host) == cache_.end())
                {
                    std::erase_if(cache_, [&](auto const& e) {
                        return e.second.second <= now;
                    });
                    if (cache_.size() >= maxCacheEntries)
                        cache_.erase(cache_.begin());
                }
                cache_[host] = {answer, now + std::chrono::seconds(ttl)};
            }
            if (auto it = pending_.find(host); it != pending_.end())
            {
                handlers = std::move(it->second.handlers);
                waiters_ -= handlers.size();
                pending_.erase(it);
            }
        }
        for (auto const& h : handlers)
            h(answer);
    }
};

// One lookup: up to maxAttempts datagrams round robin over the servers,
// each with a fresh ID and ephemeral port, all on one strand. A generation
// counter retires the handlers of an abandoned attempt.
class PWADomainResolver::Impl::Query
    : public std::enable_shared_from_this<Query>
{
    std::shared_ptr<Impl> impl_;
    std::string const host_;
    std::vector<udp::endpoint> const servers_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    udp::socket socket_;
    boost::asio::steady_timer timer_;
    std::array<std::uint8_t, 4096> buf_{};
    std::vector<std::uint8_t> wire_;
    std::size_t attempt_ = 0;
    std::uint64_t gen_ = 0, timeouts_ = 0;
    bool done_ = false;

public:
    Query(
        std::shared_ptr<Impl> impl,
        std::string host,
        std::vector<udp::endpoint> servers)
        : impl_(std::move(impl))
        , host_(std::move(host))
        , servers_(std::move(servers))
        , strand_(boost::asio::make_strand(impl_->io_))
        , socket_(strand_)
        , timer_(strand_)
    {
    }

    void
    run()
    {
        boost::asio::post(
            strand_, [self = shared_from_this()] { self->attempt(); });
    }

    void
    cancel()
    {
        boost::asio::post(strand_, [self = shared_from_this()] {
            self->finish({Status::failed, {}}, 0);
        });
    }

private:
    void
    attempt()
    {
        if (done_)
            return;
        if (servers_.empty() || attempt_ >= maxAttempts)
            return finish({Status::failed, {}}, failureTTL);
        auto const& server = servers_[attempt_++ % servers_.size()];
        auto const gen = ++gen_;

        std::uint16_t id;
        crypto_prng()(&id, sizeof(id));
        wire_ = encodeQuery(host_, id);

        boost::system::error_code ec;
        socket_.close(ec);
        socket_.open(server.protocol(), ec);
        if (!ec)
            socket_.connect(server, ec);  // no I/O: only this peer may answer
        if (ec)
            return boost::asio::post(
                strand_, [self = shared_from_this()] { self->attempt(); });

        timer_.expires_after(attemptTimeout);
        timer_.async_wait(
            [self = shared_from_this(), gen](boost::system::error_code ec) {
                if (ec || gen != self->gen_ || self->done_)
                    return;
                ++self->timeouts_;
                boost::system::error_code ignored;
                self->socket_.close(ignored);  // aborts the receive: retry
            });
        socket_.async_send(
            boost::asio::buffer(wire_),
            [self = shared_from_this(), gen](
                boost::system::error_code ec, std::size_t) {
                if (ec && gen == self->gen_ && !self->done_)
                    self->retry();
            });
        receive(gen);
    }

    void
    receive(std::uint64_t gen)
    {
        socket_.async_receive(
            boost::asio::buffer(buf_),
            [self = shared_from_this(), gen](
                boost::system::error_code ec, std::size_t n) {
                if (gen != self->gen_ || self->done_)
                    return;
                if (ec)
                    return self->retry();
                auto const parsed =
                    parseResponse(self->wire_, self->buf_.data(), n);
                switch (parsed.kind)
                {
                    case Parsed::Kind::mismatch:  // not ours: keep waiting
                        return self->receive(gen);
                    case Parsed::Kind::error:
                        return self->retry();
                    case Parsed::Kind::nxdomain:
                        return self->finish({Status::none, {}}, negativeTTL);
                    case Parsed::Kind::answer: {
                        auto const answer = decide(parsed.txt);
                        return self->finish(
                            answer,
                            answer.status == Status::found ? std::clamp(
                                                                 parsed.ttl,
                                                                 minPositiveTTL,
                                                                 maxPositiveTTL)
                                                           : negativeTTL);
                    }
                }
            });
    }

    void
    retry()
    {
        ++gen_;
        timer_.cancel();
        boost::system::error_code ignored;
        socket_.close(ignored);
        attempt();
    }

    void
    finish(Answer const& answer, std::uint32_t ttl)
    {
        if (done_)
            return;
        done_ = true;
        ++gen_;
        timer_.cancel();
        boost::system::error_code ignored;
        socket_.close(ignored);
        JLOG(impl_->j_.debug())
            << recordLabel << "." << host_ << ": "
            << (answer.status == Status::found      ? toBase58(answer.account)
                    : answer.status == Status::none ? "no record"
                                                    : "failed");
        impl_->complete(host_, answer, ttl, timeouts_);
    }
};

//------------------------------------------------------------------------------

PWADomainResolver::PWADomainResolver(
    boost::asio::io_context& io,
    beast::Journal j)
    : impl_(std::make_shared<Impl>(io, j))
{
}

// stop() is the orderly path, while the io_context runs. Here it may not,
// so post nothing: drop the waiters and let any running query finish on
// its own, keeping Impl alive until it does.
PWADomainResolver::~PWADomainResolver()
{
    std::lock_guard lock(impl_->mutex_);
    impl_->stopped_ = true;
    impl_->pending_.clear();
    impl_->waiters_ = 0;
}

std::optional<PWADomainResolver::Answer>
PWADomainResolver::cached(std::string const& host)
{
    std::lock_guard lock(impl_->mutex_);
    auto const it = impl_->cache_.find(host);
    if (it == impl_->cache_.end())
        return std::nullopt;
    if (it->second.second <= clock_type::now())
    {
        impl_->cache_.erase(it);
        return std::nullopt;
    }
    ++impl_->hits_;
    return it->second.first;
}

PWADomainResolver::Start
PWADomainResolver::resolve(
    std::string const& host,
    std::vector<udp::endpoint> const& servers,
    Handler handler)
{
    std::shared_ptr<Impl::Query> query;
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->stopped_)
            return Start::stopped;
        if (impl_->waiters_ >= maxWaiters)
            return Start::busy;
        if (auto it = impl_->pending_.find(host); it != impl_->pending_.end())
        {
            it->second.handlers.push_back(std::move(handler));
            ++impl_->waiters_;
            return Start::coalesced;
        }
        if (impl_->pending_.size() >= maxQueriesInFlight)
            return Start::busy;
        query = std::make_shared<Impl::Query>(impl_, host, servers);
        auto& p = impl_->pending_[host];
        p.handlers.push_back(std::move(handler));
        p.query = query;
        ++impl_->waiters_;
        ++impl_->queries_;
    }
    query->run();
    return Start::started;
}

void
PWADomainResolver::stop()
{
    std::vector<Handler> handlers;
    std::vector<std::shared_ptr<Impl::Query>> queries;
    {
        std::lock_guard lock(impl_->mutex_);
        if (impl_->stopped_)
            return;
        impl_->stopped_ = true;
        for (auto& [host, p] : impl_->pending_)
        {
            for (auto& h : p.handlers)
                handlers.push_back(std::move(h));
            if (auto q = p.query.lock())
                queries.push_back(std::move(q));
        }
        impl_->pending_.clear();
        impl_->waiters_ = 0;
    }
    for (auto const& q : queries)
        q->cancel();
    for (auto const& h : handlers)
        h({Status::failed, {}});
}

std::size_t
PWADomainResolver::flush()
{
    std::lock_guard lock(impl_->mutex_);
    auto const n = impl_->cache_.size();
    impl_->cache_.clear();
    return n;
}

Json::Value
PWADomainResolver::getJson()
{
    Json::Value ret(Json::objectValue);
    std::lock_guard lock(impl_->mutex_);
    auto const& i = *impl_;
    ret["cache_entries"] = static_cast<Json::UInt>(i.cache_.size());
    ret["queries_in_flight"] = static_cast<Json::UInt>(i.pending_.size());
    ret["queries"] = std::to_string(i.queries_);
    ret["cache_hits"] = std::to_string(i.hits_);
    ret["found"] = std::to_string(i.found_);
    ret["no_record"] = std::to_string(i.none_);
    ret["failed"] = std::to_string(i.failed_);
    ret["timeouts"] = std::to_string(i.timeouts_);
    return ret;
}

}  // namespace ripple
