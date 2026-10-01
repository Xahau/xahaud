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

#include <xrpld/core/ConfigSections.h>
#include <xrpld/peerfinder/PeerfinderManager.h>
#include <xrpld/peerfinder/detail/Checker.h>
#include <xrpld/peerfinder/detail/InMemoryStore.h>
#include <xrpld/peerfinder/detail/Logic.h>
#include <xrpld/peerfinder/detail/SourceStrings.h>
#include <xrpld/peerfinder/detail/StoreSqdb.h>
#include <xrpld/peerfinder/detail/Tuning.h>
#include <xrpl/basics/random.h>
#include <boost/asio/io_service.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

namespace ripple {
namespace PeerFinder {

class ManagerImp : public Manager
{
public:
    boost::asio::io_service& io_service_;
    std::optional<boost::asio::io_service::work> work_;
    clock_type& m_clock;
    beast::Journal m_journal;
    std::unique_ptr<Store> m_store;
    Checker<boost::asio::ip::tcp> checker_;
    Logic<decltype(checker_)> m_logic;
    BasicConfig const& m_config;

    //--------------------------------------------------------------------------

    ManagerImp(
        boost::asio::io_service& io_service,
        clock_type& clock,
        beast::Journal journal,
        BasicConfig const& config,
        beast::insight::Collector::ptr const& collector,
        bool useSqLiteStore)
        : Manager()
        , io_service_(io_service)
        , work_(std::in_place, std::ref(io_service_))
        , m_clock(clock)
        , m_journal(journal)
        , m_store(
              useSqLiteStore ? static_cast<Store*>(new StoreSqdb(journal))
                             : static_cast<Store*>(new InMemoryStore()))
        , checker_(io_service_)
        , m_logic(clock, *m_store, checker_, journal)
        , m_config(config)
        , m_stats(std::bind(&ManagerImp::collect_metrics, this), collector)
    {
    }

    ~ManagerImp() override
    {
        stop();
    }

    void
    stop() override
    {
        if (work_)
        {
            work_.reset();
            checker_.stop();
            m_logic.stop();
        }
    }

    //--------------------------------------------------------------------------
    //
    // PeerFinder
    //
    //--------------------------------------------------------------------------

    void
    setConfig(Config const& config) override
    {
        {
            std::lock_guard lock(m_highwayMutex);
            m_highwayEnabled = config.udpHighway;
            m_highwayPrivate = config.peerPrivate;
            if (!m_highwayEnabled)
            {
                m_highwayPeers.clear();
                m_highwayLearned = 0;
            }
        }

        m_logic.config(config);
    }

    Config
    config() override
    {
        return m_logic.config();
    }

    void
    addFixedPeer(
        std::string const& name,
        std::vector<beast::IP::Endpoint> const& addresses) override
    {
        {
            std::lock_guard lock(m_highwayMutex);
            if (m_highwayEnabled)
            {
                auto const now = m_clock.now();
                for (auto const& address : addresses)
                    learnHighwayPeer(address, HighwaySource::fixed, now);
            }
        }

        m_logic.addFixedPeer(name, addresses);
    }

    void
    addFallbackStrings(
        std::string const& name,
        std::vector<std::string> const& strings) override
    {
        m_logic.addStaticSource(SourceStrings::New(name, strings));
    }

    void
    addFallbackURL(std::string const& name, std::string const& url)
    {
        // VFALCO TODO This needs to be implemented
    }

    //--------------------------------------------------------------------------

    std::shared_ptr<Slot>
    new_inbound_slot(
        beast::IP::Endpoint const& local_endpoint,
        beast::IP::Endpoint const& remote_endpoint) override
    {
        return m_logic.new_inbound_slot(local_endpoint, remote_endpoint);
    }

    std::shared_ptr<Slot>
    new_outbound_slot(beast::IP::Endpoint const& remote_endpoint) override
    {
        return m_logic.new_outbound_slot(remote_endpoint);
    }

    void
    on_endpoints(std::shared_ptr<Slot> const& slot, Endpoints const& endpoints)
        override
    {
        {
            std::lock_guard lock(m_highwayMutex);
            if (m_highwayEnabled)
            {
                auto const now = m_clock.now();
                for (auto const& ep : endpoints)
                    learnHighwayPeer(
                        ep.address, HighwaySource::authenticated, now);
            }
        }

        SlotImp::ptr impl(std::dynamic_pointer_cast<SlotImp>(slot));
        m_logic.on_endpoints(impl, endpoints);
    }

