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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/Ingest.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/overlay/detail/Handshake.h>
#include <xrpld/shamap/SHAMapInnerNode.h>
#include <xrpld/shamap/SHAMapLeafNode.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/protocol/messages.h>
#include <xrpl/protocol/tokens.h>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <algorithm>
#include <deque>
#include <map>
#include <regex>
#include <set>
#include <thread>

/*  [export_relay], and [export_relay_<name>] per further network. Each line
    "host [port [node key]]" is a peer of that network; the node public key
    pins it (peer TLS is self-signed, so without it anyone on the path could
    stand in). Options: network_id= (default 0, the XRPL), vl_url=,
    ingest_every= (Xahau ledgers, default 64, 0 for never), max_ledgers= and
    max_ledger_bytes= (default: no limit). A relay carries only exports for
    its network and imports only what that network's lists prove.

    Inbound. The UNL is the newest validator list signed by a key trusted for
    this network ([import_vl_keys] or UNLReport ImportVLKeys), from the peer
    or vl_url. When a ledger has validations from 80% of it, its header and
    whole transaction tree are fetched (which transactions used a shadow
    ticket cannot be known in advance) and every transaction returning an
    export, or recording an exporter's activity, is imported. And once the
    ledger proven for the network (ltEXPORT_FEE) has stood for ingest_every
    Xahau ledgers, the next complete ledger is also sent as a ttINGEST (see
    Ingest.h): it needs no account and costs nothing, so every relay does,
    and since they go by the record, not by what each last sent, any number
    of relays add one entry per ingest_every ledgers between them.

    Outbound. On each validated ledger, exports at quorum are sent to the
    peer, and resent while the ledger still waits for them (the shadow
    ticket stands; or, for the sequenced backstop TicketCreate, which has
    none, sfExportTicketPending still holds its Sequence).

    Trust and bounds. The peer is trusted for nothing: validations are
    checked against the UNL, headers against their hash, the tree against
    the header's root. Ledgers are kept for 32 sequences below the highest
    validated, tree nodes are taken only in answer to a request and dropped
    once a ledger is done with, and no frame may exceed 64 MiB. Beyond that
    nothing is capped unless max_ledgers= (the oldest goes first) or
    max_ledger_bytes= (a ledger over it is skipped) says so. The fetching is
    a steady draw on the peer: point a relay at a hub you run, not at a
    public server.
*/

namespace ripple {
namespace {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using namespace std::chrono_literals;

// Every relayer signs the Import bringing an export back with this published
// key, so all build the same bytes and the network keeps one. Import never
// trusts the outer signature of a ticketed XPOP.
std::pair<PublicKey, SecretKey> const&
relayKey()
{
    static auto const k =
        generateKeyPair(KeyType::ed25519, generateSeed("xahau export relay"));
    return k;
}

struct Config
{
    Application& app;
    std::string vlURL;
    std::optional<std::uint32_t> nid{};  // the other network's, none for 0

    std::uint32_t
    network() const
    {
        return nid.value_or(0);
    }

    std::uint32_t ingestEvery = 64;    // 0: never
    std::size_t maxLedgers = 0;        // held at once; 0: no limit
    std::uint64_t maxLedgerBytes = 0;  // of nodes per ledger; 0: no limit
    std::atomic<bool> stop{false};
};

// A TLS connection whose operations are bounded by a deadline and stop
struct Conn
{
    asio::io_context ioc;
    std::shared_ptr<asio::ssl::context> ctx = make_SSLContext("");
    stream_type s{ioc, *ctx};
    asio::ip::tcp::endpoint ep;
    std::atomic<bool> const& stop;

    Conn(
        std::string const& host,
        std::string const& port,
        std::atomic<bool> const& stop)
        : stop(stop)
    {
        // resolved through io() too, so a stalled resolver cannot hold up
        // shutdown; basic_resolver_results inherits its iterator privately
        // (Boost 1.87+), so take the first entry explicitly
        asio::ip::tcp::resolver resolver(ioc);
        asio::ip::tcp::resolver::results_type r;
        io([&](auto h) {
            resolver.async_resolve(
                host,
                port,
                [&r, h](boost::system::error_code ec, auto results) {
                    r = std::move(results);
                    h(ec);
                });
        });
        if (r.empty())
            Throw<std::runtime_error>("no address for " + host);
        ep = r.begin()->endpoint();
        io([&](auto h) {
            boost::beast::get_lowest_layer(s).async_connect(ep, h);
        });
        // peers self-sign and validator lists sign themselves
        s.set_verify_mode(asio::ssl::verify_none);
        SSL_set_tlsext_host_name(s.native_handle(), host.c_str());
        io([&](auto h) {
            s.async_handshake(asio::ssl::stream_base::client, h);
        });
    }

    std::size_t
    io(auto&& start, int secs = 30)
    {
        boost::system::error_code ec;
        std::size_t n = 0;
        bool done = false;
        start([&](boost::system::error_code e, std::size_t k = 0) {
            ec = e, n = k, done = true;
        });
        ioc.restart();
        for (auto const end = std::chrono::steady_clock::now() + 1s * secs;
             !done && !stop && std::chrono::steady_clock::now() < end;)
            ioc.run_for(200ms);
        if (!done)
            Throw<std::runtime_error>("timed out");
        if (ec)
            Throw<boost::system::system_error>(ec);
        return n;
    }
};

class Peer
{
    struct Ledger
    {
        LedgerIndex seq = 0;
        Json::Value vals{Json::objectValue};  // nodepub -> validation hex
        int state = 0;  // 0 validating, 1 fetching, 2 done
        int pending = 0, rounds = 0;
        std::optional<LedgerInfo> info;
        std::map<uint256, std::shared_ptr<SHAMapTreeNode>> nodes;
        std::uint64_t bytes = 0;  // of nodes, as received
    };

