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

#include <ripple/app/main/Application.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/peerfinder/PeerfinderManager.h>
#include <ripple/peerfinder/impl/Checker.h>
#include <ripple/peerfinder/impl/InMemoryStore.h>
#include <ripple/peerfinder/impl/Logic.h>
#include <ripple/peerfinder/impl/SourceStrings.h>
#include <ripple/peerfinder/impl/StoreSqdb.h>
#include <boost/asio/io_service.hpp>
#include <boost/utility/in_place_factory.hpp>
#include <memory>
#include <optional>
#include <thread>

namespace ripple {
namespace PeerFinder {

class ManagerImp : public Manager
{
protected:
    Application& app_;

    std::map<
        beast::IP::Endpoint /* udp endpoint */,
        uint32_t /* unixtime last seen */>
        m_udp_highway_peers;

    std::mutex m_udp_highway_mutex;

public:
    boost::asio::io_service& io_service_;
    std::optional<boost::asio::io_service::work> work_;
    clock_type& m_clock;
    beast::Journal m_journal;
    std::unique_ptr<Store> m_store;
    Checker<boost::asio::ip::tcp> checker_;
    Logic<decltype(checker_)> m_logic;
    BasicConfig const& m_config;

    void
    add_highway_peers(std::vector<beast::IP::Endpoint> addresses) override
    {
        std::lock_guard<std::mutex> lock(m_udp_highway_mutex);
        uint32_t t = static_cast<uint32_t>(std::time(nullptr));
        for (auto const& a : addresses)
            m_udp_highway_peers.emplace(a, t);
    }

    /* send a transaction over datagram to a large random subset of highway
     * peers */
    void
    machine_gun_highway_peers(Slice const& tx, uint256 const& txid) override
    {
        constexpr size_t kMaxDatagram = 65535;
        constexpr size_t kUDPHeader = 8;
        constexpr size_t kIPv4Header = 20;
        constexpr size_t kIPv6Header = 40;
        constexpr size_t kHeaderSize = 52;

        // Create sockets once
        static thread_local int udp4_sock = [this]() {
            int s = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(app_.config().UDP_HIGHWAY_PORT);
            addr.sin_addr.s_addr = INADDR_ANY;
            ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            return s;
        }();

        static thread_local int udp6_sock = [this]() {
            int s = ::socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);
            sockaddr_in6 addr{};
            addr.sin6_family = AF_INET6;
            addr.sin6_port = htons(app_.config().UDP_HIGHWAY_PORT);
            addr.sin6_addr = in6addr_any;
            ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            return s;
        }();

        std::lock_guard<std::mutex> lock(m_udp_highway_mutex);
        if (m_udp_highway_peers.empty())
            return;

        // Select ~50% of peers randomly
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::vector<beast::IP::Endpoint> targets;

        auto sample_peers = [&](auto& map, auto& targets, auto& rng) {
            auto n = map.size();
            if (n == 0)
                return;
            auto k = n / 2 + 1;
            std::vector<beast::IP::Endpoint> keys;
            keys.reserve(n);
            for (auto const& p : map)
                keys.push_back(p.first);
            std::shuffle(keys.begin(), keys.end(), rng);
            targets.assign(keys.begin(), keys.begin() + std::min(k, n));
        };

        sample_peers(m_udp_highway_peers, targets, rng);

        // Determine max payload size based on endpoint types
        size_t max_data_v4 =
            kMaxDatagram - kIPv4Header - kUDPHeader - kHeaderSize;
        size_t max_data_v6 =
            kMaxDatagram - kIPv6Header - kUDPHeader - kHeaderSize;

        bool has_v6 = std::any_of(targets.begin(), targets.end(), [](auto& ep) {
            return ep.address().is_v6();
        });
        size_t max_data_size = has_v6 ? max_data_v6 : max_data_v4;

        uint32_t num_fragments =
            (tx.size() + max_data_size - 1) / max_data_size;

        for (uint32_t i = 0; i < num_fragments; ++i)
        {
            size_t offset = i * max_data_size;
            size_t chunk_size = std::min(max_data_size, tx.size() - offset);

            std::vector<uint8_t> packet(kHeaderSize + chunk_size);
            std::memcpy(packet.data(), "XUSHTXNF", 8);
            std::memcpy(packet.data() + 8, txid.data(), 32);
            *reinterpret_cast<uint32_t*>(packet.data() + 40) = htonl(tx.size());
            *reinterpret_cast<uint32_t*>(packet.data() + 44) =
                htonl(num_fragments);
            *reinterpret_cast<uint32_t*>(packet.data() + 48) = htonl(i);
            std::memcpy(packet.data() + 52, tx.data() + offset, chunk_size);

            for (auto& endpoint : targets)
            {
                if (endpoint.address().is_v4())
                {
                    sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(endpoint.port());
                    addr.sin_addr.s_addr =
                        htonl(endpoint.address().to_v4().to_uint());
                    ::sendto(
                        udp4_sock,
                        packet.data(),
                        packet.size(),
                        0,
                        reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr));
                }
                else
                {
                    sockaddr_in6 addr{};
                    addr.sin6_family = AF_INET6;
                    addr.sin6_port = htons(endpoint.port());
                    auto bytes = endpoint.address().to_v6().to_bytes();
                    std::memcpy(&addr.sin6_addr, bytes.data(), 16);
                    ::sendto(
                        udp6_sock,
                        packet.data(),
                        packet.size(),
                        0,
                        reinterpret_cast<sockaddr*>(&addr),
                        sizeof(addr));
                }
            }
        }
    }

