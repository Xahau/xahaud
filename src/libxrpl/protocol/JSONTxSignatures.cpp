//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <xrpl/basics/contract.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/PublicKey.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ripple {

namespace {

[[noreturn]] void
fail(std::string const& why)
{
    Throw<std::runtime_error>("jsontx: " + why);
}

// Not std::isdigit: that consults the locale, and every comparison in this
// file decides whether a signature verifies.
constexpr bool
digit(char c)
{
    return c >= '0' && c <= '9';
}

// json's whitespace set, exactly
constexpr bool
space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

//------------------------------------------------------------------------------
// The preimage parser.
//
// Not Json::Reader. The vendored jsoncpp reader accepts comments, stops once
// it has a root value rather than requiring end of input, keeps the last of
// two identical keys, and holds numbers as 32-bit ints or doubles - so a bare
// integer past 2^32 is refused outright and a fractional token is rounded by
// sscanf. Each of those is a way for the text a signer reads to differ from
// what executes, or for a valid transaction to be unsignable. This parser
// accepts a deliberately small language instead:
//
//   - exactly one object, with only whitespace around it;
//   - printable ASCII only (0x20-0x7E) inside strings, and no backslash at
//     all. Every string a transaction renders through getJson is plain
//     printable ASCII (hex, base58, decimal, currency codes, type names), and
//     jsontx_verify requires the preimage's strings to equal those byte for
//     byte, so nothing an escape or a non-ASCII byte could spell would ever
//     verify. Refusing them outright also rules out bidi overrides,
//     homoglyphs and terminal control sequences in the text being approved;
//   - integers spelled -?(0|[1-9][0-9]*), never -0, and within
//     +/-jsontx_max_int, kept as their exact digits rather than converted to
//     anything;
//   - no true, false or null: no transaction field renders as one;
//   - no two members of one object with the same name.
//------------------------------------------------------------------------------

struct Node
{
    enum class Kind : std::uint8_t { object, array, string, number };

    Kind kind = Kind::object;
    std::string text;               // string contents, or the number token
    std::vector<std::string> keys;  // object member names, in source order
    std::vector<Node> items;        // object member values, or array items
};

class Reader
{
    std::string_view s_;
    std::size_t p_ = 0;

    void
    skip()
    {
        while (p_ < s_.size() && space(s_[p_]))
            ++p_;
    }

    char
    peek() const
    {
        if (p_ >= s_.size())
            fail("unexpected end of document");
        return s_[p_];
    }

    void
    expect(char c, char const* what)
    {
        if (peek() != c)
            fail(what);
        ++p_;
    }

    std::string
    string()
    {
        ++p_;  // opening quote
        std::size_t const b = p_;
        for (;;)
        {
            if (p_ >= s_.size())
                fail("unterminated string");
            auto const c = static_cast<unsigned char>(s_[p_]);
            if (c == '"')
                break;
            if (c == '\\')
                fail("escape sequences are not allowed");
            if (c < 0x20 || c > 0x7E)
                fail("strings must be printable ASCII");
            ++p_;
        }
        std::string r(s_.substr(b, p_ - b));
        ++p_;  // closing quote
        return r;
    }

    std::string
    number()
    {
        std::size_t const b = p_;
        if (s_[p_] == '-')
            ++p_;
        if (p_ >= s_.size() || !digit(s_[p_]))
            fail("number must be a plain integer");
        if (s_[p_] == '0')
            ++p_;
        else
            while (p_ < s_.size() && digit(s_[p_]))
                ++p_;
        // a fraction, an exponent, a leading zero or anything else glued on
        if (p_ < s_.size() && !space(s_[p_]) && s_[p_] != ',' &&
            s_[p_] != '}' && s_[p_] != ']')
            fail("number must be a plain integer");
        std::string tok(s_.substr(b, p_ - b));
        if (tok == "-0")
            fail("number must be a plain integer");

        // The interoperable range, symmetric about zero. Sixteen digits is
        // the most 2^53 - 1 has, so the length test also keeps from_chars
        // well away from overflow.
        std::string_view const mag =
            std::string_view(tok).substr(tok.front() == '-' ? 1 : 0);
        std::uint64_t v = 0;
        if (mag.size() > 16 ||
            std::from_chars(mag.data(), mag.data() + mag.size(), v).ec !=
                std::errc{} ||
            v > jsontx_max_int)
            fail(
                "integer outside +/-(2^53 - 1); write larger values as "
                "strings");
        return tok;
    }

