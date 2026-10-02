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

// End-to-end tests for "pwa" ports: startup validation, proxy and
// X-Forwarded-For rules, routing, headers, ETag/304, connection handling,
// metering, the pwa_info RPC, and the browser-Origin admin hardening that
// accompanies them. These talk to the node over real sockets.

#include <test/jtx.h>
#include <test/jtx/JSONRPCClient.h>
#include <test/jtx/envconfig.h>
#include <xrpld/core/Config.h>
#include <xrpld/rpc/ServerHandler.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/beast/test/yield_to.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Charge.h>
#include <xrpl/resource/ResourceManager.h>
#include <xrpl/resource/detail/Tuning.h>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>

namespace ripple {
namespace test {

class PWAServe_test : public beast::unit_test::suite,
                      public beast::test::enable_yield_to
{
    using response_t =
        boost::beast::http::response<boost::beast::http::string_body>;
    using Headers = std::vector<std::pair<std::string, std::string>>;

    static std::string
    doc()
    {
        return "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">"
               "<title>t</title></head><body><script>1</script></body>"
               "</html>\n";
    }

    // A config with a working pwa port behind a "proxy" at the test's own
    // address. `tweak` may edit the [port_pwa] section.
    static std::unique_ptr<Config>
    pwaConfig(std::function<void(Section&)> const& tweak = {})
    {
        auto cfg = jtx::envconfig();
        (*cfg)["server"].append("port_pwa");
        auto& s = (*cfg)["port_pwa"];
        s.set("ip", getEnvLocalhostAddr());
        s.set("port", "0");
        s.set("protocol", "pwa");
        s.set("secure_gateway", getEnvLocalhostAddr());
        if (tweak)
            tweak(s);
        return cfg;
    }

    // Send raw bytes to a configured port and read until the server closes
    // the connection (or 5s pass, which a pwa port must never need).
    std::string
    exchange(
        jtx::Env& env,
        std::string const& raw,
        boost::asio::yield_context& yield,
        std::string const& section = "port_pwa",
        bool* closedByPeer = nullptr)
    {
        using namespace boost::asio;
        auto const ip = *env.app().config()[section].get<std::string>("ip");
        auto const port =
            *env.app().config()[section].get<std::uint16_t>("port");

        boost::system::error_code ec;
        io_service& ios = get_io_service();
        ip::tcp::socket sock{ios};
        sock.async_connect(
            ip::tcp::endpoint{ip::make_address(ip), port}, yield[ec]);
        if (!BEAST_EXPECT(!ec))
            return {};

        async_write(sock, buffer(raw), yield[ec]);
        if (!BEAST_EXPECT(!ec))
            return {};

        steady_timer timer{ios};
        bool timedOut = false;
        timer.expires_after(std::chrono::seconds(5));
        timer.async_wait([&](boost::system::error_code const& e) {
            if (!e)
            {
                timedOut = true;
                sock.close();
            }
        });

        std::string out;
        std::array<char, 4096> buf;
        for (;;)
        {
            auto const n = sock.async_read_some(buffer(buf), yield[ec]);
            out.append(buf.data(), n);
            if (ec)
                break;
        }
        timer.cancel();
        // Unread pipelined bytes make the server's close an RST rather
        // than a FIN, so either counts as the server ending the connection.
        if (closedByPeer)
            *closedByPeer = !timedOut &&
                (ec == error::eof || ec == error::connection_reset);
        return out;
    }

    static std::string
    rawGet(
        std::string const& target,
        Headers const& headers,
        std::string const& method = "GET",
        std::string const& body = "")
    {
        std::string r = method + " " + target + " HTTP/1.1\r\n";
        r += "Host: node\r\n";
        for (auto const& [k, v] : headers)
            r += k + ": " + v + "\r\n";
        if (!body.empty())
            r += "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n";
        r += "\r\n" + body;
        return r;
    }

    static std::optional<response_t>
    parse(std::string const& raw)
    {
        boost::beast::http::response_parser<boost::beast::http::string_body> p;
        p.eager(true);
        boost::system::error_code ec;
        p.put(boost::asio::buffer(raw), ec);
        if (!ec && !p.is_done())
            p.put_eof(ec);
        if (ec || !p.is_done())
            return std::nullopt;
        return p.release();
    }

    response_t
    get(jtx::Env& env,
        std::string const& target,
        boost::asio::yield_context& yield,
        Headers headers = {{"X-Forwarded-For", "203.0.113.7"}},
        std::string const& method = "GET")
    {
        auto const raw = exchange(env, rawGet(target, headers, method), yield);
        auto r = parse(raw);
        BEAST_EXPECTS(r.has_value(), raw);
        return r ? std::move(*r) : response_t{};
    }

    static std::string
    hdr(response_t const& r, std::string const& name)
    {
        auto const it = r.find(name);
        return it == r.end()
            ? std::string{}
            : std::string(it->value().data(), it->value().size());
    }

    static bool
    servedAsHtml(response_t const& r)
    {
        return r.result_int() == 200 &&
            hdr(r, "Content-Type") == "text/html; charset=utf-8";
    }

    void
    publish(jtx::Env& env, jtx::Account const& a, std::string const& d)
    {
        using namespace jtx;
        auto jt = noop(a);
        jt[sfAppLoader.fieldName] = strHex(d);
        env(jt, fee(XRP(1)));
    }

    // What one lookup that reaches the ledger costs, hit or miss:
    // feeReferenceRPC + feeMediumBurdenRPC.
    static constexpr int lookupCost = 20 + 400;

    // Charge the resource consumer for `ip` to `misses` lookups short of the
    // drop threshold. Driving it there with real requests would take
    // thousands of them, and the decay window would make the count depend
    // on how fast they run.
    static void
    chargeNearDrop(jtx::Env& env, std::string const& ip, int misses)
    {
        auto c = env.app().getResourceManager().newInboundEndpoint(
            beast::IP::Endpoint::from_string(ip));
        c.charge(Resource::Charge(
            Resource::dropThreshold * Resource::decayWindowSeconds -
                misses * lookupCost,
            "test"));
    }

    // Send misses for `xff` until one is throttled. The first must not be:
    // that is what shows the misses themselves pushed it over. A few more
    // than `chargeNearDrop` left room for are allowed, since a decay tick
    // may land in between.
    bool
    missesThrottle(
        jtx::Env& env,
        std::string const& xff,
        boost::asio::yield_context& yield)
    {
        jtx::Account const nobody{"nobody"};  // never funded: always 404
        for (int i = 0; i < 200; ++i)
        {
            auto const r = get(
                env, "/" + nobody.human(), yield, {{"X-Forwarded-For", xff}});
            if (r.result_int() == 503)
                return i > 0;
            if (!BEAST_EXPECT(r.result_int() == 404))
                return false;
        }
        return false;
    }

    //--------------------------------------------------------------------------

    void
    testConfigValidation()
    {
        testcase("pwa port configuration is validated at startup");

        auto accepts = [&](std::function<void(Section&)> tweak) {
            auto cfg = pwaConfig(tweak);
            try
            {
                setup_ServerHandler(*cfg, std::stringstream{});
                return true;
            }
            catch (std::exception const&)
            {
                return false;
            }
        };

        BEAST_EXPECT(accepts({}));
        BEAST_EXPECT(
            !accepts([](Section& s) { s.set("protocol", "pwa,http"); }));
        BEAST_EXPECT(!accepts([](Section& s) { s.set("protocol", "pwa,ws"); }));
        BEAST_EXPECT(
            !accepts([](Section& s) { s.set("protocol", "pwa,peer"); }));
        BEAST_EXPECT(!accepts([](Section& s) { s.set("secure_gateway", ""); }));
        BEAST_EXPECT(!accepts(
            [](Section& s) { s.set("admin", getEnvLocalhostAddr()); }));
        BEAST_EXPECT(!accepts([](Section& s) {
            s.set("admin", "10.255.255.254");
            s.set("admin_user", "u");
            s.set("admin_password", "p");
        }));
        BEAST_EXPECT(!accepts([](Section& s) {
            s.set("user", "u");
            s.set("password", "p");
        }));
        BEAST_EXPECT(
            !accepts([](Section& s) { s.set("ssl_key", "/nonexistent"); }));
        BEAST_EXPECT(
            !accepts([](Section& s) { s.set("ssl_cert", "/nonexistent"); }));

        // A gateway covering every address would make every client a proxy,
        // free to write its own X-Forwarded-For.
        for (auto const gw : {"0.0.0.0", "::", "0.0.0.0/0", "::/0"})
            BEAST_EXPECTS(
                !accepts([gw](Section& s) { s.set("secure_gateway", gw); }),
                gw);
        BEAST_EXPECT(
            accepts([](Section& s) { s.set("secure_gateway", "10.0.0.0/8"); }));

        // admin_origin is parsed for every port.
        for (auto const bad :
             {"null",
              "NULL",
              "http://",
              "ftp://tool.example",
              "localhost:8080",
              "http://tool.example/",
              "http://tool.example/path",
              "http://*.example",
              "http://user@tool.example",
              "http://tool example"})
            BEAST_EXPECTS(
                !accepts([bad](Section& s) { s.set("admin_origin", bad); }),
                bad);
        for (auto const good :
             {"*",
              "http://localhost:8080",
              "https://Tool.Example, http://127.0.0.1:3000"})
            BEAST_EXPECTS(
                accepts([good](Section& s) { s.set("admin_origin", good); }),
                good);
    }

    void
    testServeAndHeaders(boost::asio::yield_context& yield)
    {
        testcase("serve and headers");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        publish(env, alice, doc());
        env.close();

        bool closed = false;
        auto const raw = exchange(
            env,
            rawGet("/" + alice.human(), {{"X-Forwarded-For", "203.0.113.7"}}),
            yield,
            "port_pwa",
            &closed);
        BEAST_EXPECT(closed);
        auto const r = parse(raw);
        if (!BEAST_EXPECT(r))
            return;

        BEAST_EXPECT(servedAsHtml(*r));
        BEAST_EXPECT(r->body() == doc());
        BEAST_EXPECT(hdr(*r, "Content-Length") == std::to_string(doc().size()));
        BEAST_EXPECT(hdr(*r, "Connection") == "close");
        BEAST_EXPECT(hdr(*r, "Server") == systemName());
        BEAST_EXPECT(
            hdr(*r, "Content-Security-Policy") ==
            "sandbox allow-scripts allow-forms allow-popups");
        BEAST_EXPECT(hdr(*r, "X-Content-Type-Options") == "nosniff");
        BEAST_EXPECT(hdr(*r, "X-Frame-Options") == "DENY");
        BEAST_EXPECT(hdr(*r, "Referrer-Policy") == "no-referrer");
        BEAST_EXPECT(hdr(*r, "Cross-Origin-Resource-Policy") == "same-origin");
        BEAST_EXPECT(hdr(*r, "Cross-Origin-Opener-Policy") == "same-origin");
        BEAST_EXPECT(hdr(*r, "Cache-Control") == "no-cache");

        // ETag is the quoted PreviousTxnID of the object.
        auto const sle = env.le(keylet::appLoader(alice.id()));
        if (BEAST_EXPECT(sle))
            BEAST_EXPECT(
                hdr(*r, "ETag") ==
                "\"" + to_string(sle->getFieldH256(sfPreviousTxnID)) + "\"");

        // Errors carry the baseline headers, are never HTML and never cached.
        auto const miss = get(env, "/" + bob.human(), yield);
        BEAST_EXPECT(miss.result_int() == 404);
        BEAST_EXPECT(!servedAsHtml(miss));
        BEAST_EXPECT(hdr(miss, "X-Content-Type-Options") == "nosniff");
        BEAST_EXPECT(hdr(miss, "Cache-Control") == "no-store");
        BEAST_EXPECT(hdr(miss, "Connection") == "close");
        BEAST_EXPECT(hdr(miss, "ETag").empty());

        // Overwrite, then remove.
        std::string const v2 = "<html><body>v2</body></html>";
        publish(env, alice, v2);
        env.close();
        BEAST_EXPECT(get(env, "/" + alice.human(), yield).body() == v2);
        {
            auto jt = noop(alice);
            jt[sfAppLoader.fieldName] = "";
            env(jt);
            env.close();
        }
        BEAST_EXPECT(get(env, "/" + alice.human(), yield).result_int() == 404);
    }

    void
    testETag(boost::asio::yield_context& yield)
    {
        testcase("ETag and If-None-Match");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        env.close();

        auto const target = "/" + alice.human();
        auto const first = get(env, target, yield);
        auto const etag = hdr(first, "ETag");
        BEAST_EXPECT(etag.size() == 66);

        auto inm = [&](std::string const& v) {
            return get(
                env,
                target,
                yield,
                {{"X-Forwarded-For", "203.0.113.7"}, {"If-None-Match", v}});
        };

        for (auto const& v :
             {etag,
              "W/" + etag,
              std::string("\"nope\", ") + etag,
              std::string(" \"a\" ,\t") + etag + " ",
              std::string("*")})
        {
            auto const r = inm(v);
            BEAST_EXPECTS(r.result_int() == 304, v);
            BEAST_EXPECT(r.body().empty());
            BEAST_EXPECT(hdr(r, "Content-Length").empty());
            BEAST_EXPECT(hdr(r, "ETag") == etag);
            BEAST_EXPECT(hdr(r, "Cache-Control") == "no-cache");
            BEAST_EXPECT(hdr(r, "Connection") == "close");
        }

        for (auto const& v :
             {std::string("\"nope\""),
              etag.substr(1, 64),  // unquoted
              std::string("W/\"") + std::string(64, '0') + "\""})
        {
            BEAST_EXPECTS(inm(v).result_int() == 200, v);
        }

        // A change to the document changes the tag; the old one no longer
        // matches.
        publish(env, alice, "<html>2</html>");
        env.close();
        auto const after = inm(etag);
        BEAST_EXPECT(after.result_int() == 200);
        BEAST_EXPECT(hdr(after, "ETag") != etag);

        // "*" on a missing document is a plain 404, not 304.
        Account const bob{"bob"};
        env.fund(XRP(1000), bob);
        env.close();
        BEAST_EXPECT(
            get(env,
                "/" + bob.human(),
                yield,
                {{"X-Forwarded-For", "203.0.113.7"}, {"If-None-Match", "*"}})
                .result_int() == 404);
    }

    void
    testOnlyValidated(boost::asio::yield_context& yield)
    {
        testcase("only validated state is served");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();

        publish(env, alice, doc());
        BEAST_EXPECT(env.le(keylet::appLoader(alice.id())));  // open view
        BEAST_EXPECT(get(env, "/" + alice.human(), yield).result_int() == 404);

        env.close();  // standalone validates on close
        BEAST_EXPECT(servedAsHtml(get(env, "/" + alice.human(), yield)));
    }

    void
    testRouting(boost::asio::yield_context& yield)
    {
        testcase("routing and methods");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        env.close();
        auto const a = alice.human();

        for (auto const& t :
             {"/" + a,
              "/" + a + "/",
              "/" + a + "?v=2",
              "/" + a + "/?v=2&x=%2F"})
            BEAST_EXPECTS(servedAsHtml(get(env, t, yield)), t);

        for (auto const& t :
             {std::string("/"),
              "/pwa/" + a,
              "//" + a,
              "/" + a + "//",
              "/" + a + "//?v=2",
              "/" + a + "/extra",
              "/" + a + "/index.html",
              "/" + a.substr(0, a.size() - 1),
              "/" + a + "x",
              "/%72" + a.substr(1),
              "/" + a + "%00",
              "/" + strHex(alice.id()),
              "/" + toBase58(AccountID{})})
        {
            auto const r = get(env, t, yield);
            BEAST_EXPECTS(r.result_int() == 404, t);
        }

        for (auto const& m : {"POST", "PUT", "DELETE", "OPTIONS"})
        {
            auto const raw = exchange(
                env,
                rawGet(
                    "/" + a,
                    {{"X-Forwarded-For", "203.0.113.7"}},
                    m,
                    std::string(m) == "POST" ? R"({"method":"stop"})" : ""),
                yield);
            auto const parsed = parse(raw);
            if (!BEAST_EXPECTS(parsed, m))
                continue;
            auto const& r = *parsed;
            BEAST_EXPECTS(r.result_int() == 405, m);
            BEAST_EXPECT(hdr(r, "Allow") == "GET, HEAD");
            BEAST_EXPECT(r.body().find("<html") == std::string::npos);
        }

        // HEAD: the GET's status and headers, Content-Length included, and
        // no body. Responses to HEAD cannot be parsed without knowing the
        // request, so look at the raw bytes.
        {
            bool closed = false;
            auto const raw = exchange(
                env,
                rawGet("/" + a, {{"X-Forwarded-For", "203.0.113.7"}}, "HEAD"),
                yield,
                "port_pwa",
                &closed);
            BEAST_EXPECT(closed);
            BEAST_EXPECT(raw.rfind("HTTP/1.1 200 ", 0) == 0);
            BEAST_EXPECT(
                raw.find(
                    "\r\nContent-Length: " + std::to_string(doc().size()) +
                    "\r\n") != std::string::npos);
            BEAST_EXPECT(
                raw.find("\r\nContent-Type: text/html; charset=utf-8\r\n") !=
                std::string::npos);
            BEAST_EXPECT(raw.find("\r\nETag: \"") != std::string::npos);
            auto const end = raw.find("\r\n\r\n");
            BEAST_EXPECT(end != std::string::npos && end + 4 == raw.size());

            Account const nobody{"nobody"};
            auto const miss = exchange(
                env,
                rawGet(
                    "/" + nobody.human(),
                    {{"X-Forwarded-For", "203.0.113.7"}},
                    "HEAD"),
                yield);
            BEAST_EXPECT(miss.rfind("HTTP/1.1 404 ", 0) == 0);
            BEAST_EXPECT(miss.find("\r\n\r\n") + 4 == miss.size());
        }

        // The port is not an RPC endpoint, a status page or a websocket.
        {
            auto const r = get(env, "/", yield);
            BEAST_EXPECT(r.result_int() == 404);
        }
        {
            std::array<std::uint8_t, 16> key{};
            std::string const req =
                "GET / HTTP/1.1\r\nHost: node\r\nUpgrade: websocket\r\n"
                "Connection: upgrade\r\nSec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: " +
                base64_encode(key.data(), key.size()) +
                "\r\nX-Forwarded-For: 203.0.113.7\r\n\r\n";
            auto const raw = exchange(env, req, yield);
            BEAST_EXPECT(raw.rfind("HTTP/1.1 403 ", 0) == 0);
            // Answered by the pwa code, not statusRequestResponse: no
            // version string, and the proxy rules apply to it.
            BEAST_EXPECT(
                raw.find("\r\nServer: " + systemName() + "\r\n") !=
                std::string::npos);
            BEAST_EXPECT(
                exchange(
                    env,
                    req.substr(0, req.find("X-Forwarded-For")) + "\r\n",
                    yield)
                    .rfind("HTTP/1.1 400 ", 0) == 0);
        }
    }

    void
    testProxyRules(boost::asio::yield_context& yield)
    {
        testcase("proxy and X-Forwarded-For rules");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        env.close();
        auto const t = "/" + alice.human();

        // No X-Forwarded-For at all, and Forwarded / X-User are not
        // substitutes.
        BEAST_EXPECT(get(env, t, yield, {}).result_int() == 400);
        BEAST_EXPECT(
            get(env, t, yield, {{"Forwarded", "for=203.0.113.7"}})
                .result_int() == 400);
        BEAST_EXPECT(
            get(env, t, yield, {{"X-User", "someone"}}).result_int() == 400);

        for (auto const& v :
             {"",
              " ",
              "unknown",
              "1.2.3",
              "1.2.3.4.5",
              "0.0.0.0",
              "::",
              "_hidden",
              "[::1",
              "[::1]x",
              "1.2.3.4, ",
              "host.example"})
        {
            BEAST_EXPECTS(
                get(env, t, yield, {{"X-Forwarded-For", v}}).result_int() ==
                    400,
                v);
        }

        for (auto const& v :
             {"203.0.113.7",
              " 203.0.113.7 ",
              "203.0.113.7:4711",
              "2001:db8::1",
              "[2001:db8::1]",
              "[2001:db8::1]:443",
              "garbage, 203.0.113.7",
              "unknown,198.51.100.9"})
        {
            BEAST_EXPECTS(
                servedAsHtml(get(env, t, yield, {{"X-Forwarded-For", v}})), v);
        }

        // A repeated header: the last one's last entry is authoritative.
        BEAST_EXPECT(servedAsHtml(
            get(env,
                t,
                yield,
                {{"X-Forwarded-For", "1.1.1.1"},
                 {"X-Forwarded-For", "garbage, 203.0.113.7"}})));
        BEAST_EXPECT(
            get(env,
                t,
                yield,
                {{"X-Forwarded-For", "203.0.113.7"},
                 {"X-Forwarded-For", "unknown"}})
                .result_int() == 400);

        // A connection that does not come from a secure_gateway address.
        {
            Env env2{*this, pwaConfig([](Section& s) {
                         s.set("secure_gateway", "10.255.255.254");
                     })};
            Account const carol{"carol"};
            env2.fund(XRP(1000), carol);
            publish(env2, carol, doc());
            env2.close();
            auto const r = get(env2, "/" + carol.human(), yield);
            BEAST_EXPECT(r.result_int() == 403);
            BEAST_EXPECT(r.body().find("<html") == std::string::npos);
        }
    }

    void
    testConnectionClose(boost::asio::yield_context& yield)
    {
        testcase("every response closes the connection");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        env.close();

        // A single request asking for keep-alive still gets
        // "Connection: close" and a FIN straight after the response.
        {
            bool closed = false;
            auto const raw = exchange(
                env,
                rawGet(
                    "/" + alice.human(),
                    {{"X-Forwarded-For", "203.0.113.7"},
                     {"Connection", "keep-alive"}}),
                yield,
                "port_pwa",
                &closed);
            BEAST_EXPECT(closed);
            auto const r = parse(raw);
            BEAST_EXPECT(r && hdr(*r, "Connection") == "close");
        }

        // Pipelined requests: only the first is ever answered. Also true
        // for errors.
        Headers const h{
            {"X-Forwarded-For", "203.0.113.7"}, {"Connection", "keep-alive"}};
        for (auto const& t : {"/" + alice.human(), std::string("/nope")})
        {
            bool closed = false;
            auto const raw = exchange(
                env, rawGet(t, h) + rawGet(t, h), yield, "port_pwa", &closed);
            BEAST_EXPECTS(closed, t);
            std::size_t responses = 0;
            for (auto p = raw.find("HTTP/1.1 "); p != std::string::npos;
                 p = raw.find("HTTP/1.1 ", p + 1))
                ++responses;
            // Never a second response. (If the close turns into an RST the
            // kernel may drop even the first, so only the upper bound is
            // deterministic.)
            BEAST_EXPECTS(responses <= 1, t);
        }

        // A refused request closes too.
        bool closed = false;
        exchange(
            env, rawGet("/" + alice.human(), {}), yield, "port_pwa", &closed);
        BEAST_EXPECT(closed);
    }

    void
    testMetering(boost::asio::yield_context& yield)
    {
        testcase("metered by the rightmost X-Forwarded-For entry");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        env.close();

        // Rotate the client-controlled leftmost entry on every request; the
        // proxy-written rightmost one stays fixed. Throttling must still
        // happen, and misses alone must get it there.
        chargeNearDrop(env, "203.0.113.7", 4);
        std::mt19937 rng{7};
        auto const spoof = std::to_string(rng() % 223 + 1) + ".0.0." +
            std::to_string(rng() % 254 + 1);
        BEAST_EXPECT(missesThrottle(env, spoof + ", 203.0.113.7", yield));

        // Throttled for hits too, now.
        BEAST_EXPECT(get(env, "/" + alice.human(), yield).result_int() == 503);

        // Another client behind the same proxy is unaffected.
        BEAST_EXPECT(servedAsHtml(
            get(env,
                "/" + alice.human(),
                yield,
                {{"X-Forwarded-For", "198.51.100.9"}})));

        // A miss costs exactly what a hit does.
        auto& rm = env.app().getResourceManager();
        auto balance = [&](std::string const& ip) {
            return rm.newInboundEndpoint(beast::IP::Endpoint::from_string(ip))
                .balance();
        };
        Account const nobody{"nobody"};
        auto const hit0 = balance("192.0.2.1");
        BEAST_EXPECT(servedAsHtml(
            get(env,
                "/" + alice.human(),
                yield,
                {{"X-Forwarded-For", "192.0.2.1"}})));
        auto const hit = balance("192.0.2.1") - hit0;
        auto const miss0 = balance("192.0.2.2");
        BEAST_EXPECT(
            get(env,
                "/" + nobody.human(),
                yield,
                {{"X-Forwarded-For", "192.0.2.2"}})
                .result_int() == 404);
        auto const miss = balance("192.0.2.2") - miss0;
        // Balances are the decayed charge over the 32s window, so allow one
        // unit of rounding; before, a miss cost 1/21 of a hit.
        BEAST_EXPECTS(
            miss >= lookupCost / Resource::decayWindowSeconds - 1 &&
                std::abs(hit - miss) <= 1,
            std::to_string(hit) + " " + std::to_string(miss));
    }

    void
    testIPv6Billing(boost::asio::yield_context& yield)
    {
        testcase("IPv6 clients are billed and counted per /64");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        env.close();
        auto const t = "/" + alice.human();

        for (auto const xff :
             {"2001:db8:1:2::1",
              "[2001:db8:1:2:ffff:ffff:ffff:ffff]:443",
              "2001:db8:1:3::1",
              "::ffff:203.0.113.7",
              "203.0.113.7"})
            BEAST_EXPECTS(
                servedAsHtml(get(env, t, yield, {{"X-Forwarded-For", xff}})),
                xff);

        auto const r = env.rpc("json", "pwa_info", "{}")[jss::result];
        BEAST_EXPECT(r["clients"]["tracked"] == 3);
        std::map<std::string, std::string> rows;
        auto const& top = r["clients"]["top"];
        for (Json::UInt i = 0; i < top.size(); ++i)
            rows[top[i]["ip"].asString()] = top[i]["requests"].asString();
        BEAST_EXPECT(rows["2001:db8:1:2::/64"] == "2");
        BEAST_EXPECT(rows["2001:db8:1:3::/64"] == "1");
        // IPv4-mapped IPv6 is the same client as plain IPv4.
        BEAST_EXPECT(rows["203.0.113.7"] == "2");

        // One budget per /64: addresses across the same /64 are throttled
        // together, and the neighbouring /64 is not.
        chargeNearDrop(env, "2001:db8:1:2::", 4);
        BEAST_EXPECT(missesThrottle(env, "2001:db8:1:2::abcd", yield));
        BEAST_EXPECT(
            get(env, t, yield, {{"X-Forwarded-For", "2001:db8:1:2:1:2:3:4"}})
                .result_int() == 503);
        BEAST_EXPECT(servedAsHtml(
            get(env, t, yield, {{"X-Forwarded-For", "2001:db8:1:3::abcd"}})));
    }

    void
    testPWAInfo(boost::asio::yield_context& yield)
    {
        testcase("pwa_info RPC");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        publish(env, alice, doc());
        env.close();

        auto info = [&](std::string const& params = "{}") {
            return env.rpc("json", "pwa_info", params)[jss::result];
        };

        {
            auto const r = info();
            BEAST_EXPECT(r[jss::status] == "success");
            BEAST_EXPECT(r["requests"] == "0");
            BEAST_EXPECT(r[jss::ports].size() == 1);
            BEAST_EXPECT(r[jss::ports][0u][jss::name] == "port_pwa");
        }

        auto const a = "/" + alice.human();
        Headers const c1{{"X-Forwarded-For", "203.0.113.7"}};
        Headers const c2{{"X-Forwarded-For", "x, 198.51.100.9"}};

        auto const etag = hdr(get(env, a, yield, c1), "ETag");  // served
        get(env, a, yield, c1);                                 // served
        get(env,
            a,
            yield,
            {{"X-Forwarded-For", "203.0.113.7"},
             {"If-None-Match", etag}});          // 304
        get(env, "/" + bob.human(), yield, c1);  // not_found
        get(env, "/" + bob.human(), yield, c2);  // not_found
        get(env, "/nope", yield, c2);            // bad_target
        get(env, a, yield, c2, "POST");          // bad_method
        get(env, a, yield, {});                  // no XFF

        auto const r = info(R"({"top": 5})");
        BEAST_EXPECT(r["requests"] == "8");
        auto const& o = r["outcomes"];
        BEAST_EXPECT(o["served"] == "2");
        BEAST_EXPECT(o["not_modified"] == "1");
        BEAST_EXPECT(o["not_found"] == "2");
        BEAST_EXPECT(o["bad_target"] == "1");
        BEAST_EXPECT(o["bad_method"] == "1");
        BEAST_EXPECT(o["no_forwarded_for"] == "1");
        BEAST_EXPECT(o["direct_connection"] == "0");
        BEAST_EXPECT(o["throttled"] == "0");
        BEAST_EXPECT(o["unavailable"] == "0");
        BEAST_EXPECT(o["upgrade_refused"] == "0");
        BEAST_EXPECT(o["busy"] == "0");
        BEAST_EXPECT(r["bytes_served"] == std::to_string(2 * doc().size()));
        BEAST_EXPECT(r["last_ledger_index"] == env.closed()->seq());

        // Only accounts with a loader are tracked; bob's 404 is not.
        BEAST_EXPECT(r["accounts"]["tracked"] == 1);
        auto const& acc = r["accounts"]["top"][0u];
        BEAST_EXPECT(acc["account"] == alice.human());
        BEAST_EXPECT(acc["served"] == "2");
        BEAST_EXPECT(acc["not_modified"] == "1");

        // Clients are keyed by the billed (rightmost) address.
        BEAST_EXPECT(r["clients"]["tracked"] == 2);
        auto const& top = r["clients"]["top"][0u];
        BEAST_EXPECT(top["ip"] == "203.0.113.7");
        BEAST_EXPECT(top["requests"] == "4");
        BEAST_EXPECT(top["served"] == "2");
        BEAST_EXPECT(top["rejected"] == "1");
        auto const& second = r["clients"]["top"][1u];
        BEAST_EXPECT(second["ip"] == "198.51.100.9");
        BEAST_EXPECT(second["rejected"] == "3");

        // top limits rows.
        BEAST_EXPECT(info(R"({"top": 1})")["clients"]["top"].size() == 1);
        BEAST_EXPECT(info(R"({"top": 0})")["clients"]["top"].size() == 0);

        // Parameter validation.
        for (auto const& bad :
             {R"({"top": 201})",
              R"({"top": -1})",
              R"({"top": "5"})",
              R"({"top": true})",
              R"({"top": 1.5})",
              R"({"reset": "yes"})"})
        {
            BEAST_EXPECTS(info(bad)[jss::error] == "invalidParams", bad);
        }

        // reset returns the snapshot, then zeroes.
        {
            auto const before = info(R"({"reset": true})");
            BEAST_EXPECT(before["requests"] == "8");
            BEAST_EXPECT(before[jss::reset] == true);
            auto const after = info();
            BEAST_EXPECT(after["requests"] == "0");
            BEAST_EXPECT(after["accounts"]["tracked"] == 0);
            BEAST_EXPECT(after["clients"]["tracked"] == 0);
            BEAST_EXPECT(after["bytes_served"] == "0");
            BEAST_EXPECT(after["last_ledger_index"] == 0);
        }

        // Command-line form: pwa_info [<top>] [reset], in either order.
        {
            get(env, a, yield, c1);
            get(env, a, yield, c2);
            auto const cli = env.rpc("pwa_info", "reset", "1")[jss::result];
            BEAST_EXPECT(cli[jss::status] == "success");
            BEAST_EXPECT(cli["requests"] == "2");
            BEAST_EXPECT(cli["clients"]["top"].size() == 1);
            BEAST_EXPECT(cli[jss::reset] == true);
            BEAST_EXPECT(info()["requests"] == "0");

            get(env, a, yield, c1);
            BEAST_EXPECT(
                env.rpc("pwa_info", "0")[jss::result]["requests"] == "1");
            BEAST_EXPECT(
                env.rpc("pwa_info")[jss::result]["clients"]["top"].size() == 1);
            // A parse error never reaches the server: it is the top level.
            BEAST_EXPECT(
                env.rpc("pwa_info", "x")[jss::error] == "invalidParams");
            BEAST_EXPECT(
                env.rpc("pwa_info", "-1")[jss::error] == "invalidParams");
            BEAST_EXPECT(
                env.rpc("pwa_info", "reset", "reset")[jss::error] ==
                "invalidParams");
            BEAST_EXPECT(
                env.rpc("pwa_info", "1", "2")[jss::error] == "invalidParams");
        }

        // Admin only.
        {
            Env env2{*this, jtx::no_admin(pwaConfig())};
            auto const jrc = makeJSONRPCClient(env2.app().config());
            auto const jr = jrc->invoke("pwa_info", Json::objectValue);
            // A non-admin HTTP call to an admin method is refused with a
            // plain-text 403, which the client reports as a null result.
            BEAST_EXPECT(jr[jss::result].isNull());
        }
    }

    // Browser-originated requests must not ride an IP-based admin grant on
    // any port. This, not anything on the pwa port, is what stops a hosted
    // document driving 127.0.0.1:5005 on the visitor's own machine.
    void
    testOriginAdmin(boost::asio::yield_context& yield)
    {
        testcase("Origin header never receives IP-granted admin");
        using namespace jtx;
        Env env{*this};  // envconfig: port_rpc and port_ws are admin by IP

        auto postTo = [&](Env& e, Headers const& h, std::string const& ct) {
            std::string const body = R"({"method":"ledger_accept"})";
            std::string r = "POST / HTTP/1.1\r\nHost: node\r\n";
            for (auto const& [k, v] : h)
                r += k + ": " + v + "\r\n";
            r += "Content-Type: " + ct +
                "\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\n\r\n" + body;
            return exchange(e, r, yield, "port_rpc");
        };
        auto post = [&](Headers const& h, std::string const& ct) {
            return postTo(env, h, ct);
        };

        BEAST_EXPECT(
            post({}, "application/json").rfind("HTTP/1.1 200 ", 0) == 0);
        for (auto const origin : {"null", "https://evil.example"})
        {
            BEAST_EXPECTS(
                post({{"Origin", origin}}, "application/json")
                        .rfind("HTTP/1.1 403 ", 0) == 0,
                origin);
            BEAST_EXPECTS(
                post({{"Origin", origin}}, "text/plain")
                        .rfind("HTTP/1.1 403 ", 0) == 0,
                origin);
        }

        std::array<std::uint8_t, 16> key{};
        std::string const upgrade =
            "GET / HTTP/1.1\r\nHost: node\r\nUpgrade: websocket\r\n"
            "Connection: upgrade\r\nSec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: " +
            base64_encode(key.data(), key.size()) + "\r\nOrigin: null\r\n\r\n";
        BEAST_EXPECT(
            exchange(env, upgrade, yield, "port_ws")
                .rfind("HTTP/1.1 403 ", 0) == 0);

        // admin_origin lets named browser origins through, compared without
        // regard to case; every other origin is still refused.
        {
            auto cfg = jtx::envconfig();
            (*cfg)["port_rpc"].set(
                "admin_origin", "http://localhost:8080, https://tool.example");
            Env env2{*this, std::move(cfg)};
            auto status = [&](std::string const& origin) {
                return postTo(env2, {{"Origin", origin}}, "application/json")
                    .substr(0, 13);
            };
            BEAST_EXPECT(status("http://localhost:8080") == "HTTP/1.1 200 ");
            BEAST_EXPECT(status("https://TOOL.example") == "HTTP/1.1 200 ");
            BEAST_EXPECT(status("http://localhost:8081") == "HTTP/1.1 403 ");
            BEAST_EXPECT(status("https://evil.example") == "HTTP/1.1 403 ");
            BEAST_EXPECT(status("null") == "HTTP/1.1 403 ");
        }

        // "*" restores the behaviour from before the Origin check.
        {
            auto cfg = jtx::envconfig();
            (*cfg)["port_rpc"].set("admin_origin", "*");
            Env env2{*this, std::move(cfg)};
            BEAST_EXPECT(
                postTo(env2, {{"Origin", "null"}}, "text/plain")
                    .rfind("HTTP/1.1 200 ", 0) == 0);
        }

        // A non-admin port is unaffected: browsers keep the public API.
        {
            Env env2{*this, jtx::no_admin(jtx::envconfig())};
            std::string const body = R"({"method":"server_info"})";
            std::string const r =
                "POST / HTTP/1.1\r\nHost: node\r\n"
                "Origin: https://wallet.example\r\n"
                "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n\r\n" + body;
            BEAST_EXPECT(
                exchange(env2, r, yield, "port_rpc")
                    .rfind("HTTP/1.1 200 ", 0) == 0);
        }
    }

public:
    void
    run() override
    {
        testConfigValidation();
        yield_to([&](boost::asio::yield_context& yield) {
            testServeAndHeaders(yield);
            testETag(yield);
            testOnlyValidated(yield);
            testRouting(yield);
            testProxyRules(yield);
            testConnectionClose(yield);
            testMetering(yield);
            testIPv6Billing(yield);
            testPWAInfo(yield);
            testOriginAdmin(yield);
        });
    }
};

BEAST_DEFINE_TESTSUITE(PWAServe, rpc, ripple);

}  // namespace test
}  // namespace ripple