    ManagerImp(
        boost::asio::io_service& io_service,
        clock_type& clock,
        beast::Journal journal,
        BasicConfig const& config,
        beast::insight::Collector::ptr const& collector,
        bool useSqLiteStore,
        Application& app)
        : Manager()
        , app_(app)
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
        // add fixed peers to superhighway
        {
            std::lock_guard<std::mutex> lock(m_udp_highway_mutex);
            uint32_t t = static_cast<uint32_t>(std::time(nullptr));
            for (auto const& a : addresses)
                m_udp_highway_peers.emplace(a, t);
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
        // add endpoints to superhighway
        {
            std::lock_guard<std::mutex> lock(m_udp_highway_mutex);
            uint32_t t = static_cast<uint32_t>(std::time(nullptr));
            for (auto const& a : endpoints)
                m_udp_highway_peers.emplace(a.address, t);
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
        // add redirects to superhighway
        {
            std::lock_guard<std::mutex> lock(m_udp_highway_mutex);
            uint32_t t = static_cast<uint32_t>(std::time(nullptr));
            for (auto const& a : eps)
                m_udp_highway_peers.emplace(
                    beast::IPAddressConversion::from_asio(a), t);
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

        // clean superhighway in an amortized fashion
        static std::mt19937 rng(std::random_device{}());
        {
            std::lock_guard<std::mutex> lock(m_udp_highway_mutex);

            uint32_t t = static_cast<uint32_t>(std::time(nullptr));

            std::cout << "m_udp_highway_peers size="
                      << m_udp_highway_peers.size() << "\n";
            for (auto const& [ep, ls] : m_udp_highway_peers)
            {
                std::cout << "ep: " << ep << " ls: " << ls << "\n";
            }

            for (int i = 0; i < std::min(3, (int)m_udp_highway_peers.size());
                 ++i)
            {
                auto it = std::next(
                    m_udp_highway_peers.begin(),
                    std::uniform_int_distribution<>(
                        0, m_udp_highway_peers.size() - 1)(rng));

                /* highway peers we haven't seen anything from for 500 seconds
                 * are removed */
                if (t - it->second > 500)
                    m_udp_highway_peers.erase(it);
            }
        }

        // we will also randomly choose some peers and send our peer list
        {
            static boost::asio::io_context io_ctx;
            static boost::asio::ip::udp::socket sock(
                io_ctx, boost::asio::ip::udp::v4());

            if (m_udp_highway_peers.empty())
                return;

            // Lambda to randomly sample N items from map
            auto sample = [&](size_t n) {
                std::vector<std::pair<beast::IP::Endpoint, uint32_t>> vec(
                    m_udp_highway_peers.begin(), m_udp_highway_peers.end());
                std::shuffle(vec.begin(), vec.end(), rng);
                vec.resize(std::min(n, vec.size()));
                return vec;
            };

            // Select up to 100 peers to encode
            auto peers_to_encode = sample(100);

            // Lambda to encode peers into binary packet
            // Format: [8 bytes: "XUSHPEER"][1 byte: IPv4 count][1 byte: IPv6
            // count][IPv4s][IPv6s]
            auto encode = [](const auto& peers) {
                std::vector<uint8_t> packet;
                packet.reserve(
                    10 +
                    peers.size() * 24);  // 8 magic + 2 header + max peer data

                // Separate IPv4 and IPv6
                std::vector<const beast::IP::Endpoint*> ipv4s, ipv6s;
                for (const auto& p : peers)
                {
                    if (p.first.address().is_v4())
                        ipv4s.push_back(&p.first);
                    else
                        ipv6s.push_back(&p.first);
                }

                // Magic code: XUSHPEER
                const char* magic = "XUSHPEER";
                packet.insert(packet.end(), magic, magic + 8);

                // Header: [IPv4 count][IPv6 count]
                packet.push_back(static_cast<uint8_t>(ipv4s.size()));
                packet.push_back(static_cast<uint8_t>(ipv6s.size()));

                // Pack IPv4s (4 bytes IP + 4 bytes port)
                for (auto ep : ipv4s)
                {
                    auto v4 = ep->address().to_v4().to_bytes();
                    packet.insert(packet.end(), v4.begin(), v4.end());
                    uint32_t port = htonl(ep->port());
                    packet.insert(
                        packet.end(), (uint8_t*)&port, (uint8_t*)&port + 4);
                }

                // Pack IPv6s (16 bytes IP + 4 bytes port)
                for (auto ep : ipv6s)
                {
                    auto v6 = ep->address().to_v6().to_bytes();
                    packet.insert(packet.end(), v6.begin(), v6.end());
                    uint32_t port = htonl(ep->port());
                    packet.insert(
                        packet.end(), (uint8_t*)&port, (uint8_t*)&port + 4);
                }

                return packet;
            };

            auto packet = encode(peers_to_encode);

            // Select 20 peers to send to (re-roll, overlap is fine)
            auto targets = sample(20);

            // Send packet to each target
            for (const auto& [endpoint, _] : targets)
            {
                try
                {
                    boost::asio::ip::udp::endpoint udp_ep(
                        endpoint.address(), endpoint.port());
                    sock.send_to(boost::asio::buffer(packet), udp_ep);
                }
                catch (...)
                {
                    // Silent fail, continue to next peer
                }
            }
        }
    }

    std::vector<std::pair<std::shared_ptr<Slot>, std::vector<Endpoint>>>
    buildEndpointsForPeers() override
    {
        return m_logic.buildEndpointsForPeers();
    }

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
    bool useSqLiteStore,
    Application& app)
{
    return std::make_unique<ManagerImp>(
        io_service, clock, journal, config, collector, useSqLiteStore, app);
}

}  // namespace PeerFinder
}  // namespace ripple