    void
    on_closed(std::shared_ptr<Slot> const& slot) override
    {
        SlotImp::ptr impl(std::dynamic_pointer_cast<SlotImp>(slot));
        m_logic.on_closed(impl);
    }

    void
    on_failure(std::shared_ptr<Slot> const& slot) override
    {
        SlotImp::ptr impl(std::dynamic_pointer_cast<SlotImp>(slot));
        m_logic.on_failure(impl);
    }

    void
    onRedirects(
        boost::asio::ip::tcp::endpoint const& remote_address,
        std::vector<boost::asio::ip::tcp::endpoint> const& eps) override
    {
        {
            std::lock_guard lock(m_highwayMutex);
            if (m_highwayEnabled)
            {
                auto const now = m_clock.now();
                for (auto const& ep : eps)
                    learnHighwayPeer(
                        beast::IPAddressConversion::from_asio(ep),
                        HighwaySource::authenticated,
                        now);
            }
        }

        m_logic.onRedirects(eps.begin(), eps.end(), remote_address);
    }

    //--------------------------------------------------------------------------

    bool
    onConnected(
        std::shared_ptr<Slot> const& slot,
        beast::IP::Endpoint const& local_endpoint) override
    {
        SlotImp::ptr impl(std::dynamic_pointer_cast<SlotImp>(slot));
        return m_logic.onConnected(impl, local_endpoint);
    }

    Result
    activate(
        std::shared_ptr<Slot> const& slot,
        PublicKey const& key,
        bool reserved) override
    {
        SlotImp::ptr impl(std::dynamic_pointer_cast<SlotImp>(slot));
        return m_logic.activate(impl, key, reserved);
    }

    std::vector<Endpoint>
    redirect(std::shared_ptr<Slot> const& slot) override
    {
        SlotImp::ptr impl(std::dynamic_pointer_cast<SlotImp>(slot));
        return m_logic.redirect(impl);
    }

    std::vector<beast::IP::Endpoint>
    autoconnect() override
    {
        return m_logic.autoconnect();
    }

    void
    once_per_second() override
    {
        m_logic.once_per_second();
        expireHighwayPeers();
    }

    std::vector<std::pair<std::shared_ptr<Slot>, std::vector<Endpoint>>>
    buildEndpointsForPeers() override
    {
        return m_logic.buildEndpointsForPeers();
    }

    //--------------------------------------------------------------------------
    //
    // XUSH (Xahau UDP Superhighway)
    //
    //--------------------------------------------------------------------------

    void
    add_highway_peers(
        std::vector<beast::IP::Endpoint> const& endpoints) override
    {
        std::lock_guard lock(m_highwayMutex);
        if (!m_highwayEnabled)
            return;

        auto const now = m_clock.now();
        for (auto const& ep : endpoints)
            learnHighwayPeer(ep, HighwaySource::advertised, now);
    }

    std::vector<beast::IP::Endpoint>
    highway_targets(std::size_t n) override
    {
        std::lock_guard lock(m_highwayMutex);
        return sampleHighwayPeers(n, [this](HighwayPeer const& peer) {
            return peer.fixed || !m_highwayPrivate;
        });
    }

    std::vector<beast::IP::Endpoint>
    highway_adverts(std::size_t n) override
    {
        std::lock_guard lock(m_highwayMutex);
        if (m_highwayPrivate)
            return {};

        return sampleHighwayPeers(
            n, [](HighwayPeer const& peer) { return !peer.fixed; });
    }

    //--------------------------------------------------------------------------

    void
    start() override
    {
        if (auto sqdb = dynamic_cast<StoreSqdb*>(m_store.get()))
            sqdb->open(m_config);
        m_logic.load();
    }

    //--------------------------------------------------------------------------
    //
    // PropertyStream
    //
    //--------------------------------------------------------------------------

    void
    onWrite(beast::PropertyStream::Map& map) override
    {
        m_logic.onWrite(map);
    }

private:
    //--------------------------------------------------------------------------
    //
    // XUSH (Xahau UDP Superhighway)
    //
    //--------------------------------------------------------------------------

    enum class HighwaySource {
        // Configured by the operator in [ips_fixed]
        fixed,

        // Endpoint gossip or a redirect received over a peer connection
        authenticated,

        // An unauthenticated XUSHPEER datagram
        advertised
    };

    struct HighwayPeer
    {
        // Fixed peers never expire and are never advertised.
        bool fixed = false;

