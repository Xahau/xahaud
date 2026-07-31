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

#ifndef RIPPLE_PROTOCOL_JSONTXSIGNATURES_H_INCLUDED
#define RIPPLE_PROTOCOL_JSONTXSIGNATURES_H_INCLUDED

#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <map>
#include <functional>
#include <unordered_map>

namespace ripple {

//------------------------------------------------------------------------------
// jsontx: plaintext-JSON signing support (featureJsonTx)
//
// The delta is attacker-controlled: it arrives over the wire beside a binary
// transaction and is not covered by the signature it helps reconstruct. Every
// bound below is therefore explicit, and unsanitize_jsontx accepts only the
// exact encoding sanitize_jsontx would have produced.
//------------------------------------------------------------------------------

static constexpr std::size_t jsontx_max_text = 8192;  // canonical and original
static constexpr std::size_t jsontx_max_diff = 1024;  // delta bytes
static constexpr std::size_t jsontx_max_ops = 256;    // delta instructions
static constexpr std::size_t jsontx_min_copy = 4;     // encoder match threshold
static constexpr std::size_t jsontx_max_cand = 64;    // encoder candidate cap

// Case-insensitive field-name -> canonical SField. Built once from
// SField::knownCodeToField, the same table doServerDefinitions publishes, using
// its serializability filter (useful, binary, non-pseudo). sfInvalid if
// unknown.
static SField const&
jsontx_field(std::string const& name)
{
    static auto const tbl = [] {
        std::unordered_map<std::string, SField const*> m;
        for (auto const& [code, f] : SField::knownCodeToField)
            if (f->isUseful() && f->isBinary() && f->fieldType < 10000 &&
                !f->fieldName.empty())
                m.emplace(boost::algorithm::to_lower_copy(f->fieldName), f);
        return m;
    }();

    auto const i = tbl.find(boost::algorithm::to_lower_copy(name));
    return i == tbl.end() ? sfInvalid : *i->second;
}

// Civil calendar arithmetic (Howard Hinnant's algorithm), used in both
// directions. Pure integer maths - no strptime, no timegm, no locale, no
// tzdata - because these conversions decide whether a signature verifies and
// so must give the same answer on every node forever.
static constexpr std::int64_t
jsontx_days(int y, unsigned m, unsigned d)  // days from 1970-01-01
{
    y -= m <= 2;
    std::int64_t const era = (y >= 0 ? y : y - 399) / 400;
    unsigned const yoe = static_cast<unsigned>(y - era * 400);
    unsigned const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static constexpr std::int64_t jsontx_epoch_day = jsontx_days(2000, 1, 1);

// 9999-12-31T23:59:59.999Z: past this toISOString() switches to expanded years
// (+275760-09-13T...) and the fixed 24 character shape no longer holds
static constexpr std::uint64_t jsontx_max_time =
    (static_cast<std::uint64_t>(jsontx_days(9999, 12, 31) - jsontx_epoch_day) *
         86400 +
     86399) *
        1000 +
    999;

static_assert(jsontx_epoch_day == 10957);  // matches chrono.h epoch_offset

// Strict Date().toISOString() -> milliseconds since the ripple epoch. Exactly
// YYYY-MM-DDTHH:MM:SS.sssZ, always UTC, always three fractional digits.
static std::uint64_t
jsontx_iso(std::string const& s)
{
    static constexpr char pat[] = "0000-00-00T00:00:00.000Z";
    if (s.size() != 24)
        throw std::runtime_error("jsontx: Time must be an ISO 8601 instant");
    for (std::size_t i = 0; i < 24; ++i)
        if (pat[i] == '0' ? !std::isdigit(static_cast<unsigned char>(s[i]))
                          : s[i] != pat[i])
            throw std::runtime_error("jsontx: malformed Time");

    auto const n = [&s](std::size_t i, std::size_t c) {
        int v = 0;
        while (c--)
            v = v * 10 + (s[i++] - '0');
        return v;
    };
    int const y = n(0, 4), mo = n(5, 2), d = n(8, 2), h = n(11, 2),
              mi = n(14, 2), se = n(17, 2), ms = n(20, 3);
    if (mo < 1 || mo > 12)
        throw std::runtime_error("jsontx: Time month out of range");
    bool const leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int const dim =
        mo == 2 ? (leap ? 29 : 28) : ((mo % 2 == 1) == (mo <= 7) ? 31 : 30);
    // 60 is rejected: JS cannot emit a leap second and the ledger cannot
    // represent one
    if (d < 1 || d > dim || h > 23 || mi > 59 || se > 59)
        throw std::runtime_error("jsontx: Time out of range");

    std::int64_t const t = (jsontx_days(y, mo, d) - jsontx_epoch_day) * 86400 +
        h * 3600 + mi * 60 + se;
    if (t < 0)
        throw std::runtime_error("jsontx: Time precedes the ripple epoch");
    return static_cast<std::uint64_t>(t) * 1000 + ms;
}

// The exact inverse. Total over [0, jsontx_max_time] and injective, so sfTime
// and its ISO spelling are two views of one value and the delta carries
// nothing for the field.
static std::string
jsontx_iso_str(std::uint64_t ms)
{
    if (ms > jsontx_max_time)
        throw std::runtime_error("jsontx: Time out of range");
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
    return fmt::format(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2),
        m,
        d,
        tod / 3600,
        tod / 60 % 60,
        tod % 60,
        static_cast<unsigned>(ms % 1000));
}

//------------------------------------------------------------------------------
// jsoncpp compatibility
//
// This is xrpl's vendored jsoncpp, which predates JSON_HAS_INT64: Json::Int is
// int and Json::UInt is unsigned int, both 32 bit, and there is no isInt64 /
// asInt64 / asUInt64. Reader::decodeNumber yields intValue or uintValue only
// while the digits fit in 32 bits; on overflow, and for any token carrying a
// '.' or an exponent, it falls through to decodeDouble and the number arrives
// as a realValue.
//
// So a large integer is not lost, but it is no longer held as an integer, and
// the digits the signer wrote are recoverable only while the double is an
// exact integer view of them. That holds to 2^53; above it consecutive doubles
// are more than 1 apart and distinct decimal integers collapse onto the same
// double. Past that point the value is refused rather than guessed at.
//------------------------------------------------------------------------------

static constexpr double jsontx_exact_max = 9007199254740992.0;  // 2^53

// Exact integer view of a json number. False if v is not a number at all, or
// is one this build cannot reproduce digit for digit.
static bool
jsontx_exact(Json::Value const& v, std::int64_t& out)
{
    switch (v.type())
    {
        case Json::intValue:
            out = v.asInt();
            return true;
        case Json::uintValue:
            out = static_cast<std::int64_t>(v.asUInt());
            return true;
        case Json::realValue:
            break;
        default:  // including booleanValue, which isIntegral() would admit
            return false;
    }
    double const d = v.asDouble();
    if (!std::isfinite(d) || d != std::trunc(d) || d < -jsontx_exact_max ||
        d > jsontx_exact_max)
        return false;
    out = static_cast<std::int64_t>(d);
    return true;
}

// STI_UINT64 renders as a fixed 16 digit hex string and so needs the value
// unsigned. A negative is refused rather than wrapped: the old
// static_cast<std::uint64_t>(v.asDouble()) was undefined for a negative or
// oversized double, and a silent wrap would let -1 and 18446744073709551615
// canonicalize to the same bytes.
static std::uint64_t
jsontx_u64(Json::Value const& v)
{
    std::int64_t n = 0;
    if (!jsontx_exact(v, n) || n < 0)
        throw std::runtime_error(
            "jsontx: UInt64 must be an exact non-negative integer");
    return static_cast<std::uint64_t>(n);
}

// Renders a javascript number as an exact integer. Anything this build cannot
// reproduce digit for digit - a fraction, an infinity, a magnitude past 2^53 -
// is rejected outright: shortest-round-trip rendering of a double is not
// portable enough to sit in a consensus preimage, and nothing in a transaction
// needs one. Fractional amounts arrive as strings, which is what the ledger
// wants anyway.
static std::string
jsontx_num(Json::Value const& v)
{
    std::int64_t n = 0;
    if (!jsontx_exact(v, n))
        throw std::runtime_error("jsontx: number must be an exact integer");
    return std::to_string(n);
}

// Returns { sanitized, diff }. `sanitized` is the canonical form: whitespace
// stripped, field names capitalized to their xahau spelling, members reordered
// by field code, numbers reformatted per field type. `diff` is a binary delta
// which, applied to `sanitized` by unsanitize_jsontx, reproduces `raw` byte for
// byte. Throws on anything it cannot canonicalize.
static std::pair<std::string, std::string>
sanitize_jsontx(std::string_view raw)
{
    if (raw.size() > jsontx_max_text)
        throw std::runtime_error("jsontx: document too large");

    Json::Value jv;
    if (Json::Reader r; !r.parse(raw.data(), raw.data() + raw.size(), jv) ||
        !jv.isObject())
        throw std::runtime_error("jsontx: malformed json");

    // (a plain recursive lambda; deducing-this would drop the std::function)
    std::function<
        void(Json::Value const&, SerializedTypeID, SField const*, std::string&)>
        emit = [&](Json::Value const& v,
                   SerializedTypeID ty,
                   SField const* fld,
                   std::string& o) {
            if (v.isObject())
            {
                auto keys = v.getMemberNames();
                o += '{';
                if (ty == STI_OBJECT)  // keys are xahau fields
                {
                    std::vector<std::pair<SField const*, std::string>> ks;
                    for (auto const& k : keys)
                    {
                        auto const& f = jsontx_field(k);
                        if (f == sfInvalid)
                            throw std::runtime_error(
                                "jsontx: unknown field '" + k + "'");
                        ks.emplace_back(&f, k);
                    }
                    std::sort(
                        ks.begin(), ks.end(), [](auto const& a, auto const& b) {
                            return a.first->fieldCode < b.first->fieldCode;
                        });
                    for (std::size_t n = 0; n < ks.size(); ++n)
                    {
                        auto const& [f, k] = ks[n];
                        if (n && f == ks[n - 1].first)  // e.g. "Fee" and "fee"
                            throw std::runtime_error(
                                "jsontx: duplicate field '" + f->fieldName +
                                "'");
                        if (o.back() != '{')
                            o += ',';
                        o += Json::valueToQuotedString(f->fieldName.c_str()) +
                            ':';
                        emit(v[k], f->fieldType, f, o);
                    }
                }
                else  // amount / issue style subobject: lexicographic, quoted
                {
                    std::sort(keys.begin(), keys.end());
                    for (auto const& k : keys)
                    {
                        if (o.back() != '{')
                            o += ',';
                        o += Json::valueToQuotedString(k.c_str()) + ':';
                        emit(v[k], STI_NOTPRESENT, nullptr, o);
                    }
                }
                o += '}';
            }
            else if (v.isArray())
            {
                o += '[';
                for (auto const& e : v)
                {
                    if (o.back() != '[')
                        o += ',';
                    emit(e, STI_OBJECT, nullptr, o);
                }
                o += ']';
            }
            else if (v.isString())
            {
                // Time is spelled Date().toISOString() in the preimage and
                // stored as an sfTime u64 of milliseconds. Re-emitting the
                // round-tripped spelling rather than the input is what makes
                // the canonical form a fixed point: any string that is not
                // exactly what jsontx_iso_str produces is rejected here.
                if (fld && *fld == sfTime)
                    o += Json::valueToQuotedString(
                        jsontx_iso_str(jsontx_iso(v.asString())).c_str());
                else
                    o += Json::valueToQuotedString(v.asCString());
            }
            else if (v.isBool())
                o += v.asBool() ? "true" : "false";
            else if (v.isNull())
                throw std::runtime_error("jsontx: null value");
            else if (ty == STI_UINT8 || ty == STI_UINT16 || ty == STI_UINT32)
                o += jsontx_num(v);  // small ints stay bare
            else if (ty == STI_UINT64)
                o += '"' + fmt::format("{:016X}", jsontx_u64(v)) + '"';
            else  // amounts, u64, everything else the ledger wants as a string
                o += Json::valueToQuotedString(jsontx_num(v).c_str());
        };

    std::string out;
    emit(jv, STI_OBJECT, nullptr, out);

    // Greedy copy/insert delta over `raw`, sourcing from `out`.
    //   op 0x00 <varint len> <bytes>       literal
    //   op 0x01 <varint off> <varint len>  copy from sanitized
    std::string diff, lit;
    auto const gram = [](std::string_view s, std::size_t i) {
        return std::uint32_t(std::uint8_t(s[i])) << 24 |
            std::uint32_t(std::uint8_t(s[i + 1])) << 16 |
            std::uint32_t(std::uint8_t(s[i + 2])) << 8 |
            std::uint32_t(std::uint8_t(s[i + 3]));
    };
    auto const varint = [](std::string& o, std::uint64_t v) {
        do
        {
            std::uint8_t const c = v & 0x7F;
            v >>= 7;
            o += static_cast<char>(c | (v ? 0x80 : 0));
        } while (v);
    };
    auto const flush = [&] {
        if (lit.empty())
            return;
        diff += char(0);
        varint(diff, lit.size());
        diff += lit;
        lit.clear();
    };

    // This encoder is normative - unsanitize_jsontx only accepts its exact
    // output - so it must emit identical bytes on every node and every stdlib.
    // An unordered container would not: bucket order for equal keys is
    // unspecified, and with a candidate cap that changes which match wins.
    std::map<std::uint32_t, std::vector<std::uint32_t>> idx;
    for (std::size_t i = 0; i + jsontx_min_copy <= out.size(); ++i)
        idx[gram(out, i)].push_back(i);

    for (std::size_t i = 0; i < raw.size();)
    {
        std::size_t bo = 0, bl = 0;
        if (i + jsontx_min_copy <= raw.size())
            if (auto const it = idx.find(gram(raw, i)); it != idx.end())
            {
                std::size_t tried = 0;
                for (auto const off : it->second)  // ascending, so ties keep
                {                                  // the lowest offset
                    if (++tried > jsontx_max_cand)
                        break;
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
            i += bl;
        }
        else
            lit += raw[i++];
    }
    flush();

    return {std::move(out), std::move(diff)};
}

// Applies an UNTRUSTED delta to a canonical form the node derived itself.
// Copies read only from `sanitized`, never from the output being built, so a
// short delta cannot expand geometrically. Offsets and lengths are range
// checked before use, varints are length- and minimality-bounded, and the two
// encodings the encoder can never emit - an unmerged literal run, and a copy
// abutting the previous copy in the source - are rejected. Throws on anything
// else.
static std::string
unsanitize_jsontx(std::string_view sanitized, std::string_view diff)
{
    if (sanitized.size() > jsontx_max_text || diff.size() > jsontx_max_diff)
        throw std::runtime_error("jsontx: oversize delta input");

    std::string out;
    std::size_t p = 0, ops = 0, prevEnd = 0;
    int prev = -1;

    auto const varint = [&](std::uint64_t max) -> std::uint64_t {
        std::uint64_t v = 0;
        for (int s = 0; s <= 21; s += 7)  // four bytes; caps far under a shift
        {                                 // wide enough to be undefined
            if (p >= diff.size())
                throw std::runtime_error("jsontx: truncated delta");
            std::uint8_t const c = diff[p++];
            v |= std::uint64_t(c & 0x7F) << s;
            if (c & 0x80)
                continue;
            if (s && !(c & 0x7F))
                throw std::runtime_error("jsontx: non-minimal varint");
            if (v > max)
                throw std::runtime_error("jsontx: delta value out of range");
            return v;
        }
        throw std::runtime_error("jsontx: overlong varint");
    };

    while (p < diff.size())
    {
        if (++ops > jsontx_max_ops)
            throw std::runtime_error("jsontx: too many delta ops");

        std::uint8_t const op = diff[p++];
        if (op > 1)
            throw std::runtime_error("jsontx: unknown delta op");

        std::size_t n = 0;
        if (op == 0)  // literal
        {
            if (prev == 0)
                throw std::runtime_error("jsontx: unmerged literal run");
            n = varint(jsontx_max_text);
            if (n == 0 || n > diff.size() - p)
                throw std::runtime_error("jsontx: bad literal length");
            if (out.size() + n > jsontx_max_text)
                throw std::runtime_error("jsontx: delta expands too far");
            out += diff.substr(p, n);
            p += n;
        }
        else  // copy from the canonical form
        {
            auto const off = varint(sanitized.size());
            n = varint(sanitized.size() - off);
            if (n < jsontx_min_copy)
                throw std::runtime_error("jsontx: undersize copy");
            if (prev == 1 && off == prevEnd)
                throw std::runtime_error("jsontx: unmerged copy run");
            if (out.size() + n > jsontx_max_text)
                throw std::runtime_error("jsontx: delta expands too far");
            out += sanitized.substr(off, n);
            prevEnd = off + n;
        }
        prev = op;
    }

    if (out.empty())
        throw std::runtime_error("jsontx: empty delta");
    return out;
}

// The complete untrusted-side check, in one place so the RPC path and the
// relay/consensus path cannot drift. Takes the transaction exactly as it came
// off the wire and returns the reconstructed preimage.
static std::string
jsontx_verify(STTx const& stx, std::string_view diff)
{
    if (!stx.isFieldPresent(sfTxnSignature) || stx.isFieldPresent(sfSigners))
        throw std::runtime_error("jsontx: expects a lone TxnSignature");

    // Out comes everything the signer did not have in front of them: the
    // signature, and (once the field exists) the delta carrier. SigningPubKey
    // stays. It being inside the preimage is what binds key to signature and
    // stops a third party re-signing a captured preimage under their own key.
    auto txj = stx.STObject::getJson(JsonOptions::none);
    txj.removeMember(sfTxnSignature.fieldName);

    // sfTime is a u64 of milliseconds on the wire and an ISO 8601 instant in
    // the preimage. The two are a bijection over the representable range, so
    // this is a rewrite rather than a reconstruction and the delta carries
    // nothing for the field.
    if (stx.isFieldPresent(sfTime))
        txj[sfTime.fieldName] = jsontx_iso_str(stx.getFieldU64(sfTime));

    auto const pkb = stx.getSigningPubKey();
    if (publicKeyType(makeSlice(pkb)) != KeyType::ed25519)
        throw std::runtime_error("jsontx: SigningPubKey must be ed25519");

    // canonical form, derived only from data the node has already validated
    auto const san = sanitize_jsontx(Json::FastWriter{}.write(txj)).first;

    // reconstruct the signed preimage under the caps above
    auto const raw = unsanitize_jsontx(san, diff);

    // Bind the preimage to the transaction. This is the load-bearing check,
    // not a sanity check: a delta of pure literals can reconstruct ANY text,
    // so without it any ed25519 signature the key ever produced over anything
    // at all would authorise this transaction. Comparing the delta too - not
    // just the canonical form - makes the delta a pure function of the
    // preimage, which rules out a second delta reconstructing the same bytes
    // and yielding a second valid transaction id.
    auto const [san2, diff2] = sanitize_jsontx(raw);
    if (san2 != san || diff2 != diff)
        throw std::runtime_error("jsontx: preimage does not match transaction");

    if (!verify(
            PublicKey(makeSlice(pkb)),
            makeSlice(raw),
            makeSlice(stx.getFieldVL(sfTxnSignature))))
        throw std::runtime_error("jsontx: signature does not verify");

    return raw;
}

}  // namespace ripple
#endif