    void
    value(Node& n, std::size_t depth)
    {
        switch (peek())
        {
            case '{':
                object(n, depth + 1);
                return;
            case '[':
                array(n, depth + 1);
                return;
            case '"':
                n.kind = Node::Kind::string;
                n.text = string();
                return;
            case 't':
            case 'f':
                fail("boolean values are not allowed");
            case 'n':
                fail("null values are not allowed");
            case '/':
                fail("comments are not allowed");
            default:
                if (peek() == '-' || digit(peek()))
                {
                    n.kind = Node::Kind::number;
                    n.text = number();
                    return;
                }
                fail("unexpected character");
        }
    }

    void
    object(Node& n, std::size_t depth)
    {
        if (depth > jsontx_max_depth)
            fail("document nested too deeply");
        n.kind = Node::Kind::object;
        ++p_;  // '{'
        skip();
        if (peek() != '}')
        {
            for (;;)
            {
                skip();
                if (peek() != '"')
                    fail("member name must be a string");
                n.keys.push_back(string());
                skip();
                expect(':', "expected ':'");
                skip();
                value(n.items.emplace_back(), depth);
                skip();
                if (peek() == ',')
                {
                    ++p_;
                    continue;
                }
                break;
            }
        }
        expect('}', "expected ',' or '}'");

        // Two identical names are one member to jsoncpp (the last wins) and
        // may be either to whatever the signer's wallet parsed.
        std::vector<std::string_view> ks(n.keys.begin(), n.keys.end());
        std::sort(ks.begin(), ks.end());
        if (std::adjacent_find(ks.begin(), ks.end()) != ks.end())
            fail("duplicate member name");
    }

    void
    array(Node& n, std::size_t depth)
    {
        if (depth > jsontx_max_depth)
            fail("document nested too deeply");
        n.kind = Node::Kind::array;
        ++p_;  // '['
        skip();
        if (peek() != ']')
        {
            for (;;)
            {
                skip();
                value(n.items.emplace_back(), depth);
                skip();
                if (peek() == ',')
                {
                    ++p_;
                    continue;
                }
                break;
            }
        }
        expect(']', "expected ',' or ']'");
    }

public:
    explicit Reader(std::string_view s) : s_(s)
    {
    }

    Node
    parse()
    {
        Node root;
        skip();
        if (p_ >= s_.size() || s_[p_] != '{')
            fail("document must be an object");
        object(root, 1);
        skip();
        if (p_ != s_.size())
            fail(
                s_[p_] == '/' ? "comments are not allowed"
                              : "trailing data after document");
        return root;
    }
};

Node
parse(std::string_view raw)
{
    if (raw.size() > jsontx_max_text)
        fail("document too large");
    return Reader(raw).parse();
}

//------------------------------------------------------------------------------
// Canonicalization, directed by field type.
//
// Keys of a transaction or inner object are SField names, matched
// case-insensitively and re-emitted in their canonical spelling, ordered by
// field code. A field's type decides what its value may be and how it is
// written. The handful of non-field objects a transaction renders (an issued
// amount, an Issue, a path step) have their keys sorted bytewise and hold
// only scalars.
//------------------------------------------------------------------------------

void
quoted(std::string& o, std::string_view s)
{
    o += '"';
    o += s;
    o += '"';
}

std::uint64_t
unsignedValue(Node const& v, SField const& f)
{
    std::uint64_t n = 0;
    auto const& t = v.text;
    auto const r = std::from_chars(t.data(), t.data() + t.size(), n);
    if (t.front() == '-' || r.ec != std::errc{} || r.ptr != t.data() + t.size())
        fail("'" + f.fieldName + "' is out of range");
    return n;
}

char const*
kindName(Node::Kind k)
{
    switch (k)
    {
        case Node::Kind::object:
            return "an object";
        case Node::Kind::array:
            return "an array";
        case Node::Kind::string:
            return "a string";
        case Node::Kind::number:
            return "a number";
    }
    return "?";  // LCOV_EXCL_LINE
}

void
require(Node const& v, Node::Kind k, std::string const& what)
{
    if (v.kind != k)
        fail(what + " must be " + kindName(k) + ", not " + kindName(v.kind));
}

void
emitFields(Node const& n, std::string& o, bool root = false);

// A string or a number, where the ledger wants a string: numbers become the
// quoted spelling of their exact digits.
void
emitScalar(Node const& v, std::string& o, std::string const& what)
{
    if (v.kind != Node::Kind::string && v.kind != Node::Kind::number)
        fail(what + " must be a string or a number");
    quoted(o, v.text);
}

// {"currency":..,"issuer":..,"value":..} and the like
void
emitPlain(Node const& n, std::string const& what, std::string& o)
{
    require(n, Node::Kind::object, what);
    std::vector<std::size_t> idx(n.keys.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&n](auto a, auto b) {
        return n.keys[a] < n.keys[b];
    });
    o += '{';
    for (auto const i : idx)
    {
        if (o.back() != '{')
            o += ',';
        quoted(o, n.keys[i]);
        o += ':';
        emitScalar(n.items[i], o, what + "." + n.keys[i]);
    }
    o += '}';
}

