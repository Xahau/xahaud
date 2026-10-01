//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <test/unit_test/SuiteJournal.h>
#include <xrpld/peerfinder/PeerfinderManager.h>
#include <xrpld/peerfinder/detail/Tuning.h>
#include <xrpld/peerfinder/make_Manager.h>
#include <xrpl/basics/BasicConfig.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/insight/NullCollector.h>
#include <xrpl/beast/net/IPAddressConversion.h>
#include <xrpl/beast/unit_test.h>

#include <boost/asio/io_service.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ripple {
namespace PeerFinder {

// Tests the UDP Superhighway (XUSH) endpoint set kept by PeerFinder::Manager
class HighwayPeers_test : public beast::unit_test::suite
{
    test::SuiteJournal journal_{"HighwayPeers_test", *this};

    struct Fixture
    {
        boost::asio::io_service io;
        TestStopwatch clock;
        BasicConfig basicConfig;
        std::unique_ptr<Manager> manager;

        Fixture(beast::Journal journal, bool enabled, bool peerPrivate)
            : manager(make_Manager(
                  io,
                  clock,
                  journal,
                  basicConfig,
                  beast::insight::NullCollector::New(),
                  false))
        {
            Config config;
            config.udpHighway = enabled;
            config.peerPrivate = peerPrivate;
            manager->setConfig(config);
        }

        ~Fixture()
        {
            manager->stop();
        }
    };

    static beast::IP::Endpoint
    ep(std::string const& s)
    {
        return beast::IP::Endpoint::from_string(s);
    }

    // A distinct public IPv4 endpoint
    static beast::IP::Endpoint
    publicEp(std::uint32_t i)
    {
        return beast::IP::Endpoint(
            boost::asio::ip::address_v4(0x2C000000 + i), 51235);
    }

    static std::set<beast::IP::Endpoint>
    asSet(std::vector<beast::IP::Endpoint> const& v)
    {
        return {v.begin(), v.end()};
    }

    void
    testDisabled()
    {
        testcase("disabled");
        Fixture f(journal_, false, false);

        f.manager->addFixedPeer("fixed", {ep("10.0.0.1:51235")});
        f.manager->add_highway_peers({ep("8.8.8.8:51235")});
        f.manager->onRedirects(
            beast::IP::to_asio_endpoint(ep("8.8.4.4:51235")),
            {beast::IP::to_asio_endpoint(ep("1.1.1.1:51235"))});

        BEAST_EXPECT(f.manager->highway_targets(100).empty());
        BEAST_EXPECT(f.manager->highway_adverts(100).empty());
    }

    void
    testLearning()
    {
        testcase("learning");
        Fixture f(journal_, true, false);

        // Only valid public endpoints are learned from other servers
        f.manager->add_highway_peers(
            {ep("8.8.8.8:51235"),
             ep("10.0.0.1:51235"),
             ep("127.0.0.1:51235"),
             ep("9.9.9.9:0"),
             ep("0.0.0.0:51235")});
        BEAST_EXPECT(
            asSet(f.manager->highway_targets(100)) ==
            std::set<beast::IP::Endpoint>{ep("8.8.8.8:51235")});

        // Fixed peers are trusted, even with a private address, but they
        // are never advertised
        f.manager->addFixedPeer("fixed", {ep("10.0.0.2:51235")});
        BEAST_EXPECT(
            asSet(f.manager->highway_targets(100)) ==
            (std::set<beast::IP::Endpoint>{
                ep("8.8.8.8:51235"), ep("10.0.0.2:51235")}));
        BEAST_EXPECT(
            asSet(f.manager->highway_adverts(100)) ==
            std::set<beast::IP::Endpoint>{ep("8.8.8.8:51235")});

        // A learned endpoint that is later configured as fixed stops being
        // advertised
        f.manager->addFixedPeer("fixed2", {ep("8.8.8.8:51235")});
        BEAST_EXPECT(f.manager->highway_adverts(100).empty());
        BEAST_EXPECT(f.manager->highway_targets(100).size() == 2);

        // Redirects are learned too
        f.manager->onRedirects(
            beast::IP::to_asio_endpoint(ep("8.8.4.4:51235")),
            {beast::IP::to_asio_endpoint(ep("1.1.1.1:51235"))});
        BEAST_EXPECT(f.manager->highway_targets(100).size() == 3);
    }