    Config& cfg_;
    Application& app_;
    beast::Journal const j_;
    std::string const host_, port_;
    std::optional<PublicKey> const pin_;  // the peer's node key, if given
    std::function<void(::google::protobuf::Message const&, int)> send_;

    Json::Value unl_;           // the XPOPs' validation.unl
    std::uint32_t vlSeq_ = 0;   // its sequence
    std::set<PublicKey> keys_;  // its validators' signing keys
    std::size_t quorum_ = 0;
    std::map<uint256, Ledger> ledgers_;
    LedgerIndex highest_ = 0;         // the highest ledger seq validated so far
    std::optional<PublicKey> vlKey_;  // the UNL's publisher (master key)
    LedgerIndex xahau_ = 0;     // the last validated ledger whose exports went
    LedgerIndex ingested_ = 0;  // the validated ledger at the last ttINGEST
    std::chrono::steady_clock::time_point nextVL_{};

public:
    Peer(
        Config& cfg,
        std::string host,
        std::string port,
        std::optional<PublicKey> pin)
        : cfg_(cfg)
        , app_(cfg.app)
        , j_(app_.journal("ExportRelay"))
        , host_(std::move(host))
        , port_(std::move(port))
        , pin_(std::move(pin))
    {
    }

    void
    run()
    {
        while (!cfg_.stop)
        {
            try
            {
                session();
            }
            catch (std::exception const& e)
            {
                JLOG(j_.warn()) << host_ << ": " << e.what();
            }
            for (int i = 0; i < 50 && !cfg_.stop; ++i)
                std::this_thread::sleep_for(200ms);
        }
    }

private:
    void
    session()
    {
        Conn c(host_, port_, cfg_.stop);
        auto const sv = makeSharedValue(c.s, j_);
        if (!sv)
            Throw<std::runtime_error>("no shared value");
        auto req = makeRequest(false, false, false, false, false);
        buildHandshake(req, *sv, cfg_.nid, {}, c.ep.address(), app_);
        c.io([&](auto h) { http::async_write(c.s, req, h); });
        boost::beast::flat_buffer buf;
        http_response_type res;
        c.io([&](auto h) { http::async_read(c.s, buf, res, h); });
        if (res.result() != http::status::switching_protocols)
            Throw<std::runtime_error>("refused: " + std::string(res.reason()));
        auto const remote =
            verifyHandshake(res, *sv, cfg_.nid, {}, c.ep.address(), app_);
        if (pin_ && *pin_ != remote)
            Throw<std::runtime_error>(
                "peer is " + toBase58(TokenType::NodePublic, remote) +
                ", not the configured node key");
        JLOG(j_.info()) << "connected to " << host_ << " ("
                        << toBase58(TokenType::NodePublic, remote) << ")";

        send_ = [&](::google::protobuf::Message const& m, int type) {
            Message msg(m, type);
            auto const& b = msg.getBuffer(compression::Compressed::Off);
            c.io([&](auto h) { asio::async_write(c.s, asio::buffer(b), h); });
        };

        // What the last session was still fetching went unanswered with it:
        // ask again, or a transaction returning an export in one of those
        // ledgers would never be imported by this relay.
        for (auto& [h, l] : ledgers_)
            if (l.state == 1)
            {
                l.pending = 0;
                if (!l.info)
                    request(h, protocol::liBASE);
                request(h, protocol::liTX_NODE, {SHAMapNodeID{}});
            }

        while (!cfg_.stop)
        {
            if (!cfg_.vlURL.empty() &&
                std::chrono::steady_clock::now() >= nextVL_)
                fetchVL();
            exports();

            // frames: 4 byte size (uncompressed: top 6 bits clear), 2 byte type
            for (std::uint8_t const* p; buf.size() >= 6; buf.consume(6 + n_))
            {
                p = static_cast<std::uint8_t const*>(buf.data().data());
                n_ = (std::size_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) |
                    p[3];
                if (n_ > (64 << 20))
                    Throw<std::runtime_error>("bad frame");
                if (buf.size() < 6 + n_)
                    break;
                dispatch((p[4] << 8) | p[5], p + 6);
            }
            buf.commit(c.io(
                [&](auto h) { c.s.async_read_some(buf.prepare(1 << 16), h); },
                90));
        }
    }

    std::size_t n_ = 0;  // size of the frame being dispatched