void
emitField(SField const& f, Node const& v, std::string& o)
{
    std::string const& name = f.fieldName;

    switch (f.fieldType)
    {
        case STI_OBJECT:
        case STI_XCHAIN_BRIDGE:  // renders its members under field names
            require(v, Node::Kind::object, "'" + name + "'");
            emitFields(v, o);
            return;

        case STI_ARRAY:
            require(v, Node::Kind::array, "'" + name + "'");
            o += '[';
            for (auto const& e : v.items)
            {
                if (o.back() != '[')
                    o += ',';
                // each element is {"InnerFieldName": {...}}
                require(e, Node::Kind::object, "an element of '" + name + "'");
                if (e.keys.size() != 1)
                    fail(
                        "an element of '" + name +
                        "' must have exactly one member");
                emitFields(e, o);
            }
            o += ']';
            return;

        case STI_PATHSET:
            require(v, Node::Kind::array, "'" + name + "'");
            o += '[';
            for (auto const& path : v.items)
            {
                if (o.back() != '[')
                    o += ',';
                require(path, Node::Kind::array, "a path in '" + name + "'");
                o += '[';
                for (auto const& step : path.items)
                {
                    if (o.back() != '[')
                        o += ',';
                    emitPlain(step, "a path step in '" + name + "'", o);
                }
                o += ']';
            }
            o += ']';
            return;

        case STI_VECTOR256:
            require(v, Node::Kind::array, "'" + name + "'");
            o += '[';
            for (auto const& e : v.items)
            {
                if (o.back() != '[')
                    o += ',';
                require(e, Node::Kind::string, "an element of '" + name + "'");
                quoted(o, e.text);
            }
            o += ']';
            return;

        case STI_AMOUNT:
        case STI_ISSUE:
            if (v.kind == Node::Kind::object)
                emitPlain(v, "'" + name + "'", o);
            else
                emitScalar(v, o, "'" + name + "'");
            return;

        case STI_UINT8:
        case STI_UINT16:
        case STI_UINT32:
            if (v.kind == Node::Kind::string)  // e.g. TransactionType
            {
                quoted(o, v.text);
                return;
            }
            require(v, Node::Kind::number, "'" + name + "'");
            {
                std::uint64_t const max = f.fieldType == STI_UINT8 ? 0xFFu
                    : f.fieldType == STI_UINT16                    ? 0xFFFFu
                                                : 0xFFFF'FFFFu;
                if (unsignedValue(v, f) > max)
                    fail("'" + name + "' is out of range");
            }
            o += v.text;  // already minimal: the parser refuses leading zeros
            return;

        case STI_UINT64:
            if (f == sfTime)
            {
                // Spelled Date().toISOString() in the preimage and stored as
                // milliseconds. Re-emitting the round-tripped spelling rather
                // than the input is what makes this a fixed point.
                require(v, Node::Kind::string, "'" + name + "'");
                quoted(o, jsontx_iso_str(jsontx_iso(v.text)));
                return;
            }
            if (v.kind == Node::Kind::string)
            {
                quoted(o, v.text);
                return;
            }
            require(v, Node::Kind::number, "'" + name + "'");
            quoted(o, jsontx_u64_str(f, unsignedValue(v, f)));
            return;

        default:  // hashes, blobs, accounts, currencies, numbers
            emitScalar(v, o, "'" + name + "'");
            return;
    }
}

