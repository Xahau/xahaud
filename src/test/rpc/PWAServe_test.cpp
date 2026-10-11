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

// End-to-end tests for "pwa" ports over real sockets: startup validation,
// proxy and X-Forwarded-For rules, routing, the bare-zone redirect, custom
// domains (against an in-process DNS server), lists, headers, ETag/304,
// pwa_info, and the browser-Origin admin hardening.

// End-to-end tests for "pwa" ports over real sockets: startup validation,
// proxy and X-Forwarded-For rules, routing, the bare-zone redirect, custom
// domains (against an in-process DNS server), lists, headers, ETag/304 and
// pwa_info.

#include <test/jtx.h>
#include <test/jtx/JSONRPCClient.h>
#include <test/jtx/envconfig.h>
#include <xrpld/core/Config.h>
#include <xrpld/rpc/ServerHandler.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/beast/test/yield_to.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>
#include <xrpl/resource/ResourceManager.h>
#include <xrpl/resource/detail/Tuning.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace ripple {
namespace test {

// A recursive resolver stand-in: answers TXT queries for
// "_xahau-pwa.<host>" from a table, on its own thread.
class FakeDNS
{
public:
    enum class Mode { answer, nxdomain, servfail };
    struct Entry
    {
        Mode mode = Mode::answer;
        std::vector<std::string> txt;  // one TXT RR each
    };

    FakeDNS()
        : socket_(
              io_,
              boost::asio::ip::udp::endpoint(
                  boost::asio::ip::make_address(getEnvLocalhostAddr()),
                  0))
    {
        receive();
        thread_ = std::thread([this] { io_.run(); });
    }

    ~FakeDNS()
    {
        boost::asio::post(io_, [this] {
            boost::system::error_code ec;
            socket_.close(ec);
        });
        thread_.join();
    }

    std::string
    address() const  // as a "resolver" value
    {
        auto const ep = socket_.local_endpoint();
        auto const a = ep.address().to_string();
        return (ep.address().is_v6() ? "[" + a + "]" : a) + ":" +
            std::to_string(ep.port());
    }

    void
    set(std::string const& host, Entry e)
    {
        std::lock_guard lock(mutex_);
        table_[host] = std::move(e);
    }

    int
    queries(std::string const& host)
    {
        std::lock_guard lock(mutex_);
        return seen_[host];
    }

private:
    void
    receive()
    {
        socket_.async_receive_from(
            boost::asio::buffer(buf_),
            from_,
            [this](boost::system::error_code ec, std::size_t n) {
                if (ec)
                    return;
                answer(n);
                receive();
            });
    }

    void
    answer(std::size_t n)
    {
        std::size_t pos = 12;
        std::vector<std::string> labels;
        while (pos < n && buf_[pos] != 0)
        {
            auto const len = buf_[pos];
            if (pos + 1 + len > n)
                return;
            labels.emplace_back(
                reinterpret_cast<char const*>(&buf_[pos + 1]), len);
            pos += 1 + len;
        }
        pos += 1 + 4;
        if (pos > n || labels.size() < 2 || labels.front() != "_xahau-pwa")
            return;
        std::string host;
        for (std::size_t i = 1; i < labels.size(); ++i)
            host += (i > 1 ? "." : "") + labels[i];

        Entry e{Mode::nxdomain, {}};
        {
            std::lock_guard lock(mutex_);
            ++seen_[host];
            if (auto it = table_.find(host); it != table_.end())
                e = it->second;
        }
        std::vector<std::uint8_t> r(buf_.begin(), buf_.begin() + pos);
        r[2] = 0x81;
        r[3] = 0x80 |
            (e.mode == Mode::nxdomain       ? 3
                 : e.mode == Mode::servfail ? 2
                                            : 0);
        auto const an = e.mode == Mode::answer ? e.txt.size() : 0;
        r[6] = 0;
        r[7] = static_cast<std::uint8_t>(an);
        r[8] = r[9] = r[10] = r[11] = 0;
        auto put16 = [&](std::uint16_t x) {
            r.push_back(static_cast<std::uint8_t>(x >> 8));
            r.push_back(static_cast<std::uint8_t>(x & 0xff));
        };
        for (std::size_t i = 0; i < an; ++i)
        {
            r.push_back(0xC0);  // pointer to the question name
            r.push_back(12);
            put16(16);
            put16(1);
            put16(0);
            put16(300);
            put16(static_cast<std::uint16_t>(e.txt[i].size() + 1));
            r.push_back(static_cast<std::uint8_t>(e.txt[i].size()));
            r.insert(r.end(), e.txt[i].begin(), e.txt[i].end());
        }
        boost::system::error_code ec;
        socket_.send_to(boost::asio::buffer(r), from_, 0, ec);
    }