    void
    dispatch(int type, std::uint8_t const* data)
    {
        auto const parse = [&](auto& m) {
            if (!m.ParseFromArray(data, n_))
                Throw<std::runtime_error>("bad message");
            return true;
        };
        protocol::TMPing ping;
        protocol::TMValidatorList vl;
        protocol::TMValidatorListCollection vls;
        protocol::TMValidation val;
        protocol::TMLedgerData ld;
        switch (type)
        {
            case protocol::mtPING:
                if (parse(ping) && ping.type() == protocol::TMPing::ptPING)
                {
                    ping.set_type(protocol::TMPing::ptPONG);
                    send_(ping, protocol::mtPING);
                }
                break;
            case protocol::mtVALIDATORLIST:
                if (parse(vl))
                    onVL(
                        vl.manifest(), vl.blob(), vl.signature(), vl.version());
                break;
            case protocol::mtVALIDATORLISTCOLLECTION:
                if (parse(vls))
                    for (auto const& b : vls.blobs())
                        onVL(
                            b.has_manifest() ? b.manifest() : vls.manifest(),
                            b.blob(),
                            b.signature(),
                            vls.version());
                break;
            case protocol::mtVALIDATION:
                if (parse(val))
                    onValidation(val.validation());
                break;
            case protocol::mtLEDGER_DATA:
                if (parse(ld))
                    onLedgerData(ld);
                break;
        }
    }

    void
    fetchVL()
    {
        nextVL_ = std::chrono::steady_clock::now() + 1h;
        try
        {
            static std::regex const re{R"(https://([^/:]+)(?::(\d+))?(/.*)?)"};
            std::smatch u;
            if (!std::regex_match(cfg_.vlURL, u, re))
                Throw<std::runtime_error>("not an https URL");
            Conn c(u[1].str(), u[2].matched ? u[2].str() : "443", cfg_.stop);
            http::request<http::empty_body> req{
                http::verb::get, u[3].matched ? u[3].str() : "/", 11};
            req.set(http::field::host, u[1].str());
            c.io([&](auto h) { http::async_write(c.s, req, h); });
            boost::beast::flat_buffer buf;
            http::response<http::string_body> res;
            c.io([&](auto h) { http::async_read(c.s, buf, res, h); });

            // v2 lists carry blobs_v2, v1 lists are their own single blob
            Json::Value v, blobs;
            if (!Json::Reader().parse(res.body(), v) || !v.isObject())
                Throw<std::runtime_error>("not a validator list");
            if (!(blobs = v["blobs_v2"]).isArray() || blobs.size() == 0)
                (blobs = Json::arrayValue).append(v);
            for (auto const& b : blobs)
            {
                // Json::Value::operator[] on a non-object is undefined, not
                // an exception, so check the shape before reading it
                if (!b.isObject() || !b[jss::blob].isString() ||
                    !b[jss::signature].isString())
                    continue;
                auto const& m = b.isMember(jss::manifest) ? b : v;
                if (!m[jss::manifest].isString() ||
                    !v[jss::version].isIntegral())
                    continue;
                onVL(
                    m[jss::manifest].asString(),
                    b[jss::blob].asString(),
                    b[jss::signature].asString(),
                    v[jss::version].asUInt());
            }
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn()) << cfg_.vlURL << ": " << e.what();
        }
    }

    // the key as Import will look it up, if it may sign an XPOP's VL
    std::optional<std::string>
    trusted(PublicKey const& pk) const
    {
        // and only if it is trusted for this relay's network: Import takes
        // a proof's network from the key that signed its list, so another
        // network's list must never become this one's UNL
        for (auto const& [hex, k] : app_.config().IMPORT_VL_KEYS)
            if (k == pk)
                return app_.config().importVLNetwork(hex) == cfg_.network()
                    ? std::optional{hex}
                    : std::nullopt;
        auto const l = app_.getLedgerMaster().getValidatedLedger();
        auto const unl = l ? l->read(keylet::UNLReport()) : nullptr;
        if (unl && unl->isFieldPresent(sfImportVLKeys))
            for (auto const& k : unl->getFieldArray(sfImportVLKeys))
                if (PublicKey(k[sfPublicKey]) == pk &&
                    k[~sfRemoteNetworkID].value_or(0) == cfg_.network())
                    return strHex(pk);
        return std::nullopt;
    }

    void
    onVL(
        std::string const& manifest,
        std::string const& blob,
        std::string const& sig,
        std::uint32_t version)
    {
        auto const m = deserializeManifest(base64_decode(manifest));
        auto const pk = m ? trusted(m->masterKey) : std::nullopt;
        auto const data = base64_decode(blob);
        auto const s = strUnHex(sig);
        auto const now = app_.timeKeeper().now().time_since_epoch().count();
        Json::Value list;
        if (!pk || !m->signingKey || !m->verify() || !s ||
            !verify(*m->signingKey, makeSlice(data), makeSlice(*s)) ||
            !Json::Reader().parse(data, list) || !list.isObject() ||
            !list[jss::validators].isArray() ||
            list[jss::sequence].asUInt() <= vlSeq_ ||
            list[jss::expiration].asUInt() <= now ||
            list[jss::effective].asUInt() > now)
            return;

        keys_.clear();
        for (auto const& v : list[jss::validators])
            if (auto const vm = v.isObject() && v[jss::manifest].isString()
                    ? deserializeManifest(
                          base64_decode(v[jss::manifest].asString()))
                    : std::nullopt;
                vm && vm->signingKey && vm->verify())
                keys_.insert(*vm->signingKey);

        // counted as Import counts it
        quorum_ =
            std::max<std::size_t>(1, list[jss::validators].size() * 4 / 5);
        vlSeq_ = list[jss::sequence].asUInt();
        vlKey_ = m->masterKey;
        unl_ = Json::objectValue;
        unl_[jss::public_key] = *pk;
        unl_[jss::manifest] = manifest;
        unl_[jss::blob] = blob;
        unl_[jss::signature] = sig;
        unl_[jss::version] = version;
        JLOG(j_.info()) << "VL " << vlSeq_ << ", " << keys_.size() << " keys";
    }