void
emitFields(Node const& n, std::string& o, bool root)
{
    std::vector<std::pair<SField const*, Node const*>> ks;
    ks.reserve(n.keys.size() + 1);
    for (std::size_t i = 0; i < n.keys.size(); ++i)
    {
        auto const& f = jsontx_field(n.keys[i]);
        if (f == sfInvalid)
            fail("unknown field '" + n.keys[i] + "'");
        ks.emplace_back(&f, &n.items[i]);
    }

    // An omitted Sequence is Sequence 0 when something else sequences the
    // transaction: a Time (time-sequenced) or a TicketSequence. Both need
    // Sequence 0, and neither can mean anything else by it. The canonical
    // form always carries the field, because the node's own rendering does:
    // getJson emits every required field. Written this way the two spellings
    // of such a preimage - with and without "Sequence": 0 - canonicalize to
    // the same bytes, and the delta simply skips the ones the signer left
    // out.
    //
    // Only a constant may be implied here, never anything read from the
    // ledger such as the account's next sequence: the transaction must be a
    // pure function of the preimage, or the binding check means nothing.
    //
    // With neither field, nothing is implied: an absent Sequence is left
    // absent, and the submit RPC refuses the document for want of one.
    if (root)
    {
        auto const has = [&ks](SField const& f) {
            return std::any_of(ks.begin(), ks.end(), [&f](auto const& k) {
                return *k.first == f;
            });
        };
        if (!has(sfSequence) && (has(sfTime) || has(sfTicketSequence)))
        {
            static Node const zero{Node::Kind::number, "0", {}, {}};
            ks.emplace_back(&sfSequence, &zero);
        }
    }

    // field codes are unique, so this order is total
    std::sort(ks.begin(), ks.end(), [](auto const& a, auto const& b) {
        return a.first->fieldCode < b.first->fieldCode;
    });

    o += '{';
    for (std::size_t j = 0; j < ks.size(); ++j)
    {
        auto const& [f, v] = ks[j];
        if (j && f == ks[j - 1].first)  // e.g. "Fee" and "fee"
            fail("duplicate field '" + f->fieldName + "'");
        if (o.back() != '{')
            o += ',';
        quoted(o, f->fieldName);
        o += ':';
        emitField(*f, *v, o);
    }
    o += '}';
}

std::string
canonical(Node const& root)
{
    std::string out;
    emitFields(root, out, true);
    // Usually far smaller than the input, but a bare number in a field the
    // ledger wants as a string gains two quote characters, so a document of
    // bare amounts can grow past the cap. unsanitize_jsontx refuses a
    // canonical form that large, so refuse it here too.
    if (out.size() > jsontx_max_text)
        fail("canonical form too large");
    return out;
}

void
varint(std::string& o, std::uint64_t v)
{
    do
    {
        std::uint8_t const c = v & 0x7F;
        v >>= 7;
        o += static_cast<char>(c | (v ? 0x80 : 0));
    } while (v);
}

}  // namespace

//------------------------------------------------------------------------------

std::string
jsontx_signing_data(std::string_view preimage)
{
    std::string r;
    r.reserve(jsontx_sign_prefix.size() + preimage.size());
    r += jsontx_sign_prefix;
    r += preimage;
    return r;
}

// Not boost::algorithm::to_lower_copy or std::tolower: both consult the global
// locale, under which a byte such as 0xC9 folds on one node and not another.
std::string
jsontx_lower(std::string_view s)
{
    std::string r(s);
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return r;
}