    boost::asio::io_context io_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint from_;
    std::array<std::uint8_t, 2048> buf_{};
    std::thread thread_;
    std::mutex mutex_;
    std::map<std::string, Entry> table_;
    std::map<std::string, int> seen_;
};

class PWAServe_test : public beast::unit_test::suite,
                      public beast::test::enable_yield_to

                      class PWAServe_test : public beast::unit_test::suite,
                                            public beast::test::enable_yield_to
{
    using response_t =
        boost::beast::http::response<boost::beast::http::string_body>;
    using Headers = std::vector<std::pair<std::string, std::string>>;

    static constexpr char const* zone = "pwa.test";
    static constexpr char const* xff = "203.0.113.7";

    static std::string
    doc()
    {
        return "<!DOCTYPE html>\n<html><head><title>t</title></head><body>"
               "<script>1</script></body></html>\n";
    }

    static std::string
    lower(std::string const& s)
    {
        return boost::algorithm::to_lower_copy(s);
    }

    // The account's own host label: pwa + its decimal AccountIndex.
    static std::string
    label(jtx::Env& env, jtx::Account const& a)
    {
        return "pwa" + std::to_string((*env.le(a))[sfAccountIndex]);
    }

    // A config with a working pwa port behind a "proxy" at the test's own
    // address. `tweak` may edit the config, [port_pwa] included.
    static std::unique_ptr<Config>
    pwaConfig(std::function<void(Config&)> const& tweak = {})
    {
        auto cfg = jtx::envconfig();
        (*cfg)["server"].append("port_pwa");
        auto& s = (*cfg)["port_pwa"];
        s.set("ip", getEnvLocalhostAddr());
        s.set("port", "0");
        s.set("protocol", "pwa");
        s.set("secure_gateway", getEnvLocalhostAddr());
        s.set("domain", zone);
        if (tweak)
            tweak(*cfg);
        return cfg;
    }

    // Send raw bytes to a port and read until the server closes (or 5s).
    std::string
    exchange(
        jtx::Env& env,
        std::string const& raw,
        boost::asio::yield_context& yield,
        std::string const& section = "port_pwa")
    {
        using namespace boost::asio;
        auto const ip = *env.app().config()[section].get<std::string>("ip");
        auto const port =
            *env.app().config()[section].get<std::uint16_t>("port");
        boost::system::error_code ec;
        io_context& ioc = get_io_context();
        ip::tcp::socket sock{ioc};
        sock.async_connect(
            ip::tcp::endpoint{ip::make_address(ip), port}, yield[ec]);
        if (!BEAST_EXPECT(!ec))
            return {};
        async_write(sock, buffer(raw), yield[ec]);
        if (!BEAST_EXPECT(!ec))
            return {};
        steady_timer timer{ioc};
        timer.expires_after(std::chrono::seconds(5));
        timer.async_wait([&](boost::system::error_code const& e) {
            if (!e)
                sock.close();
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
        return out;
    }

    // A request for `target` at `host`, with X-Forwarded-For unless the
    // caller supplied headers of its own.
    static std::string
    raw(std::string const& host,
        std::string const& target,
        Headers const& headers = {{"X-Forwarded-For", xff}},
        std::string const& method = "GET",
        std::string const& body = "")
    {
        std::string r = method + " " + target + " HTTP/1.1\r\n";
        if (!host.empty())
            r += "Host: " + host + "\r\n";
        for (auto const& [k, v] : headers)
            r += k + ": " + v + "\r\n";
        if (!body.empty())
            r += "Content-Type: application/json\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\n";
        return r + "\r\n" + body;
    }

    static std::optional<response_t>
    parse(std::string const& s)
    {
        boost::beast::http::response_parser<boost::beast::http::string_body> p;
        p.eager(true);
        boost::system::error_code ec;
        p.put(boost::asio::buffer(s), ec);
        if (!ec && !p.is_done())
            p.put_eof(ec);
        if (ec || !p.is_done())
            return std::nullopt;
        return p.release();
    }

    response_t
    get(jtx::Env& env,
        std::string const& host,
        std::string const& target,
        boost::asio::yield_context& yield,
        Headers const& headers = {{"X-Forwarded-For", xff}},
        std::string const& method = "GET")
    {
        auto const s = exchange(env, raw(host, target, headers, method), yield);
        auto r = parse(s);
        BEAST_EXPECTS(r.has_value(), s);
        return r ? std::move(*r) : response_t{};
    }

    // The document at the account's own host.
    response_t
    getDoc(
        jtx::Env& env,
        jtx::Account const& a,
        boost::asio::yield_context& yield,
        Headers const& headers = {{"X-Forwarded-For", xff}})
    {
        return get(
            env, label(env, a) + "." + zone, "/" + a.human(), yield, headers);
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
    html(response_t const& r)
    {
        return r.result_int() == 200 &&
            hdr(r, "Content-Type") == "text/html; charset=utf-8";
    }

    static void
    publish(jtx::Env& env, jtx::Account const& a, std::string const& d)
    {
        using namespace jtx;
        auto jt = noop(a);
        jt[sfAppLoader.fieldName] = strHex(d);
        env(jt, fee(XRP(1)));
        env.close();
    }

    Json::Value
    info(jtx::Env& env, std::string const& params = "{}")
    {
        return env.rpc("json", "pwa_info", params)[jss::result];
    }

    //--------------------------------------------------------------------------

    void
    testConfigValidation()
    {
        testcase("configuration is validated at startup");
        auto accepts = [&](std::function<void(Config&)> const& tweak) {
            try
            {
                setup_ServerHandler(*pwaConfig(tweak), std::stringstream{});
                return true;
            }
            catch (std::exception const&)
            {
                return false;
            }
        };
        auto set = [](char const* k, std::string v) {
            return [=](Config& c) { c["port_pwa"].set(k, v); };
        };
        jtx::Account const a{"a"};

        BEAST_EXPECT(accepts({}));
        for (auto const& [k, v] :
             std::vector<std::pair<char const*, std::string>>{
                 {"protocol", "pwa,http"},
                 {"protocol", "pwa,ws"},
                 {"secure_gateway", ""},
                 {"secure_gateway", "0.0.0.0"},
                 {"secure_gateway", "::/0"},
                 {"admin", getEnvLocalhostAddr()},
                 {"admin_user", "u"},
                 {"user", "u"},
                 {"ssl_key", "/nonexistent"},
                 {"domain", ""},
                 {"domain", "pwa_test"},
                 {"domain", ".pwa.test"},
                 {"domain", "pwa..test"},
                 {"domain", "-pwa.test"},
                 {"domain", "pwa test"},
                 {"domain", std::string(64, 'a') + ".test"}})
            BEAST_EXPECTS(!accepts(set(k, v)), std::string(k) + "=" + v);
        BEAST_EXPECT(accepts(set("secure_gateway", "10.0.0.0/8")));
        BEAST_EXPECT(accepts(set("domain", "PWA.Test., apps.example.net")));

        for (auto const& [k, v] :
             std::vector<std::pair<char const*, std::string>>{
                 {"custom_domains", "2"},
                 {"resolver", "localhost"},
                 {"resolver", "0.0.0.0"},
                 {"resolver", "::1"}})
            BEAST_EXPECTS(!accepts(set(k, v)), std::string(k) + "=" + v);
        BEAST_EXPECT(accepts(set("custom_domains", "1")));
        BEAST_EXPECT(accepts(set("resolver", "127.0.0.1:5353")));

        // pwa settings belong to pwa ports only.
        for (auto const& [k, v] :
             std::vector<std::pair<char const*, std::string>>{
                 {"domain", zone},
                 {"custom_domains", "1"},
                 {"resolver", "127.0.0.1"}})
            BEAST_EXPECTS(
                !accepts([=](Config& c) { c["port_rpc"].set(k, v); }), k);

        // Lists: one r-address per line, and never on both.
        for (auto const& line :
             {std::string("not-an-address"), toBase58(AccountID{})})
            BEAST_EXPECTS(
                !accepts([=](Config& c) { c["pwa_allowlist"].append(line); }),
                line);
        BEAST_EXPECT(!accepts(
            [&](Config& c) { c["pwa_blocklist"].append(strHex(a.id())); }));
        BEAST_EXPECT(!accepts([&](Config& c) {
            c["pwa_allowlist"].append(a.human());
            c["pwa_blocklist"].append(a.human());
        }));
    }

    void
    testServe(boost::asio::yield_context& yield)
    {
        testcase("serve, headers, ETag, HEAD, validated state only");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"}, bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        publish(env, alice, doc());

        auto const r = getDoc(env, alice, yield);
        BEAST_EXPECT(html(r));
        BEAST_EXPECT(r.body() == doc());
        BEAST_EXPECT(hdr(r, "Connection") == "close");
        BEAST_EXPECT(hdr(r, "Server") == systemName());
        BEAST_EXPECT(
            hdr(r, "Content-Security-Policy").rfind("sandbox ", 0) == 0);
        BEAST_EXPECT(hdr(r, "X-Frame-Options") == "DENY");
        BEAST_EXPECT(hdr(r, "X-Content-Type-Options") == "nosniff");
        BEAST_EXPECT(hdr(r, "Cache-Control") == "no-cache");
        // What a client needs to vet the document against a second node.
        BEAST_EXPECT(
            hdr(r, "X-NetworkID") ==
            std::to_string(env.app().config().NETWORK_ID));
        BEAST_EXPECT(
            hdr(r, "X-LedgerSeq") == std::to_string(env.closed()->seq()));
        BEAST_EXPECT(
            hdr(r, "X-LedgerHash") == to_string(env.closed()->info().hash));
        BEAST_EXPECT(hdr(r, "X-PWASeq") == "1");
        auto const etag = hdr(r, "ETag");
        BEAST_EXPECT(
            etag ==
            "\"" +
                to_string(
                    (*env.le(keylet::appLoader(alice.id())))[sfPreviousTxnID]) +
                "\"");

        // If-None-Match: the tag, or *; a stale tag is served.
        for (auto const& v : {etag, std::string("*")})
        {
            auto const m = getDoc(
                env,
                alice,
                yield,
                {{"X-Forwarded-For", xff}, {"If-None-Match", v}});
            BEAST_EXPECTS(m.result_int() == 304 && m.body().empty(), v);
            BEAST_EXPECT(hdr(m, "ETag") == etag && !m.has_content_length());
        }
        BEAST_EXPECT(html(getDoc(
            env,
            alice,
            yield,
            {{"X-Forwarded-For", xff}, {"If-None-Match", "\"old\""}})));

        // A new version changes the ETag and the sequence.
        publish(env, alice, "<html><body>2</body></html>");
        auto const r2 = getDoc(env, alice, yield);
        BEAST_EXPECT(html(r2) && hdr(r2, "ETag") != etag);
        BEAST_EXPECT(hdr(r2, "X-PWASeq") == "2");

        // HEAD: the GET's headers, Content-Length included, no body.
        {
            auto const s = exchange(
                env,
                raw(label(env, alice) + "." + zone,
                    "/" + alice.human(),
                    {{"X-Forwarded-For", xff}},
                    "HEAD"),
                yield);
            BEAST_EXPECT(s.rfind("HTTP/1.1 200 ", 0) == 0);
            BEAST_EXPECT(
                s.find(
                    "\r\nContent-Length: " +
                    std::to_string(
                        std::string("<html><body>2</body></html>").size()) +
                    "\r\n") != std::string::npos);
            BEAST_EXPECT(s.find("\r\n\r\n") + 4 == s.size());
        }

        // Only validated state: bob's loader is not served until the ledger
        // holding it closes.
        {
            auto jt = noop(bob);
            jt[sfAppLoader.fieldName] = strHex(doc());
            env(jt, fee(XRP(1)));
            BEAST_EXPECT(getDoc(env, bob, yield).result_int() == 404);
            env.close();
            BEAST_EXPECT(html(getDoc(env, bob, yield)));
        }
    }

    void
    testRouting(boost::asio::yield_context& yield)
    {
        testcase("routing, redirect and methods");
        using namespace jtx;
        Env env{*this, pwaConfig([](Config& c) {
                    c["port_pwa"].set("domain", "pwa.test, Apps.Example.");
                })};
        Account const alice{"alice"}, bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        publish(env, alice, doc());
        auto const a = alice.human();
        auto const la = label(env, alice);
        auto const ua = boost::algorithm::to_upper_copy(la);
        auto const wrong =
            "pwa" + std::to_string((*env.le(alice))[sfAccountIndex] + 1);

        // Served: pwa<index> label (any case) with the exact address in the
        // path; either zone; port, trailing dot, one trailing slash and a
        // query string ignored.
        for (auto const& [host, t] :
             std::vector<std::pair<std::string, std::string>>{
                 {la + ".pwa.test", "/" + a},
                 {ua + ".PWA.TEST:443", "/" + a + "?v=1"},
                 {la + ".pwa.test.", "/" + a + "/"},
                 {la + ".apps.example:8443", "/" + a + "/?v=2&x=%2F"}})
            BEAST_EXPECTS(html(get(env, host, t, yield)), host + t);

        // Not a document: 404.
        for (auto const& [host, t] :
             std::vector<std::pair<std::string, std::string>>{
                 {"pwa.test", "/"},
                 {"pwa.test", "/" + lower(a)},  // path must be exact
                 {"pwa.test", "/" + a + "x"},
                 {"pwa.test", "/" + a + "/extra"},
                 {"pwa.test", "//" + a},
                 {"pwa.test", "/" + strHex(alice.id())},
                 {"pwa.test", "/" + toBase58(AccountID{})},
                 {"www.pwa.test", "/" + a},
                 {la + ".pwa.test", "/"},
                 {la + ".pwa.test", "/" + lower(a)},
                 {la + ".pwa.test", "/" + bob.human()},  // disagree
                 {la + ".pwa.test", "/" + a + "/index.html"},
                 {wrong + ".pwa.test", "/" + a},         // not alice's index
                 {la.substr(3) + ".pwa.test", "/" + a},  // no pwa prefix
                 {"pwa0" + la.substr(3) + ".pwa.test", "/" + a},  // leading 0
                 {"pwax.pwa.test", "/" + a},
                 {"pwa.pwa.test", "/" + a},
                 {la + ".other.test", "/" + a},  // not our zone
                 {la + ".pwa.test.evil", "/" + a},
                 {"x." + la + ".pwa.test", "/" + a}})
            BEAST_EXPECTS(
                get(env, host, t, yield).result_int() == 404, host + t);

        // No usable Host: 400. A port is echoed into redirects, so digits.
        for (auto const& host :
             {"",
              "[::1]:8088",
              "pwa_test",
              "a b.pwa.test",
              "pwa.test:",
              "pwa.test:x",
              "pwa.test:1234567"})
            BEAST_EXPECTS(
                get(env, host, "/" + a, yield).result_int() == 400, host);
        BEAST_EXPECT(
            exchange(
                env,
                raw("",
                    "/" + a,
                    {{"X-Forwarded-For", xff},
                     {"Host", "a.pwa.test"},
                     {"Host", "a.pwa.test"}}),
                yield)
                .rfind("HTTP/1.1 400 ", 0) == 0);

        // <zone>/<r-address>: 301 to the account's own host. Exact path,
        // port and query kept, scheme-relative, no loader needed (bob has
        // none). Following it works.
        auto const lb = label(env, bob);
        for (auto const& [host, t, location] :
             std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"pwa.test", "/" + a, "//" + la + ".pwa.test/" + a},
                 {"PWA.TEST.", "/" + a + "/", "//" + la + ".pwa.test/" + a},
                 {"pwa.test:8443",
                  "/" + a + "?v=2&x=%2F",
                  "//" + la + ".pwa.test:8443/" + a + "?v=2&x=%2F"},
                 {"apps.example",
                  "/" + a + "/?q#f",
                  "//" + la + ".apps.example/" + a + "?q"},
                 {"pwa.test",
                  "/" + bob.human(),
                  "//" + lb + ".pwa.test/" + bob.human()}})
        {
            auto const r = get(env, host, t, yield);
            BEAST_EXPECTS(r.result_int() == 301, host + t);
            BEAST_EXPECTS(hdr(r, "Location") == location, hdr(r, "Location"));
            BEAST_EXPECT(hdr(r, "Cache-Control") == "public, max-age=86400");
            BEAST_EXPECT(
                hdr(r, "ETag").empty() && hdr(r, "Connection") == "close");
            auto const rest = location.substr(2);
            auto const slash = rest.find('/');
            auto const followed =
                get(env, rest.substr(0, slash), rest.substr(slash), yield);
            BEAST_EXPECTS(
                followed.result_int() ==
                    (t.find(bob.human()) == std::string::npos ? 200 : 404),
                location);
        }
        {
            auto const s = exchange(
                env,
                raw("pwa.test", "/" + a, {{"X-Forwarded-For", xff}}, "HEAD"),
                yield);
            BEAST_EXPECT(s.rfind("HTTP/1.1 301 ", 0) == 0);
            BEAST_EXPECT(s.find("\r\n\r\n") + 4 == s.size());
        }

        // Methods, and no websockets.
        for (auto const m : {"POST", "PUT", "DELETE", "OPTIONS"})
        {
            auto const r = parse(exchange(
                env,
                raw(la + ".pwa.test",
                    "/" + a,
                    {{"X-Forwarded-For", xff}},
                    m,
                    std::string(m) == "POST" ? R"({"method":"stop"})" : ""),
                yield));
            if (BEAST_EXPECTS(r, m))
                BEAST_EXPECTS(
                    r->result_int() == 405 && hdr(*r, "Allow") == "GET, HEAD",
                    m);
        }
        {
            std::array<std::uint8_t, 16> key{};
            auto const s = exchange(
                env,
                raw("node",
                    "/",
                    {{"Upgrade", "websocket"},
                     {"Connection", "upgrade"},
                     {"Sec-WebSocket-Version", "13"},
                     {"Sec-WebSocket-Key",
                      base64_encode(key.data(), key.size())},
                     {"X-Forwarded-For", xff}}),
                yield);
            BEAST_EXPECT(s.rfind("HTTP/1.1 403 ", 0) == 0);
            BEAST_EXPECT(
                s.find("\r\nServer: " + systemName() + "\r\n") !=
                std::string::npos);
        }
    }

    void
    testLists(boost::asio::yield_context& yield)
    {
        testcase("allowlist and blocklist");
        using namespace jtx;
        Account const alice{"alice"}, bob{"bob"}, carol{"carol"}, dave{"dave"};
        auto status = [&](std::unique_ptr<Config> cfg,
                          std::vector<int> expect) {
            Env env{*this, std::move(cfg)};
            env.fund(XRP(1000), alice, bob, carol, dave);
            for (auto const& a : {alice, bob, carol})
                publish(env, a, doc());
            int i = 0;
            for (auto const& a : {alice, bob, carol, dave})
                BEAST_EXPECTS(
                    getDoc(env, a, yield).result_int() == expect[i++],
                    a.name());
        };
        // Open: everyone with a loader.
        status(pwaConfig(), {200, 200, 200, 404});
        // allowlist: only those (dave is listed but has no loader).
        status(
            pwaConfig([&](Config& c) {
                c["pwa_allowlist"].append(alice.human());
                c["pwa_allowlist"].append(dave.human());
            }),
            {200, 403, 403, 404});
        // blocklist, and blocklist wins over allowlist.
        status(
            pwaConfig(
                [&](Config& c) { c["pwa_blocklist"].append(bob.human()); }),
            {200, 403, 200, 404});
        status(
            pwaConfig([&](Config& c) {
                c["pwa_allowlist"].append(alice.human());
                c["pwa_allowlist"].append(bob.human());
                c["pwa_blocklist"].append(carol.human());
            }),
            {200, 200, 403, 404});
    }

    void
    testProxy(boost::asio::yield_context& yield)
    {
        testcase("proxy, X-Forwarded-For and metering");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        publish(env, alice, doc());
        auto with = [&](Headers const& h) {
            return getDoc(env, alice, yield, h).result_int();
        };

        // Missing or unusable: 400. Forwarded and X-User are not substitutes.
        BEAST_EXPECT(with({}) == 400);
        BEAST_EXPECT(with({{"Forwarded", "for=203.0.113.7"}}) == 400);
        for (auto const v :
             {"",
              "unknown",
              "1.2.3",
              "0.0.0.0",
              "::",
              "203.0.113.7:4711",
              "1.2.3.4, ",
              "host.example"})
            BEAST_EXPECTS(with({{"X-Forwarded-For", v}}) == 400, v);
        // The rightmost entry (of the last header) is the client.
        for (auto const v :
             {"203.0.113.7",
              " 203.0.113.7 ",
              "2001:db8::1",
              "garbage, 203.0.113.7",
              "::ffff:203.0.113.7"})
            BEAST_EXPECTS(with({{"X-Forwarded-For", v}}) == 200, v);
        BEAST_EXPECT(
            with(
                {{"X-Forwarded-For", "1.1.1.1"},
                 {"X-Forwarded-For", "x, 203.0.113.7"}}) == 200);
        BEAST_EXPECT(
            with(
                {{"X-Forwarded-For", "203.0.113.7"},
                 {"X-Forwarded-For", "unknown"}}) == 400);

        // Not from a secure_gateway address: 403.
        {
            Env env2{*this, pwaConfig([](Config& c) {
                         c["port_pwa"].set("secure_gateway", "10.255.255.254");
                     })};
            env2.fund(XRP(1000), alice);
            publish(env2, alice, doc());
            BEAST_EXPECT(getDoc(env2, alice, yield).result_int() == 403);
        }

        // Metering: the forwarded client is billed, not the proxy, an IPv6
        // client per /64, and a miss costs what a hit does. Push a client
        // close to the drop threshold, then a few misses tip it over.
        Account const nobody{"nobody"};
        auto nearDrop = [&](std::string const& ip, int misses) {
            auto c = env.app().getResourceManager().newInboundEndpoint(
                beast::IP::Endpoint::from_string(ip));
            c.charge(Resource::Charge(
                Resource::dropThreshold * Resource::decayWindowSeconds -
                    misses *
                        (20 + 400),  // feeReferenceRPC + feeMediumBurdenRPC
                "test"));
        };
        auto throttles = [&](std::string const& client) {
            for (int i = 0; i < 50; ++i)
            {
                auto const s =
                    getDoc(env, nobody, yield, {{"X-Forwarded-For", client}})
                        .result_int();
                if (s == 503)
                    return i > 0;
                if (s != 404)
                    return false;
            }
            return false;
        };
        nearDrop("198.51.100.9", 3);
        BEAST_EXPECT(throttles("198.51.100.9"));
        BEAST_EXPECT(
            with({{"X-Forwarded-For", "203.0.113.7"}}) ==
            200);  // others unaffected
        nearDrop("2001:db8:1:2::", 3);
        BEAST_EXPECT(throttles("2001:db8:1:2:dead:beef::1"));  // same /64
        BEAST_EXPECT(with({{"X-Forwarded-For", "2001:db8:1:3::1"}}) == 200);
    }

    void
    testCustomDomains(boost::asio::yield_context& yield)
    {
        testcase("custom domains: _xahau-pwa TXT record and Domain consent");
        using namespace jtx;
        Account const alice{"alice"}, bob{"bob"}, carol{"carol"};
        FakeDNS dns;
        Env env{*this, pwaConfig([&](Config& c) {
                    c["port_pwa"].set("custom_domains", "1");
                    c["port_pwa"].set("resolver", dns.address());
                    c["pwa_blocklist"].append(carol.human());
                })};
        env.fund(XRP(1000), alice, bob, carol);
        auto consent = [&](Account const& a, std::string const& d) {
            auto jt = noop(a);
            jt[sfDomain.fieldName] = strHex(d);
            env(jt);
            env.close();
        };
        publish(env, alice, doc());
        consent(alice, "App.Example.org");
        publish(env, bob, doc());  // no Domain
        publish(env, carol, doc());
        consent(carol, "blocked.example.org");

        using M = FakeDNS::Mode;
        dns.set("app.example.org", {M::answer, {alice.human()}});
        dns.set("sub.app.example.org", {M::answer, {alice.human()}});
        dns.set(
            "other.example.org",
            {M::answer, {"v=spf1 -all", " " + alice.human() + " "}});
        dns.set("nodomain.example.org", {M::answer, {bob.human()}});
        dns.set("blocked.example.org", {M::answer, {carol.human()}});
        dns.set("two.example.org", {M::answer, {alice.human(), bob.human()}});
        dns.set("empty.example.org", {M::answer, {}});
        dns.set("broken.example.org", {M::servfail, {}});

        // A miss is answered 429 at once, with a refresh; poll until the
        // lookup has landed in the cache.
        auto until = [&](std::string const& host, std::string const& t = "/") {
            response_t r;
            for (int i = 0; i < 50; ++i)
            {
                r = get(env, host, t, yield);
                if (r.result_int() != 429)
                    break;
                BEAST_EXPECT(hdr(r, "Retry-After") == "2");
                BEAST_EXPECT(
                    r.body().find("http-equiv=\"refresh\"") !=
                    std::string::npos);
                boost::asio::steady_timer wait{get_io_context()};
                wait.expires_after(std::chrono::milliseconds(100));
                wait.async_wait(yield);
            }
            return r;
        };

        // Served as the named account, same document and ETag as its own
        // host; the answer is cached, so the next request is immediate.
        auto const own = getDoc(env, alice, yield);
        auto const r = until("app.example.org");
        BEAST_EXPECT(
            html(r) && r.body() == doc() && hdr(r, "ETag") == hdr(own, "ETag"));
        BEAST_EXPECT(
            get(env, "app.example.org", "/", yield).result_int() == 200);
        BEAST_EXPECT(dns.queries("app.example.org") >= 1);
        BEAST_EXPECT(
            until("APP.Example.Org.:443", "/?utm=x").result_int() == 200);
        // A parent in Domain consents for the host.
        BEAST_EXPECT(html(until("sub.app.example.org")));

        // Only the root is a document; nothing under our zone reaches DNS.
        for (auto const& t : {"/" + alice.human(), std::string("/index.html")})
            BEAST_EXPECTS(until("app.example.org", t).result_int() == 404, t);
        for (auto const host : {"www.pwa.test", "x.y.pwa.test"})
            BEAST_EXPECTS(until(host).result_int() == 404, host);
        BEAST_EXPECT(dns.queries("www.pwa.test") == 0);

        // No usable record: 404. Resolver failure: 503. Named account must
        // consent with its Domain: 503. Lists apply: 403.
        for (auto const host :
             {"nothing.example.org", "empty.example.org", "two.example.org"})
            BEAST_EXPECTS(until(host).result_int() == 404, host);
        BEAST_EXPECT(until("broken.example.org").result_int() == 503);
        BEAST_EXPECT(until("other.example.org").result_int() == 503);
        BEAST_EXPECT(until("nodomain.example.org").result_int() == 503);
        BEAST_EXPECT(until("blocked.example.org").result_int() == 403);

        // Negative answers are cached; flush_dns picks up a changed record.
        auto const seen = dns.queries("nothing.example.org");
        BEAST_EXPECT(until("nothing.example.org").result_int() == 404);
        BEAST_EXPECT(dns.queries("nothing.example.org") == seen);
        dns.set("app.example.org", {M::answer, {bob.human()}});
        BEAST_EXPECT(until("app.example.org").result_int() == 200);
        BEAST_EXPECT(
            info(env, R"({"flush_dns": true})")["dns_flushed"].asUInt() >= 5);
        BEAST_EXPECT(until("app.example.org").result_int() == 503);

        auto const o = info(env)["outcomes"];
        BEAST_EXPECT(o["unknown_domain"] == "4");
        BEAST_EXPECT(o["dns_failed"] == "1");
        BEAST_EXPECT(o["domain_mismatch"] == "3");
        BEAST_EXPECT(o["blocked"] == "1");
        BEAST_EXPECT(o["dns_pending"].asString() != "0");
        BEAST_EXPECT(info(env)[jss::ports][0u]["custom_domains"] == true);
    }

    void
    testPWAInfo(boost::asio::yield_context& yield)
    {
        testcase("pwa_info");
        using namespace jtx;
        Env env{*this, pwaConfig()};
        Account const alice{"alice"}, bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        publish(env, alice, doc());

        auto r = info(env);
        BEAST_EXPECT(r[jss::status] == "success" && r["requests"] == "0");
        BEAST_EXPECT(
            r[jss::ports].size() == 1 &&
            r[jss::ports][0u][jss::name] == "port_pwa");
        BEAST_EXPECT(r[jss::ports][0u]["domains"][0u] == zone);
        BEAST_EXPECT(r[jss::ports][0u]["custom_domains"] == false);

        auto const etag = hdr(getDoc(env, alice, yield), "ETag");
        getDoc(
            env,
            alice,
            yield,
            {{"X-Forwarded-For", xff}, {"If-None-Match", etag}});
        getDoc(env, bob, yield);
        get(env, zone, "/", yield);
        get(env, zone, "/" + alice.human(), yield);
        getDoc(env, alice, yield, {});
        r = info(env);
        BEAST_EXPECT(r["requests"] == "6");
        auto const& o = r["outcomes"];
        BEAST_EXPECT(
            o["served"] == "1" && o["not_modified"] == "1" &&
            o["not_found"] == "1");
        BEAST_EXPECT(
            o["bad_target"] == "1" && o["redirected"] == "1" &&
            o["no_forwarded_for"] == "1");
        BEAST_EXPECT(r["bytes_served"] == std::to_string(doc().size()));

        for (auto const bad : {R"({"reset": "yes"})", R"({"flush_dns": 1})"})
            BEAST_EXPECTS(info(env, bad)[jss::error] == "invalidParams", bad);
        BEAST_EXPECT(info(env, R"({"reset": true})")["requests"] == "6");
        BEAST_EXPECT(info(env)["requests"] == "0");

        // Command line, and admin only.
        getDoc(env, alice, yield);
        BEAST_EXPECT(
            env.rpc("pwa_info", "reset")[jss::result]["requests"] == "1");
        BEAST_EXPECT(env.rpc("pwa_info", "x")[jss::error] == "invalidParams");
        {
            Env env2{*this, jtx::no_admin(pwaConfig())};
            auto const jrc = makeJSONRPCClient(env2.app().config());
            BEAST_EXPECT(jrc->invoke("pwa_info", Json::objectValue)[jss::result]
                             .isNull());
        }
    }

public:
    void
    run() override
    {
        testConfigValidation();
        yield_to([&](boost::asio::yield_context& yield) {
            testServe(yield);
            testRouting(yield);
            testLists(yield);
            testProxy(yield);
            testCustomDomains(yield);
            testPWAInfo(yield);
        });
    }
};

BEAST_DEFINE_TESTSUITE(PWAServe, rpc, ripple);

}  // namespace test
}  // namespace ripple
