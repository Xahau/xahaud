//------------------------------------------------------------------------------
/*
    This file is part of Beast: https://github.com/vinniefalco/Beast
    Copyright 2013, Vinnie Falco <vinnie.falco@gmail.com>

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

// MODULES: ../impl/IPEndpoint.cpp

#include <test/beast/IPEndpointCommon.h>
#include <xrpl/basics/random.h>
#include <xrpl/beast/net/IPEndpoint.h>
#include <xrpl/beast/unit_test.h>
#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/predef.h>
#include <typeinfo>

namespace beast {
namespace IP {

//------------------------------------------------------------------------------

class IPEndpoint_test : public unit_test::suite
{
public:
    void
    testAddressV4()
    {
        testcase("IPv4 address private/public classification");

        using namespace boost::asio::ip;

        // 10.0.0.0/8 range
        BEAST_EXPECT(is_private(make_address_v4("10.0.0.0")));
        BEAST_EXPECT(is_private(make_address_v4("10.255.255.255")));
        BEAST_EXPECT(!is_private(make_address_v4("11.0.0.0")));

        // 172.16.0.0/12 range
        BEAST_EXPECT(is_private(make_address_v4("172.16.0.0")));
        BEAST_EXPECT(is_private(make_address_v4("172.31.255.255")));
        BEAST_EXPECT(!is_private(make_address_v4("172.15.255.255")));
        BEAST_EXPECT(!is_private(make_address_v4("172.32.0.0")));

        // 192.168.0.0/16 range
        BEAST_EXPECT(is_private(make_address_v4("192.168.0.0")));
        BEAST_EXPECT(is_private(make_address_v4("192.168.255.255")));
        BEAST_EXPECT(!is_private(make_address_v4("192.167.255.255")));

        // Loopback is considered private
        BEAST_EXPECT(is_private(make_address_v4("127.0.0.1")));

        // Public addresses
        BEAST_EXPECT(is_public(make_address_v4("8.8.8.8")));
        BEAST_EXPECT(is_public(make_address_v4("1.1.1.1")));

        // Multicast is not public (even though not private)
        BEAST_EXPECT(!is_private(make_address_v4("224.0.0.1")));
        BEAST_EXPECT(!is_public(make_address_v4("224.0.0.1")));
    }

    //--------------------------------------------------------------------------

    void
    shouldParseEPV4(
        std::string const& s,
        boost::asio::ip::address_v4::bytes_type const& value,
        std::uint16_t p,
        std::string const& normal = "")
    {
        auto const result = Endpoint::from_string_checked(s);
        if (!BEAST_EXPECT(result))
            return;
        if (!BEAST_EXPECT(result->address().is_v4()))
            return;
        if (!BEAST_EXPECT(
                result->address().to_v4() ==
                boost::asio::ip::address_v4{value}))
            return;

        BEAST_EXPECT(result->port() == p);
        BEAST_EXPECT(to_string(*result) == (normal.empty() ? s : normal));
    }

    void
    shouldParseEPV6(
        std::string const& s,
        boost::asio::ip::address_v6::bytes_type const& value,
        std::uint16_t p,
        std::string const& normal = "")
    {
        auto result = Endpoint::from_string_checked(s);
        if (!BEAST_EXPECT(result))
            return;
        if (!BEAST_EXPECT(result->address().is_v6()))
            return;
        if (!BEAST_EXPECT(
                result->address().to_v6() ==
                boost::asio::ip::address_v6{value}))
            return;

        BEAST_EXPECT(result->port() == p);
        BEAST_EXPECT(to_string(*result) == (normal.empty() ? s : normal));
    }

    void
    failParseEP(std::string s)
    {
        auto a1 = Endpoint::from_string(s);
        BEAST_EXPECTS(is_unspecified(a1), s + " parses as " + a1.to_string());

        auto a2 = Endpoint::from_string(s);
        BEAST_EXPECTS(is_unspecified(a2), s + " parses as " + a2.to_string());

        boost::replace_last(s, ":", " ");
        auto a3 = Endpoint::from_string(s);
        BEAST_EXPECTS(is_unspecified(a3), s + " parses as " + a3.to_string());
    }

    void
    testEndpoint()
    {
        testcase("Endpoint");

        using namespace boost::asio::ip;

        shouldParseEPV4("1.2.3.4", {{1, 2, 3, 4}}, 0);
        shouldParseEPV4("1.2.3.4:5", {{1, 2, 3, 4}}, 5);
        shouldParseEPV4("1.2.3.4 5", {{1, 2, 3, 4}}, 5, "1.2.3.4:5");
        // leading, trailing space
        shouldParseEPV4("   1.2.3.4:5", {{1, 2, 3, 4}}, 5, "1.2.3.4:5");
        shouldParseEPV4("1.2.3.4:5    ", {{1, 2, 3, 4}}, 5, "1.2.3.4:5");
        shouldParseEPV4("1.2.3.4   ", {{1, 2, 3, 4}}, 0, "1.2.3.4");
        shouldParseEPV4("  1.2.3.4", {{1, 2, 3, 4}}, 0, "1.2.3.4");
        shouldParseEPV6(
            "2001:db8:a0b:12f0::1",
            {{32, 01, 13, 184, 10, 11, 18, 240, 0, 0, 0, 0, 0, 0, 0, 1}},
            0);
        shouldParseEPV6(
            "[2001:db8:a0b:12f0::1]:8",
            {{32, 01, 13, 184, 10, 11, 18, 240, 0, 0, 0, 0, 0, 0, 0, 1}},
            8);
        shouldParseEPV6(
            "[2001:2002:2003:2004:2005:2006:2007:2008]:65535",
            {{32, 1, 32, 2, 32, 3, 32, 4, 32, 5, 32, 6, 32, 7, 32, 8}},
            65535);
        shouldParseEPV6(
            "2001:2002:2003:2004:2005:2006:2007:2008 65535",
            {{32, 1, 32, 2, 32, 3, 32, 4, 32, 5, 32, 6, 32, 7, 32, 8}},
            65535,
            "[2001:2002:2003:2004:2005:2006:2007:2008]:65535");

        Endpoint ep;

        address_v4::bytes_type d = {{127, 0, 0, 1}};
        ep = Endpoint(address_v4{d}, 80);
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(!is_public(ep));
        BEAST_EXPECT(is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(is_loopback(ep));
        BEAST_EXPECT(to_string(ep) == "127.0.0.1:80");
        // same address as v4 mapped in ipv6
        ep = Endpoint(address_v6::v4_mapped(address_v4{d}), 80);
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(!is_public(ep));
        BEAST_EXPECT(is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(!is_loopback(ep));  // mapped loopback is not a loopback
        BEAST_EXPECTS(to_string(ep) == "[::ffff:127.0.0.1]:80", to_string(ep));

        d = {{10, 0, 0, 1}};
        ep = Endpoint(address_v4{d});
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(!is_public(ep));
        BEAST_EXPECT(is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(!is_loopback(ep));
        BEAST_EXPECT(to_string(ep) == "10.0.0.1");
        // same address as v4 mapped in ipv6
        ep = Endpoint(address_v6::v4_mapped(address_v4{d}));
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(!is_public(ep));
        BEAST_EXPECT(is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(!is_loopback(ep));
        BEAST_EXPECTS(to_string(ep) == "::ffff:10.0.0.1", to_string(ep));

        d = {{166, 78, 151, 147}};
        ep = Endpoint(address_v4{d});
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(is_public(ep));
        BEAST_EXPECT(!is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(!is_loopback(ep));
        BEAST_EXPECT(to_string(ep) == "166.78.151.147");
        // same address as v4 mapped in ipv6
        ep = Endpoint(address_v6::v4_mapped(address_v4{d}));
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(is_public(ep));
        BEAST_EXPECT(!is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(!is_loopback(ep));
        BEAST_EXPECTS(to_string(ep) == "::ffff:166.78.151.147", to_string(ep));

        // a private IPv6
        address_v6::bytes_type d2 = {
            {253, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
        ep = Endpoint(address_v6{d2});
        BEAST_EXPECT(!is_unspecified(ep));
        BEAST_EXPECT(!is_public(ep));
        BEAST_EXPECT(is_private(ep));
        BEAST_EXPECT(!is_multicast(ep));
        BEAST_EXPECT(!is_loopback(ep));
        BEAST_EXPECTS(to_string(ep) == "fd00::1", to_string(ep));

        {
            ep = Endpoint::from_string("192.0.2.112");
            BEAST_EXPECT(!is_unspecified(ep));
            BEAST_EXPECT(ep == Endpoint::from_string("192.0.2.112"));

            auto const ep1 = Endpoint::from_string("192.0.2.112:2016");
            BEAST_EXPECT(!is_unspecified(ep1));
            BEAST_EXPECT(ep.address() == ep1.address());
            BEAST_EXPECT(ep1.port() == 2016);

            auto const ep2 = Endpoint::from_string("192.0.2.112:2016");
            BEAST_EXPECT(!is_unspecified(ep2));
            BEAST_EXPECT(ep.address() == ep2.address());
            BEAST_EXPECT(ep2.port() == 2016);
            BEAST_EXPECT(ep1 == ep2);

            auto const ep3 = Endpoint::from_string("192.0.2.112 2016");
            BEAST_EXPECT(!is_unspecified(ep3));
            BEAST_EXPECT(ep.address() == ep3.address());
            BEAST_EXPECT(ep3.port() == 2016);
            BEAST_EXPECT(ep2 == ep3);

            auto const ep4 = Endpoint::from_string("192.0.2.112     2016");
            BEAST_EXPECT(!is_unspecified(ep4));
            BEAST_EXPECT(ep.address() == ep4.address());
            BEAST_EXPECT(ep4.port() == 2016);
            BEAST_EXPECT(ep3 == ep4);

            BEAST_EXPECT(to_string(ep1) == to_string(ep2));
            BEAST_EXPECT(to_string(ep1) == to_string(ep3));
            BEAST_EXPECT(to_string(ep1) == to_string(ep4));
        }

        {
            ep = Endpoint::from_string("[::]:2017");
            BEAST_EXPECT(is_unspecified(ep));
            BEAST_EXPECT(ep.port() == 2017);
            BEAST_EXPECT(ep.address() == address_v6{});
        }

        // Failures:
        failParseEP("192.0.2.112:port");
        failParseEP("ip:port");
        failParseEP("");
        failParseEP("1.2.3.256");

#if BOOST_OS_WINDOWS
        // windows asio bugs...false positives
        shouldParseEPV4("255", {{0, 0, 0, 255}}, 0, "0.0.0.255");
        shouldParseEPV4("512", {{0, 0, 2, 0}}, 0, "0.0.2.0");
        shouldParseEPV4("1.2.3:80", {{1, 2, 0, 3}}, 80, "1.2.0.3:80");
#else
        failParseEP("255");
        failParseEP("512");
        failParseEP("1.2.3:80");
#endif

        failParseEP("1.2.3.4:65536");
        failParseEP("1.2.3.4:89119");
        failParseEP("1.2.3:89119");
        failParseEP("[::1]:89119");
        failParseEP("[::az]:1");
        failParseEP("[1234:5678:90ab:cdef:1234:5678:90ab:cdef:1111]:1");
        failParseEP("[1234:5678:90ab:cdef:1234:5678:90ab:cdef:1111]:12345");
        failParseEP("abcdef:12345");
        failParseEP("[abcdef]:12345");
        failParseEP("foo.org 12345");

        // test with hashed container
        std::unordered_set<Endpoint> eps;
        constexpr auto items{100};
        float max_lf{0};
        for (auto i = 0; i < items; ++i)
        {
            eps.insert(randomEP(ripple::rand_int(0, 1) == 1));
            max_lf = std::max(max_lf, eps.load_factor());
        }
        BEAST_EXPECT(eps.bucket_count() >= items);
        BEAST_EXPECT(max_lf > 0.90);
    }

    //--------------------------------------------------------------------------

    bool
    parse(std::string const& text, Endpoint& t)
    {
        if (auto result = Endpoint::from_string_checked(text))
        {
            t = *result;
            return true;
        }

        return false;
    }

    template <typename T>
    void
    shouldPass(std::string const& text, std::string const& normal = "")
    {
        using namespace std::literals;
        T t;
        BEAST_EXPECT(parse(text, t));
        BEAST_EXPECTS(
            to_string(t) == (normal.empty() ? text : normal),
            "string mismatch for "s + text);
    }

    template <typename T>
    void
    shouldFail(std::string const& text)
    {
        T t;
        unexpected(parse(text, t), text + " should not parse");
    }

    template <typename T>
    void
    testParse(char const* name)
    {
        testcase(name);

        shouldPass<T>("0.0.0.0");
        shouldPass<T>("192.168.0.1");
        shouldPass<T>("168.127.149.132");
        shouldPass<T>("168.127.149.132:80");
        shouldPass<T>("168.127.149.132:54321");
        shouldPass<T>("2001:db8:a0b:12f0::1");
        shouldPass<T>("[2001:db8:a0b:12f0::1]:8");
        shouldPass<T>("2001:db8:a0b:12f0::1 8", "[2001:db8:a0b:12f0::1]:8");
        shouldPass<T>("[::1]:8");
        shouldPass<T>("[2001:2002:2003:2004:2005:2006:2007:2008]:65535");

        shouldFail<T>("1.2.3.256");
        shouldFail<T>("");
#if BOOST_OS_WINDOWS
        // windows asio bugs...false positives
        shouldPass<T>("512", "0.0.2.0");
        shouldPass<T>("255", "0.0.0.255");
        shouldPass<T>("1.2.3:80", "1.2.0.3:80");
#else
        shouldFail<T>("512");
        shouldFail<T>("255");
        shouldFail<T>("1.2.3:80");
#endif
        shouldFail<T>("1.2.3:65536");
        shouldFail<T>("1.2.3:72131");
        shouldFail<T>("[::1]:89119");
        shouldFail<T>("[::az]:1");
        shouldFail<T>("[1234:5678:90ab:cdef:1234:5678:90ab:cdef:1111]:1");
        shouldFail<T>("[1234:5678:90ab:cdef:1234:5678:90ab:cdef:1111]:12345");
    }

    void
    run() override
    {
        testAddressV4();
        testEndpoint();
        testParse<Endpoint>("Parse Endpoint");
    }
};

BEAST_DEFINE_TESTSUITE(IPEndpoint, net, beast);

}  // namespace IP
}  // namespace beast