SField const&
jsontx_field(std::string const& name)
{
    static auto const tbl = [] {
        std::unordered_map<std::string, SField const*> m;
        for (auto const& [code, f] : SField::knownCodeToField)
            if (f->isUseful() && f->isBinary() && f->fieldType < 10000 &&
                !f->fieldName.empty())
            {
                // Two fields folding to one key would make the lookup depend
                // on iteration order. None do today; keep it that way.
                if (!m.emplace(jsontx_lower(f->fieldName), f).second)
                    LogicError(
                        "jsontx: field names collide case-insensitively: " +
                        f->fieldName);
            }
        return m;
    }();

    auto const i = tbl.find(jsontx_lower(name));
    return i == tbl.end() ? sfInvalid : *i->second;
}

std::uint64_t
jsontx_iso(std::string_view s)
{
    static constexpr char pat[] = "0000-00-00T00:00:00.000Z";
    if (s.size() != 24)
        fail("Time must be an ISO 8601 instant");
    for (std::size_t i = 0; i < 24; ++i)
        if (pat[i] == '0' ? !digit(s[i]) : s[i] != pat[i])
            fail("malformed Time");

    auto const n = [&s](std::size_t i, std::size_t c) {
        int v = 0;
        while (c--)
            v = v * 10 + (s[i++] - '0');
        return v;
    };
    int const y = n(0, 4), mo = n(5, 2), d = n(8, 2), h = n(11, 2),
              mi = n(14, 2), se = n(17, 2), ms = n(20, 3);
    if (mo < 1 || mo > 12)
        fail("Time month out of range");
    bool const leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int const dim =
        mo == 2 ? (leap ? 29 : 28) : ((mo % 2 == 1) == (mo <= 7) ? 31 : 30);
    // 60 is refused: JS cannot emit a leap second and the ledger cannot
    // represent one
    if (d < 1 || d > dim || h > 23 || mi > 59 || se > 59)
        fail("Time out of range");

    std::int64_t const t = (jsontx_days(y, mo, d) - jsontx_epoch_day) * 86400 +
        h * 3600 + mi * 60 + se;
    if (t < 0)
        fail("Time precedes the ripple epoch");
    return static_cast<std::uint64_t>(t) * 1000 + ms;
}

std::string
jsontx_iso_str(std::uint64_t ms)
{
    if (ms > jsontx_max_time)
        fail("Time out of range");
    std::int64_t const z =
        static_cast<std::int64_t>(ms / 86400000) + jsontx_epoch_day + 719468;
    unsigned const tod = static_cast<unsigned>(ms / 1000 % 86400);
    std::int64_t const era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned const doe = static_cast<unsigned>(z - era * 146097);
    unsigned const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned const mp = (5 * doy + 2) / 153;
    unsigned const d = doy - (153 * mp + 2) / 5 + 1;
    unsigned const m = mp + (mp < 10 ? 3 : -9);
    std::int64_t const y =
        static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2);

    // jsontx_max_time bounds y to [2000, 9999], so every field fits its
    // width. Hand-rolled: this spelling is part of the signed preimage and a
    // library's padding rules are not.
    char buf[24];
    auto const pad = [&buf](std::size_t at, std::uint64_t v, std::size_t w) {
        while (w--)
        {
            buf[at + w] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
    };
    pad(0, static_cast<std::uint64_t>(y), 4);
    buf[4] = '-';
    pad(5, m, 2);
    buf[7] = '-';
    pad(8, d, 2);
    buf[10] = 'T';
    pad(11, tod / 3600, 2);
    buf[13] = ':';
    pad(14, tod / 60 % 60, 2);
    buf[16] = ':';
    pad(17, tod % 60, 2);
    buf[19] = '.';
    pad(20, ms % 1000, 3);
    buf[23] = 'Z';
    return std::string(buf, sizeof(buf));
}

std::string
jsontx_u64_str(SField const& f, std::uint64_t v)
{
    char buf[20];
    auto const r = std::to_chars(
        buf, buf + sizeof(buf), v, f.shouldMeta(SField::sMD_BaseTen) ? 10 : 16);
    return std::string(buf, r.ptr);
}

void
jsontx_strict(std::string_view raw)
{
    (void)parse(raw);
}

std::string
jsontx_canonical(std::string_view raw)
{
    return canonical(parse(raw));
}