    void
    request(
        uint256 const& h,
        protocol::TMLedgerInfoType type,
        std::vector<SHAMapNodeID> const& ids = {})
    {
        protocol::TMGetLedger m;
        m.set_itype(type);
        m.set_ledgerhash(h.data(), h.size());
        for (auto const& id : ids)
            m.add_nodeids(id.getRawString());
        if (type == protocol::liTX_NODE)
            m.set_querydepth(3), ++ledgers_[h].pending;
        send_(m, protocol::mtGET_LEDGER);
    }

    void
    onValidation(std::string const& blob)
    {
        std::optional<STValidation> v;
        try
        {
            SerialIter sit(makeSlice(blob));
            v.emplace(
                sit, [](PublicKey const& pk) { return calcNodeID(pk); }, false);
            if (!keys_.count(v->getSignerPublic()) || !v->isValid())
                return;
        }
        catch (std::exception const&)
        {
            return;
        }

        // Ledgers are kept relative to the highest seq validated, so a
        // replay of old validations (in any order) opens nothing new, and
        // never more than maxLedgers at once: the oldest goes first.
        auto const seq = (*v)[~sfLedgerSequence].value_or(0);
        if (seq + 32 < highest_)
            return;
        highest_ = std::max(highest_, seq);
        std::erase_if(ledgers_, [this](auto const& e) {
            return e.second.seq + 32 < highest_;
        });

        auto const h = v->getLedgerHash();
        if (cfg_.maxLedgers && !ledgers_.count(h) &&
            ledgers_.size() >= cfg_.maxLedgers)
        {
            auto const oldest = std::ranges::min_element(
                ledgers_, {}, [](auto const& e) { return e.second.seq; });
            if (oldest->second.seq >= seq)
                return;
            ledgers_.erase(oldest);
        }
        auto& l = ledgers_[h];
        l.seq = seq;
        l.vals[toBase58(TokenType::NodePublic, v->getSignerPublic())] =
            strHex(blob);
        if (l.state == 0 && l.vals.size() >= quorum_)
        {
            l.state = 1;
            request(h, protocol::liBASE);
            request(h, protocol::liTX_NODE, {SHAMapNodeID{}});
        }
    }

    void
    onLedgerData(protocol::TMLedgerData const& m)
    {
        auto const it = m.ledgerhash().size() == 32
            ? ledgers_.find(uint256::fromVoid(m.ledgerhash().data()))
            : ledgers_.end();
        if (it == ledgers_.end() || it->second.state != 1)
            return;
        auto& [h, l] = *it;
        try
        {
            if (m.type() == protocol::liBASE && m.nodes_size() > 0)
            {
                auto const info =
                    deserializeHeader(makeSlice(m.nodes(0).nodedata()));
                if (calculateLedgerHash(info) == h)
                    l.info = info;
            }
            else if (m.type() == protocol::liTX_NODE)
            {
                // only the answer to a request of ours: one reply each
                if (l.pending == 0)
                    return;
                --l.pending;
                for (auto const& n : m.nodes())
                {
                    // a peer streaming nodes it was never asked for, or
                    // never sending the header, gets this far and no further
                    l.bytes += n.nodedata().size();
                    if (cfg_.maxLedgerBytes && l.bytes > cfg_.maxLedgerBytes)
                    {
                        JLOG(j_.warn())
                            << host_ << ": ledger " << l.seq << " exceeds "
                            << cfg_.maxLedgerBytes
                            << " bytes of nodes (max_ledger_bytes), giving "
                               "up on it";
                        l.state = 2;
                        l.nodes.clear();
                        return;
                    }
                    if (auto const node = SHAMapTreeNode::makeFromWire(
                            makeSlice(n.nodedata())))
                        l.nodes[node->getHash().as_uint256()] = node;
                }
            }
        }
        catch (std::exception const&)
        {
        }
        if (!l.info || l.pending > 0)
            return;

        std::vector<SHAMapNodeID> missing;
        std::vector<SHAMapLeafNode const*> leaves;
        walk(l, l.info->txHash, {}, missing, leaves);
        if (missing.empty())
        {
            l.state = 2;
            for (auto const* leaf : leaves)
                relay(l, *leaf);
            ingest(h, l);
        }
        else if (++l.rounds < 16)
            request(h, protocol::liTX_NODE, missing);
        else
            l.state = 2;
        // the tree is only needed to be walked once
        if (l.state == 2)
            l.nodes.clear();
    }

    static void
    walk(
        Ledger const& l,
        uint256 const& h,
        SHAMapNodeID const& id,
        std::vector<SHAMapNodeID>& missing,
        std::vector<SHAMapLeafNode const*>& leaves)
    {
        auto const n = l.nodes.find(h);
        if (h == beast::zero)
            return;
        if (n == l.nodes.end())
            return missing.push_back(id);
        if (auto const in = dynamic_cast<SHAMapInnerNode const*>(&*n->second))
            for (int b = 0; b < 16; ++b)
                walk(
                    l,
                    in->getChildHash(b).as_uint256(),
                    id.getChildNodeID(b),
                    missing,
                    leaves);
        else if (
            auto const f = dynamic_cast<SHAMapLeafNode const*>(&*n->second))
            leaves.push_back(f);
    }

