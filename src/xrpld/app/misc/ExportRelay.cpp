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
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <regex>
#include <thread>

/*  [xrpl_relay]: one XRPL peer connection per "host [port]" line, plus
    optional vl_url= and network_id= (the XRPL's, default 0).

    XRPL to Xahau. The UNL is the newest validator list signed by a key this
    network trusts for Import ([import_vl_keys] or UNLReport ImportVLKeys),
    from the peer or vl_url. Import consults only the manifests embedded in
    it, so manifest gossip is not needed. When a ledger has validations from
    80% of it, its header and transaction tree are fetched, and every txn in
    it returning a pending export (its shadow ticket exists) is imported.

    Xahau to XRPL. On each validated ledger, pending exports whose signatures
    meet the exporter's quorum are sent to the peer.
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
    std::optional<std::uint32_t> nid;
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
        ep = asio::ip::tcp::resolver(ioc).resolve(host, port)->endpoint();
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
    };

    Config& cfg_;
    Application& app_;
    beast::Journal const j_;
    std::string const host_, port_;
    std::function<void(::google::protobuf::Message const&, int)> send_;

    Json::Value unl_;           // the XPOPs' validation.unl
    std::uint32_t vlSeq_ = 0;   // its sequence
    std::set<PublicKey> keys_;  // its validators' signing keys
    std::size_t quorum_ = 0;
    std::map<uint256, Ledger> ledgers_;
    LedgerIndex xahau_ = 0;  // the last validated ledger whose exports went
    std::chrono::steady_clock::time_point nextVL_{};

public:
    Peer(Config& cfg, std::string host, std::string port)
        : cfg_(cfg)
        , app_(cfg.app)
        , j_(app_.journal("ExportRelay"))
        , host_(std::move(host))
        , port_(std::move(port))
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
        verifyHandshake(res, *sv, cfg_.nid, {}, c.ep.address(), app_);
        JLOG(j_.info()) << "connected to " << host_;

        send_ = [&](::google::protobuf::Message const& m, int type) {
            Message msg(m, type);
            auto const& b = msg.getBuffer(compression::Compressed::Off);
            c.io([&](auto h) { asio::async_write(c.s, asio::buffer(b), h); });
        };

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
                onVL(
                    (b.isMember(jss::manifest) ? b : v)[jss::manifest]
                        .asString(),
                    b[jss::blob].asString(),
                    b[jss::signature].asString(),
                    v[jss::version].asUInt());
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
        for (auto const& [hex, k] : app_.config().IMPORT_VL_KEYS)
            if (k == pk)
                return hex;
        auto const l = app_.getLedgerMaster().getValidatedLedger();
        auto const unl = l ? l->read(keylet::UNLReport()) : nullptr;
        if (unl && unl->isFieldPresent(sfImportVLKeys))
            for (auto const& k : unl->getFieldArray(sfImportVLKeys))
                if (PublicKey(k[sfPublicKey]) == pk)
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
            !Json::Reader().parse(data, list) ||
            !list[jss::validators].isArray() ||
            list[jss::sequence].asUInt() <= vlSeq_ ||
            list[jss::expiration].asUInt() <= now ||
            list[jss::effective].asUInt() > now)
            return;

        keys_.clear();
        for (auto const& v : list[jss::validators])
            if (auto const vm = deserializeManifest(
                    base64_decode(v[jss::manifest].asString()));
                vm && vm->signingKey && vm->verify())
                keys_.insert(*vm->signingKey);

        // counted as Import counts it
        quorum_ =
            std::max<std::size_t>(1, list[jss::validators].size() * 4 / 5);
        vlSeq_ = list[jss::sequence].asUInt();
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

        auto const h = v->getLedgerHash();
        auto& l = ledgers_[h];
        l.seq = (*v)[~sfLedgerSequence].value_or(0);
        l.vals[toBase58(TokenType::NodePublic, v->getSignerPublic())] =
            strHex(blob);
        if (l.state == 0 && l.vals.size() >= quorum_)
        {
            l.state = 1;
            request(h, protocol::liBASE);
            request(h, protocol::liTX_NODE, {SHAMapNodeID{}});
        }
        std::erase_if(ledgers_, [seq = l.seq](auto const& e) {
            return e.second.seq + 32 < seq;
        });
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
                for (l.pending -= l.pending > 0; auto const& n : m.nodes())
                    if (auto const node = SHAMapTreeNode::makeFromWire(
                            makeSlice(n.nodedata())))
                        l.nodes[node->getHash().as_uint256()] = node;
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
        }
        else if (++l.rounds < 16)
            request(h, protocol::liTX_NODE, missing);
        else
            l.state = 2;
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

    void
    relay(Ledger const& l, SHAMapLeafNode const& leaf)
    {
        if (leaf.getType() != SHAMapNodeType::tnTRANSACTION_MD)
            return;
        auto const nid = app_.config().NETWORK_ID;
        auto const& item = leaf.peekItem();
        try
        {
            SerialIter sit(item->slice());
            auto const tx = sit.getVL();
            auto const meta = sit.getVL();
            SerialIter st(makeSlice(tx));
            STTx const t(st);
            auto const ticket = t[~sfTicketSequence];
            auto const view = app_.openLedger().current();
            auto const acct = view->read(keylet::account(t[sfAccount]));

            // only exports coming back: other imports need their owner's key
            if (!ticket || t[~sfOperationLimit] != nid || !acct ||
                !view->exists(keylet::shadowTicket(t[sfAccount], *ticket)))
                return;

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
            auto& xt = x[jss::transaction];
            xt[jss::blob] = strHex(tx);
            xt[jss::meta] = strHex(meta);
            xt[jss::proof] = proof(l, i.txHash, item->key());
            x[jss::validation][jss::data] = l.vals;
            x[jss::validation][jss::unl] = unl_;
            auto const xpop = to_string(x);

            // not a structured binding: the lambda below captures these
            PublicKey const& pk = relayKey().first;
            SecretKey const& sk = relayKey().second;
            STTx imp(ttIMPORT, [&](auto& o) {
                o[sfAccount] = t[sfAccount];
                o[sfSequence] = (*acct)[sfSequence];
                // Import's fee for an existing account, pinned by preclaim
                o[sfFee] = view->fees().base * 10;
                o[sfSigningPubKey] = pk.slice();
                o[sfBlob] = makeSlice(xpop);
                if (nid > 1024)
                    o[sfNetworkID] = nid;
            });
            imp.sign(pk, sk);
            app_.getOPs().submitTransaction(
                std::make_shared<STTx const>(std::move(imp)));
            JLOG(j_.info()) << "importing " << t.getTransactionID();
        }
        catch (std::exception const& e)
        {
            JLOG(j_.debug()) << "skipping " << item->key() << ": " << e.what();
        }
    }

    // on each validated ledger, the pending exports that meet quorum
    void
    exports()
    {
        auto const l = app_.getLedgerMaster().getValidatedLedger();
        if (!l || l->info().seq == xahau_ || !l->rules().enabled(featureExport))
            return;
        xahau_ = l->info().seq;

        auto const end = keylet::exportedTxn(xahau_ + 1, beast::zero).key;
        for (auto k = keylet::exportedTxn(0, beast::zero).key;
             auto const next = l->succ(k, end);
             k = *next)
        {
            auto const sle = l->read(Keylet{ltEXPORTED_TXN, *next});
            if (!sle)
                continue;
            auto const& in =
                sle->peekAtField(sfExportedTxn).downcast<STObject>();
            std::uint32_t weight = 0;
            if (in.isFieldPresent(sfSigners))
                for (auto const& s : in.getFieldArray(sfSigners))
                    for (auto const& e : sle->getFieldArray(sfSignerEntries))
                        if (e[sfAccount] == s[sfAccount])
                            weight += e[sfSignerWeight];
            if (weight < (*sle)[sfSignerQuorum])
                continue;

            Serializer s;
            in.add(s);
            protocol::TMTransaction m;
            m.set_rawtransaction(s.data(), s.size());
            m.set_status(protocol::tsNEW);
            send_(m, protocol::mtTRANSACTION);
        }
    }
};

class Relay final : public ExportRelay
{
    Config cfg_;
    std::vector<std::thread> threads_;

public:
    explicit Relay(Application& app, Section const& sec)
        : cfg_{app, sec.get<std::string>("vl_url").value_or(""), std::nullopt}
    {
        if (auto const nid = sec.get<std::uint32_t>("network_id").value_or(0))
            cfg_.nid = nid;
        for (auto const& line : sec.values())
        {
            std::istringstream ss(line);
            std::string host, port = "51235";
            ss >> host >> port;
            threads_.emplace_back(
                [this, host, port] { Peer(cfg_, host, port).run(); });
        }
    }

    ~Relay() override
    {
        cfg_.stop = true;
        for (auto& t : threads_)
            t.join();
    }
};

}  // namespace

std::unique_ptr<ExportRelay>
makeExportRelay(Application& app)
{
    if (app.config().standalone() || !app.config().exists(SECTION_XRPL_RELAY))
        return nullptr;
    return std::make_unique<Relay>(
        app, app.config().section(SECTION_XRPL_RELAY));
}

}  // namespace ripple