std::pair<std::string, std::string>
sanitize_jsontx(std::string_view raw)
{
    std::string out = canonical(parse(raw));

    // Greedy copy/insert delta over `raw`, sourcing from `out`.
    //   op 0x00 <varint len> <bytes>       literal
    //   op 0x01 <varint off> <varint len>  copy from canonical
    //
    // This encoder is normative - unsanitize_jsontx only accepts its exact
    // output - so it must emit identical bytes on every node and stdlib.
    // Candidates for a 4-gram are tried in ascending offset order and ties
    // keep the lowest offset. The index is a sorted vector of
    // (gram << 32 | offset): offsets are unique, so the order is total and
    // std::sort is deterministic.
    std::string diff, lit;
    std::size_t ops = 0;

    auto const gram = [](std::string_view s, std::size_t i) {
        return std::uint32_t(std::uint8_t(s[i])) << 24 |
            std::uint32_t(std::uint8_t(s[i + 1])) << 16 |
            std::uint32_t(std::uint8_t(s[i + 2])) << 8 |
            std::uint32_t(std::uint8_t(s[i + 3]));
    };
    auto const flush = [&] {
        if (lit.empty())
            return;
        diff += char(0);
        varint(diff, lit.size());
        diff += lit;
        lit.clear();
        ++ops;
    };

    std::vector<std::uint64_t> idx;
    if (out.size() >= jsontx_min_copy)
    {
        idx.reserve(out.size() - jsontx_min_copy + 1);
        for (std::size_t i = 0; i + jsontx_min_copy <= out.size(); ++i)
            idx.push_back(std::uint64_t(gram(out, i)) << 32 | i);
        std::sort(idx.begin(), idx.end());
    }

    for (std::size_t i = 0; i < raw.size();)
    {
        std::size_t bo = 0, bl = 0;
        if (i + jsontx_min_copy <= raw.size())
        {
            std::uint64_t const g = gram(raw, i);
            auto it = std::lower_bound(idx.begin(), idx.end(), g << 32);
            for (std::size_t tried = 0;
                 it != idx.end() && (*it >> 32) == g && tried < jsontx_max_cand;
                 ++it, ++tried)
            {
                std::size_t const off = *it & 0xFFFF'FFFFu;
                std::size_t l = 0;
                while (i + l < raw.size() && off + l < out.size() &&
                       out[off + l] == raw[i + l])
                    ++l;
                if (l > bl)
                    bl = l, bo = off;
            }
        }
        if (bl >= jsontx_min_copy)
        {
            flush();
            diff += char(1);
            varint(diff, bo);
            varint(diff, bl);
            ++ops;
            i += bl;
        }
        else
            lit += raw[i++];
    }
    flush();

    // The bounds unsanitize_jsontx applies, applied here so that a document
    // this accepts is never one the verifier then refuses.
    if (diff.size() > jsontx_max_diff || ops > jsontx_max_ops)
        fail("formatting differs too much from canonical form");

    return {std::move(out), std::move(diff)};
}

// Copies read only from `sanitized`, never from the output being built, so a
// short delta cannot expand geometrically. Offsets and lengths are range
// checked before use, varints are length- and minimality-bounded, and the two
// encodings the encoder can never emit - an unmerged literal run, and a copy
// abutting the previous copy in the source - are refused.
std::string
unsanitize_jsontx(std::string_view sanitized, std::string_view diff)
{
    if (sanitized.size() > jsontx_max_text || diff.size() > jsontx_max_diff)
        fail("oversize delta input");

    std::string out;
    std::size_t p = 0, ops = 0, prevEnd = 0;
    int prev = -1;

    auto const read = [&](std::uint64_t max) -> std::uint64_t {
        std::uint64_t v = 0;
        for (int s = 0; s <= 21; s += 7)  // four bytes at most
        {
            if (p >= diff.size())
                fail("truncated delta");
            std::uint8_t const c = diff[p++];
            v |= std::uint64_t(c & 0x7F) << s;
            if (c & 0x80)
                continue;
            if (s && !(c & 0x7F))
                fail("non-minimal varint");
            if (v > max)
                fail("delta value out of range");
            return v;
        }
        fail("overlong varint");
    };

    while (p < diff.size())
    {
        if (++ops > jsontx_max_ops)
            fail("too many delta ops");

        std::uint8_t const op = diff[p++];
        if (op > 1)
            fail("unknown delta op");

        if (op == 0)  // literal
        {
            if (prev == 0)
                fail("unmerged literal run");
            auto const n = read(jsontx_max_text);
            if (n == 0 || n > diff.size() - p)
                fail("bad literal length");
            if (out.size() + n > jsontx_max_text)
                fail("delta expands too far");
            out += diff.substr(p, n);
            p += n;
        }
        else  // copy from the canonical form
        {
            auto const off = read(sanitized.size());
            auto const n = read(sanitized.size() - off);
            if (n < jsontx_min_copy)
                fail("undersize copy");
            if (prev == 1 && off == prevEnd)
                fail("unmerged copy run");
            if (out.size() + n > jsontx_max_text)
                fail("delta expands too far");
            out += sanitized.substr(off, n);
            prevEnd = off + n;
        }
        prev = op;
    }

    if (out.empty())
        fail("empty delta");
    return out;
}