    // XPOP proof, list form: each inner node on the path to `key` as its 16
    // child hashes, the one on the path expanded unless it is the leaf
    static Json::Value
    proof(Ledger const& l, uint256 const& h, uint256 const& key, int depth = 0)
    {
        Json::Value a{Json::arrayValue};
        auto const& in = dynamic_cast<SHAMapInnerNode const&>(*l.nodes.at(h));
        int const path = (key.data()[depth / 2] >> (depth % 2 ? 0 : 4)) & 0xF;
        for (int b = 0; b < 16; ++b)
        {
            auto const c = in.getChildHash(b).as_uint256();
            if (b == path && l.nodes.at(c)->isInner())
                a.append(proof(l, c, key, depth + 1));
            else
                a.append(to_string(c));
        }
        return a;
    }

    // what an XPOP and an XPOF share: the ledger's header and what proves it
    Json::Value
    proven(Ledger const& l) const
    {
        auto const& i = *l.info;
        Json::Value x;
        auto& lg = x[jss::ledger];
        lg[jss::index] = i.seq;
        lg[jss::coins] = std::to_string(i.drops.drops());
        lg[jss::phash] = to_string(i.parentHash);
        lg[jss::txroot] = to_string(i.txHash);
        lg[jss::acroot] = to_string(i.accountHash);
        lg[jss::pclose] = i.parentCloseTime.time_since_epoch().count();
        lg[jss::close] = i.closeTime.time_since_epoch().count();
        lg[jss::cres] = i.closeTimeResolution.count();
        lg[jss::flags] = i.closeFlags;
        x[jss::validation][jss::data] = l.vals;
        x[jss::validation][jss::unl] = unl_;
        return x;
    }

    // The ledger as a proof of fees (Ingest.h), if one is due, its parent's
    // header is still held and the ledger is newer than the one recorded
    // (no other is taken). Due by the record: ingest_every ledgers since it
    // last changed, whoever changed it; and by this relay's own last, so
    // that one the ledger refuses is not sent again for every ledger there.
    // The whole tree was fetched for relay() anyway.
    void
    ingest(uint256 const& h, Ledger const& l)
    {
        auto const now = app_.getLedgerMaster().getValidLedgerIndex();
        if (!cfg_.ingestEvery || now < ingested_ + cfg_.ingestEvery)
            return;
        auto const view = app_.openLedger().current();
        auto const fee = view->read(keylet::exportFee(cfg_.network()));
        auto const parent = ledgers_.find(l.info->parentHash);
        if ((fee &&
             ((*fee)[sfLedgerSequence] >= l.seq ||
              now < (*fee)[sfPreviousTxnLgrSeq] + cfg_.ingestEvery)) ||
            parent == ledgers_.end() || !parent->second.info)
            return;

        Json::Value x = proven(l);
        // An XPOF is only taken in its canonical form (Ingest.cpp): the
        // list's publisher by its key in upper-case hex, whatever case
        // [import_vl_keys] spells it in, and the list itself re-encoded,
        // whichever base64 or hex dialect its publisher serves it in. The
        // rest of what is built here is in that form already.
        if (!vlKey_)
            return;
        {
            auto& u = x[jss::validation][jss::unl];
            auto const sig = strUnHex(unl_[jss::signature].asString());
            if (!sig)
                return;
            u[jss::public_key] = strHex(*vlKey_);
            u[jss::manifest] =
                base64_encode(base64_decode(unl_[jss::manifest].asString()));
            u[jss::blob] =
                base64_encode(base64_decode(unl_[jss::blob].asString()));
            u[jss::signature] = strHex(*sig);
            u[jss::version] = 1;
        }
        Serializer ph;
        addRaw(*parent->second.info, ph);
        x["parent"] = strHex(ph.slice());
        auto& tree = x["tree"] = Json::arrayValue;
        for (auto const& [hash, node] : l.nodes)
            if (node->isInner())
            {
                Serializer s;
                node->serializeForWire(s);
                tree.append(strHex(s.slice()));
            }
        auto& samples = x["samples"] = Json::arrayValue;
        for (auto const& leaf :
             Ingest::sampleLeaves(l.nodes, l.info->txHash, h))
        {
            auto const n = l.nodes.find(leaf);
            if (n == l.nodes.end() || !n->second->isLeaf())
                return;
            SerialIter sit(static_cast<SHAMapLeafNode const&>(*n->second)
                               .peekItem()
                               ->slice());
            auto& s = samples.append(Json::objectValue);
            s[jss::blob] = strHex(sit.getVL());
            s[jss::meta] = strHex(sit.getVL());
        }

        auto const nid = app_.config().NETWORK_ID;
        auto const blob = to_string(x);
        // The one transaction that may carry it: from no account, so
        // nothing is signed and there is nothing to pay or sequence. Never
        // one the ledger would refuse: peers would take that for a bad
        // signature.
        auto tx = Ingest::make(nid, cfg_.network(), l.seq, makeSlice(blob));
        if (!Ingest::proof(tx, nid, j_))
        {
            JLOG(j_.warn())
                << "not ingesting ledger " << l.seq << " of network "
                << cfg_.network() << ": " << blob.size()
                << " bytes, too large or not provable";
            return;
        }
        app_.getOPs().submitTransaction(
            std::make_shared<STTx const>(std::move(tx)));
        ingested_ = now;
        JLOG(j_.info()) << "ingesting XRPL ledger " << l.seq << ", "
                        << blob.size() << " bytes";
    }