    void
    testSampling()
    {
        testcase("sampling");
        Fixture f(journal_, true, false);

        std::vector<beast::IP::Endpoint> endpoints;
        for (std::uint32_t i = 0; i < 100; ++i)
            endpoints.push_back(publicEp(i));
        f.manager->add_highway_peers(endpoints);

        for (int i = 0; i < 10; ++i)
        {
            auto const targets = f.manager->highway_targets(16);
            BEAST_EXPECT(targets.size() == 16);
            BEAST_EXPECT(asSet(targets).size() == 16);
        }

        BEAST_EXPECT(
            asSet(f.manager->highway_targets(1000)) == asSet(endpoints));
        BEAST_EXPECT(f.manager->highway_targets(0).empty());
    }

    void
    testPrivate()
    {
        testcase("private");
        Fixture f(journal_, true, true);

        f.manager->addFixedPeer("fixed", {ep("10.0.0.2:51235")});
        f.manager->add_highway_peers({ep("8.8.8.8:51235")});

        // A private server only talks to its fixed peers, and advertises
        // nothing
        BEAST_EXPECT(
            asSet(f.manager->highway_targets(100)) ==
            std::set<beast::IP::Endpoint>{ep("10.0.0.2:51235")});
        BEAST_EXPECT(f.manager->highway_adverts(100).empty());
    }

    void
    testExpiry()
    {
        testcase("expiry");
        using namespace std::chrono_literals;
        Fixture f(journal_, true, false);

        auto const ttl = Tuning::highwayPeerSecondsToLive;
        auto const advertised = ep("8.8.8.8:51235");
        auto const redirected = ep("1.1.1.1:51235");
        auto const fixed = ep("10.0.0.2:51235");

        f.manager->addFixedPeer("fixed", {fixed});
        f.manager->add_highway_peers({advertised});
        f.manager->add_highway_peers({redirected});

        // Advertisements don't extend the life of an entry, but
        // authenticated sources (here, a redirect) do
        f.clock.advance(ttl / 2);
        f.manager->add_highway_peers({advertised, redirected});
        f.manager->onRedirects(
            beast::IP::to_asio_endpoint(ep("8.8.4.4:51235")),
            {beast::IP::to_asio_endpoint(redirected)});

        f.clock.advance(ttl / 2);
        f.manager->once_per_second();
        BEAST_EXPECT(f.manager->highway_targets(100).size() == 3);

        f.clock.advance(1s);
        f.manager->once_per_second();
        BEAST_EXPECT(
            asSet(f.manager->highway_targets(100)) ==
            (std::set<beast::IP::Endpoint>{redirected, fixed}));

        f.clock.advance(ttl / 2);
        f.manager->once_per_second();
        BEAST_EXPECT(
            asSet(f.manager->highway_targets(100)) ==
            std::set<beast::IP::Endpoint>{fixed});

        // An expired entry can be learned again
        f.manager->add_highway_peers({advertised});
        BEAST_EXPECT(f.manager->highway_targets(100).size() == 2);
    }

    void
    testLimit()
    {
        testcase("limit");
        Fixture f(journal_, true, false);

        auto const max = Tuning::highwayPeersMax;

        std::vector<beast::IP::Endpoint> endpoints;
        for (std::uint32_t i = 0; i < max + 100; ++i)
            endpoints.push_back(publicEp(i));
        f.manager->add_highway_peers(endpoints);
        BEAST_EXPECT(f.manager->highway_targets(max + 100).size() == max);

        // Fixed peers don't count towards the limit
        f.manager->addFixedPeer("fixed", {ep("10.0.0.2:51235")});
        BEAST_EXPECT(f.manager->highway_targets(max + 100).size() == max + 1);

        // Disabling the highway forgets everything
        Config config;
        config.udpHighway = false;
        f.manager->setConfig(config);
        BEAST_EXPECT(f.manager->highway_targets(max + 100).empty());
    }

public:
    void
    run() override
    {
        testDisabled();
        testLearning();
        testSampling();
        testPrivate();
        testExpiry();
        testLimit();
    }
};

BEAST_DEFINE_TESTSUITE(HighwayPeers, peerfinder, ripple);

}  // namespace PeerFinder
}  // namespace ripple