std::string
jsontx_verify(STTx const& stx, std::string_view diff)
{
    // The cheap refusals first: this runs on every relay of a transaction
    // carrying a delta, before any signature is known to be good.
    if (!stx.isFieldPresent(sfTxnSignature) || stx.isFieldPresent(sfSigners))
        fail("expects a lone TxnSignature");

    auto const pkb = stx.getSigningPubKey();
    if (publicKeyType(makeSlice(pkb)) != KeyType::ed25519)
        fail("SigningPubKey must be ed25519");

    if (diff.size() > jsontx_max_diff)
        fail("oversize delta");

    // No transaction serializes to more than twice its canonical text
    // (JSONTxSignatures_test checks every field of every format; the worst
    // is a bare three-letter currency code, about 1.3x). Allowing twice that
    // again, plus the delta and signature this counts but the canonical form
    // does not, loses nothing that could verify, and spares getJson on an
    // arbitrarily large object.
    if (stx.getSerializer().size() > 4 * jsontx_max_text + jsontx_max_diff)
        fail("transaction too large");

    // Out comes everything the signer did not have in front of them: the
    // signature and the delta carrier. SigningPubKey stays; it being inside
    // the preimage binds key to signature and stops a third party re-signing
    // a captured preimage under their own key.
    auto txj = stx.STObject::getJson(JsonOptions::none);
    txj.removeMember(sfTxnSignature.fieldName);
    txj.removeMember(sfJsonTxDelta.fieldName);

    // sfTime is milliseconds on the wire and an ISO 8601 instant in the
    // preimage: a bijection over the representable range, so the delta
    // carries nothing for the field.
    if (stx.isFieldPresent(sfTime))
        txj[sfTime.fieldName] = jsontx_iso_str(stx.getFieldU64(sfTime));

    // canonical form, derived only from data the node already holds
    auto const san = jsontx_canonical(Json::FastWriter{}.write(txj));

    // reconstruct the signed preimage under the caps above
    auto const raw = unsanitize_jsontx(san, diff);

    // The signature before the binding check purely for cost: the binding
    // check re-canonicalizes and re-encodes, and without the key an attacker
    // cannot get past this line. Both must pass.
    if (!verify(
            PublicKey(makeSlice(pkb)),
            makeSlice(jsontx_signing_data(raw)),
            makeSlice(stx.getFieldVL(sfTxnSignature))))
        fail("signature does not verify");

    // Bind the preimage to the transaction. This is the load-bearing check:
    // a delta of pure literals can reconstruct ANY text, so without it every
    // JsonTx signature the key ever produced would authorise this
    // transaction. Comparing the delta too makes it a pure function of the
    // preimage, which rules out a second delta reconstructing the same bytes
    // and yielding a second valid transaction id.
    auto const [san2, diff2] = sanitize_jsontx(raw);
    if (san2 != san || diff2 != diff)
        fail("preimage does not match transaction");

    return raw;
}

std::string
jsontx_verify(STTx const& stx)
{
    if (!stx.isFieldPresent(sfJsonTxDelta))
        fail("no delta");

    auto const delta = stx.getFieldVL(sfJsonTxDelta);
    return jsontx_verify(
        stx,
        std::string_view(
            reinterpret_cast<char const*>(delta.data()), delta.size()));
}

}  // namespace ripple