    // An XRPL transaction the ledger may want (Import::preclaim): an export
    // coming back, or an exporter's own TicketCreate, SignerListSet or spend
    // of a pooled ticket. Only an account whose signer list is recorded here
    // can be either. Built as soon as its XRPL ledger is complete, since the
    // ledger data goes 32 ledgers on, and kept while the Import that would
    // register its ticket here may still be on its way.
    struct Candidate
    {
        AccountID account;
        std::optional<std::uint32_t> ticket;
        TxType type;
        uint256 txid;
        std::string xpop;
        LedgerIndex since;  // the validated Xahau ledger when first seen
    };
    std::deque<Candidate> deferred_;
    static constexpr std::uint32_t deferFor = 64;  // Xahau ledgers
    static constexpr std::size_t deferMax = 4096;

    // whether Import::preclaim would take it against `view` now
    bool
    wanted(ReadView const& view, Candidate const& c) const
    {
        auto const nid = cfg_.network();
        auto const ex = view.read(keylet::exporter(c.account, nid));
        bool const exporter = ex && isExporter(*ex);
        if (c.ticket)
            return view.exists(
                       keylet::shadowTicket(c.account, nid, *c.ticket)) ||
                (exporter && ExportTickets::load(*ex).contains(*c.ticket));
        return exporter &&
            (c.type == ttTICKET_CREATE || c.type == ttSIGNER_LIST_SET);
    }

    void
    submitImport(Candidate const& c)
    {
        auto const nid = app_.config().NETWORK_ID;
        // not a structured binding: the lambda below captures these
        PublicKey const& pk = relayKey().first;
        SecretKey const& sk = relayKey().second;
        // free and sequence 0: the XRPL fee paid for it (see isLoopback)
        STTx imp(ttIMPORT, [&](auto& o) {
            o[sfAccount] = c.account;
            o[sfSequence] = 0;
            o[sfFee] = XRPAmount{0};
            o[sfSigningPubKey] = pk.slice();
            o[sfBlob] = makeSlice(c.xpop);
            if (nid > 1024)
                o[sfNetworkID] = nid;
        });
        imp.sign(pk, sk);
        app_.getOPs().submitTransaction(
            std::make_shared<STTx const>(std::move(imp)));
        JLOG(j_.info()) << "importing " << c.txid;
    }

    void
    relay(Ledger const& l, SHAMapLeafNode const& leaf)
    {
        if (leaf.getType() != SHAMapNodeType::tnTRANSACTION_MD)
            return;
        auto const& item = leaf.peekItem();
        try
        {
            SerialIter sit(item->slice());
            auto const tx = sit.getVL();
            auto const meta = sit.getVL();
            SerialIter st(makeSlice(tx));
            STTx const t(st);
            auto const view = app_.openLedger().current();
            auto const ex =
                view->read(keylet::exporter(t[sfAccount], cfg_.network()));
            if (!ex || !ex->isFieldPresent(sfExportSignerList))
                return;

            Candidate c{
                t[sfAccount],
                t[~sfTicketSequence],
                t.getTxnType(),
                t.getTransactionID(),
                {},
                app_.getLedgerMaster().getValidLedgerIndex()};
            if (!c.ticket && c.type != ttTICKET_CREATE &&
                c.type != ttSIGNER_LIST_SET)
                return;

            Json::Value x = proven(l);
            auto& xt = x[jss::transaction];
            xt[jss::blob] = strHex(tx);
            xt[jss::meta] = strHex(meta);
            xt[jss::proof] = proof(l, l.info->txHash, item->key());
            c.xpop = to_string(x);

            if (wanted(*view, c))
            {
                submitImport(c);
                return;
            }
            // its ticket is not registered here yet: the Import of the
            // TicketCreate that made it may still be in flight, or sit in an
            // earlier XRPL ledger whose data arrived after this one
            if (deferred_.size() >= deferMax)
                deferred_.pop_front();
            deferred_.push_back(std::move(c));
        }
        catch (std::exception const& e)
        {
            JLOG(j_.debug()) << "skipping " << item->key() << ": " << e.what();
        }
    }

    // exports at quorum, kept and resent while the ledger waits for them:
    // XRPL may have been short of our transaction for a while (fee, load, a
    // peer dropping it). A ticketed one waits while its shadow ticket stands
    // (the ticket is only gone once something used it there); the sequenced
    // backstop TicketCreate of an empty pool while sfExportTicketPending
    // holds its Sequence. After the window the signed transaction exists
    // only in the DeletedNode of its ttEXPORT_FINAL, so those are read back
    // from every validated ledger seen, and after a restart or a long
    // disconnection from the last recoverDepth of them.
    struct Held
    {
        AccountID owner;
        std::optional<std::uint32_t> ticket;
        std::uint32_t sequence;
        std::optional<std::uint32_t> lastLedger;  // its LastLedgerSequence
        uint256 id;     // without signatures, as its shadow ticket names it
        XRPAmount fee;  // what it pays there
        Blob blob;
    };
    std::map<uint256, Held> held_;  // by shadow ticket, or account root key
    static constexpr std::uint32_t resendEvery = 8;
    static constexpr std::uint32_t recoverDepth = 256;