        // When the endpoint was last learned from a fixed or authenticated
        // source. Learned entries expire once this is too old.
        clock_type::time_point learned;
    };

    std::mutex m_highwayMutex;
    bool m_highwayEnabled = false;
    bool m_highwayPrivate = true;
    std::size_t m_highwayLearned = 0;  // Number of entries that are not fixed
    std::map<beast::IP::Endpoint, HighwayPeer> m_highwayPeers;

    // Requires m_highwayMutex
    void
    learnHighwayPeer(
        beast::IP::Endpoint const& ep,
        HighwaySource source,
        clock_type::time_point now)
    {
        if (is_unspecified(ep) || ep.port() == 0)
            return;

        auto const it = m_highwayPeers.find(ep);

        if (source == HighwaySource::fixed)
        {
            if (it == m_highwayPeers.end())
            {
                m_highwayPeers.emplace(ep, HighwayPeer{true, now});
            }
            else if (!it->second.fixed)
            {
                it->second.fixed = true;
                --m_highwayLearned;
            }
            return;
        }

        // Endpoints learned from other servers must be publicly routable.
        if (!is_public(ep))
            return;

        if (it != m_highwayPeers.end())
        {
            // Only authenticated sources extend the life of an entry, so that
            // unauthenticated advertisements can't keep stale entries alive.
            if (source == HighwaySource::authenticated)
                it->second.learned = now;
            return;
        }

        if (m_highwayLearned >= Tuning::highwayPeersMax)
            return;

        m_highwayPeers.emplace(ep, HighwayPeer{false, now});
        ++m_highwayLearned;
    }

    void
    expireHighwayPeers()
    {
        std::lock_guard lock(m_highwayMutex);
        auto const now = m_clock.now();

        for (auto it = m_highwayPeers.begin(); it != m_highwayPeers.end();)
        {
            if (!it->second.fixed &&
                now - it->second.learned > Tuning::highwayPeerSecondsToLive)
            {
                it = m_highwayPeers.erase(it);
                --m_highwayLearned;
            }
            else
            {
                ++it;
            }
        }
    }

    // Requires m_highwayMutex
    template <class Predicate>
    std::vector<beast::IP::Endpoint>
    sampleHighwayPeers(std::size_t n, Predicate&& eligible)
    {
        std::vector<beast::IP::Endpoint const*> candidates;
        candidates.reserve(m_highwayPeers.size());
        for (auto const& [ep, peer] : m_highwayPeers)
        {
            if (eligible(peer))
                candidates.push_back(&ep);
        }

        n = std::min(n, candidates.size());

        // A partial Fisher-Yates shuffle: the first n candidates become a
        // uniformly random sample.
        std::vector<beast::IP::Endpoint> result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            std::uniform_int_distribution<std::size_t> pick(
                i, candidates.size() - 1);
            std::swap(candidates[i], candidates[pick(default_prng())]);
            result.push_back(*candidates[i]);
        }

        return result;
    }

    //--------------------------------------------------------------------------

    struct Stats
    {
        template <class Handler>
        Stats(
            Handler const& handler,
            beast::insight::Collector::ptr const& collector)
            : hook(collector->make_hook(handler))
            , activeInboundPeers(
                  collector->make_gauge("Peer_Finder", "Active_Inbound_Peers"))
            , activeOutboundPeers(
                  collector->make_gauge("Peer_Finder", "Active_Outbound_Peers"))
        {
        }

        beast::insight::Hook hook;
        beast::insight::Gauge activeInboundPeers;
        beast::insight::Gauge activeOutboundPeers;
    };

    std::mutex m_statsMutex;
    Stats m_stats;

    void
    collect_metrics()
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.activeInboundPeers = m_logic.counts_.inboundActive();
        m_stats.activeOutboundPeers = m_logic.counts_.out_active();
    }
};

//------------------------------------------------------------------------------

Manager::Manager() noexcept : beast::PropertyStream::Source("peerfinder")
{
}

std::unique_ptr<Manager>
make_Manager(
    boost::asio::io_service& io_service,
    clock_type& clock,
    beast::Journal journal,
    BasicConfig const& config,
    beast::insight::Collector::ptr const& collector,
    bool useSqLiteStore)
{
    return std::make_unique<ManagerImp>(
        io_service, clock, journal, config, collector, useSqLiteStore);
}

}  // namespace PeerFinder
}  // namespace ripple