    bool
    waitedFor(ReadView const& view, Held const& h) const
    {
        auto const nid = cfg_.network();
        if (h.ticket)
        {
            // its shadow ticket stands and is still this export's: one
            // exported again on the same ticket makes this one stale
            auto const st =
                view.read(keylet::shadowTicket(h.owner, nid, *h.ticket));
            return st && (*st)[sfTransactionHash] == h.id;
        }
        auto const ex = view.read(keylet::exporter(h.owner, nid));
        return ex && (*ex)[~sfExportTicketPending] == h.sequence;
    }

    // Whether the account can pay for it there, by the balance the ledger
    // last saw proven (ltEXPORTER.sfBalance; unknown counts as yes). One
    // that cannot is held back rather than dropped: sent an eighth as
    // often, since the ledger only learns of money arriving there with the
    // account's next transaction that comes back, and that may be this one.
    bool
    funded(ReadView const& view, Held const& h) const
    {
        auto const ex = view.read(keylet::exporter(h.owner, cfg_.network()));
        return !ex || !ex->isFieldPresent(sfBalance) ||
            ex->getFieldAmount(sfBalance).xrp() >= h.fee;
    }

    void
    submit(Blob const& blob)
    {
        protocol::TMTransaction m;
        m.set_rawtransaction(blob.data(), blob.size());
        m.set_status(protocol::tsNEW);
        send_(m, protocol::mtTRANSACTION);
    }

    // hold and send `in`, the export of `owner` with unsigned id `id`, if
    // the ledger still waits for it: a ticketed one if its shadow ticket
    // still stands and still names it (a later export may have taken the
    // ticket over), a sequenced one while its Sequence is the one out
    void
    hold(
        ReadView const& now,
        AccountID const& owner,
        STObject const& in,
        uint256 const& id)
    {
        Held h{
            owner,
            in[~sfTicketSequence],
            in[~sfSequence].value_or(0),
            in[~sfLastLedgerSequence],
            id,
            in.getFieldAmount(sfFee).xrp(),
            {}};
        uint256 key;
        if (h.ticket)
        {
            auto const st =
                keylet::shadowTicket(owner, cfg_.network(), *h.ticket);
            auto const sle = now.read(st);
            if (!sle || (*sle)[sfTransactionHash] != id)
                return;
            key = st.key;
        }
        else
        {
            if (h.sequence == 0 || !waitedFor(now, h))
                return;
            key = keylet::exporter(owner, cfg_.network()).key;
        }
        Serializer s;
        in.add(s);
        h.blob = s.peekData();
        // already held as it is: the same export reaches here from its
        // final, from the ledger's own record of it and from every later
        // pass, and is sent on the resend schedule, not once for each
        if (auto const it = held_.find(key); it != held_.end() &&
            it->second.id == id && it->second.blob == h.blob)
            return;
        if (funded(now, h))
            submit(h.blob);
        else
            JLOG(j_.info())
                << "holding back export " << id << " of " << toBase58(owner)
                << ": its balance on network " << cfg_.network()
                << " does not cover its fee of " << h.fee;
        held_.insert_or_assign(key, std::move(h));
    }

    // Every export in flight that the ledger holds signed (the shadow
    // tickets of the exporters listed for this network, see ttEXPORT_FINAL):
    // what a relay that has just started, or was away for longer than
    // recoverDepth, still has to carry. The ledger is the one record of
    // what is wanted, so nothing stale comes back from here: a shadow ticket
    // that has gone, or been taken over by a later export, holds nothing.
    void
    recover(ReadView const& l)
    {
        forEachExporter(l, [&](uint256 const& key) {
            auto const ex = l.read(Keylet{ltEXPORTER, key});
            if (!ex || (*ex)[sfRemoteNetworkID] != cfg_.network() ||
                !ex->isFieldPresent(sfIndexes))
                return;
            for (auto const& k : ex->getFieldV256(sfIndexes))
            {
                auto const st = l.read(Keylet{ltSHADOW_TICKET, k});
                if (st && st->isFieldPresent(sfExportedTxn))
                    hold(
                        l,
                        (*st)[sfAccount],
                        st->peekAtField(sfExportedTxn).downcast<STObject>(),
                        (*st)[sfTransactionHash]);
            }
        });
    }

    // the exports whose window closed in `l`, as their finals left them
    void
    finals(ReadView const& l, ReadView const& now)
    {
        for (auto const& [tx, meta] : l.txs)
        {
            if (!tx || !meta || tx->getTxnType() != ttEXPORT_FINAL ||
                !meta->isFieldPresent(sfAffectedNodes))
                continue;
            for (auto const& n : meta->getFieldArray(sfAffectedNodes))
            {
                if (n.getFName() != sfDeletedNode ||
                    n[~sfLedgerEntryType] !=
                        static_cast<std::uint16_t>(ltEXPORTED_TXN) ||
                    !n.isFieldPresent(sfFinalFields))
                    continue;
                auto const& ff =
                    n.peekAtField(sfFinalFields).downcast<STObject>();
                if (!ff.isFieldPresent(sfExportedTxn) ||
                    !ff.isFieldPresent(sfSignerEntries) ||
                    ff[~sfRemoteNetworkID] != cfg_.network() ||
                    exportWeight(ff) < ff[~sfSignerQuorum].value_or(0))
                    continue;
                hold(
                    now,
                    ff[sfOwner],
                    ff.peekAtField(sfExportedTxn).downcast<STObject>(),
                    ff[sfTransactionHash]);
            }
        }
    }

    void
    exports()
    {
        auto const l = app_.getLedgerMaster().getValidatedLedger();
        if (!l || l->info().seq == xahau_ || !l->rules().enabled(featureExport))
            return;
        auto const seq = l->info().seq;

        // every ledger since the last one seen here, bounded by recoverDepth
        // and by the history this server keeps
        auto from = xahau_ ? xahau_ + 1 : 1;
        // just started, or away for longer than the finals read back below
        // reach: what is in flight is then taken from the ledger itself
        bool const resync = xahau_ == 0 || from + recoverDepth < seq;
        if (from + recoverDepth < seq)
            from = seq - recoverDepth;
        xahau_ = seq;
        if (resync)
            recover(*l);
        for (auto s = from; s < seq; ++s)
            if (auto const past = app_.getLedgerMaster().getLedgerBySeq(s))
                finals(*past, *l);
        finals(*l, *l);

        // still pending but already at quorum: no need to wait for the window
        auto const end = keylet::exportedTxn(seq + 1, beast::zero).key;
        for (auto k = keylet::exportedTxn(0, beast::zero).key;
             auto const next = l->succ(k, end);
             k = *next)
        {
            auto const sle = l->read(Keylet{ltEXPORTED_TXN, *next});
            if (!sle || (*sle)[sfRemoteNetworkID] != cfg_.network() ||
                exportWeight(*sle) < (*sle)[sfSignerQuorum])
                continue;
            hold(
                *l,
                (*sle)[sfOwner],
                sle->peekAtField(sfExportedTxn).downcast<STObject>(),
                (*sle)[sfTransactionHash]);
        }

        for (auto it = held_.begin(); it != held_.end();)
        {
            if (!waitedFor(*l, it->second))
                it = held_.erase(it);
            else
            {
                // (not once the other network is past its last ledger: it
                // can no longer be included, and the ledger will expire it)
                if (seq %
                            (funded(*l, it->second) ? resendEvery
                                                    : resendEvery * 8) ==
                        0 &&
                    !(it->second.lastLedger &&
                      highest_ > *it->second.lastLedger))
                    submit(it->second.blob);
                ++it;
            }
        }

        // XRPL transactions waiting for the Import that registers their
        // ticket here; forgotten once that has had deferFor ledgers to land
        auto const open = app_.openLedger().current();
        for (auto it = deferred_.begin(); it != deferred_.end();)
        {
            if (wanted(*open, *it))
            {
                submitImport(*it);
                it = deferred_.erase(it);
            }
            else if (it->since + deferFor < seq)
                it = deferred_.erase(it);
            else
                ++it;
        }
    }
};

class Relay final : public ExportRelay
{
    Config cfg_;
    std::vector<std::thread> threads_;

public:
    explicit Relay(Application& app, Section const& sec)
        : cfg_{app, sec.get<std::string>("vl_url").value_or("")}
    {
        if (auto const nid = sec.get<std::uint32_t>("network_id").value_or(0))
            cfg_.nid = nid;
        cfg_.ingestEvery =
            sec.get<std::uint32_t>("ingest_every").value_or(cfg_.ingestEvery);
        cfg_.maxLedgers = sec.get<std::size_t>("max_ledgers").value_or(0);
        cfg_.maxLedgerBytes =
            sec.get<std::uint64_t>("max_ledger_bytes").value_or(0);
        for (auto const& line : sec.values())
        {
            std::istringstream ss(line);
            std::string host, port = "51235", key;
            ss >> host >> port >> key;
            std::optional<PublicKey> pin;
            if (!key.empty())
            {
                pin = parseBase58<PublicKey>(TokenType::NodePublic, key);
                if (!pin)
                    Throw<std::runtime_error>(
                        "[" + sec.name() + "] " + host + ": '" + key +
                        "' is not a node public key");
            }
            threads_.emplace_back(
                [this, host, port, pin] { Peer(cfg_, host, port, pin).run(); });
        }
    }

    ~Relay() override
    {
        cfg_.stop = true;
        for (auto& t : threads_)
            t.join();
    }
};

// one Relay per [export_relay*] section, each for its own network
class Relays final : public ExportRelay
{
    std::vector<std::unique_ptr<Relay>> relays_;

public:
    explicit Relays(Application& app)
    {
        for (auto const& [name, sec] : app.config())
            if (name == SECTION_EXPORT_RELAY ||
                name.rfind(std::string(SECTION_EXPORT_RELAY) + "_", 0) == 0)
                relays_.push_back(std::make_unique<Relay>(app, sec));
    }
};

}  // namespace

std::unique_ptr<ExportRelay>
makeExportRelay(Application& app)
{
    if (app.config().standalone())
        return nullptr;
    return std::make_unique<Relays>(app);
}

}  // namespace ripple
