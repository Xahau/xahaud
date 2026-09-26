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
    OR  IN  CONNECTION  WITH  THE  USE  OR  PERFORMANCE  OF  THIS  SOFTWARE.
*/
//==============================================================================

#include <xrpl/basics/strHex.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/jss.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

namespace ripple {

/**
 * Unit tests for JsonTx plaintext transaction signature support.
 *
 * Covers:
 *   - jsontx_strict:   document framing (no comments, no \u, nothing outside)
 *   - jsontx_iso / jsontx_iso_str: ISO 8601 <-> ripple-epoch milliseconds
 *   - sanitize_jsontx / unsanitize_jsontx: canonical form + delta round-trip
 *   - unsanitize_jsontx: rejection of every delta the encoder cannot emit
 *   - jsontx_exact / jsontx_num / jsontx_u64 / jsontx_u64_str: number handling
 *   - jsontx_field: case-insensitive canonical field lookup
 *
 * All of these report failure by throwing rather than by returning a status,
 * so the helpers below adapt them to the boolean BEAST_EXPECT wants.
 */
class JSONTxSignatures_test : public beast::unit_test::suite
{
    // True if f() threw. Used in place of a throws-macro so the expectation
    // still reports the failing file and line through BEAST_EXPECT.
    template <class F>
    static bool
    threw(F&& f)
    {
        try
        {
            f();
        }
        catch (std::exception const&)
        {
            return true;
        }
        return false;
    }

    static bool
    strictOk(std::string_view s)
    {
        return !threw([&] { jsontx_strict(s); });
    }

    // sanitize then unsanitize must reproduce the input byte for byte; this is
    // the property jsontx_verify rests on.
    static bool
    roundTrips(std::string const& raw)
    {
        auto const [san, diff] = sanitize_jsontx(raw);
        return unsanitize_jsontx(san, diff) == raw;
    }

    static bool
    unsanitizeThrows(std::string_view san, std::string const& diff)
    {
        return threw([&] { (void)unsanitize_jsontx(san, diff); });
    }

    void
    testStrictAccepts()
    {
        testcase("jsontx_strict accepts");

        BEAST_EXPECT(strictOk("{}"));
        BEAST_EXPECT(strictOk(R"({"key":"value"})"));
        BEAST_EXPECT(strictOk(R"({"n":0})"));
        BEAST_EXPECT(strictOk(R"({"n":12345})"));
        BEAST_EXPECT(strictOk(R"({"n":-1})"));
        BEAST_EXPECT(strictOk(R"({"outer":{"inner":true}})"));
        BEAST_EXPECT(strictOk(R"({"a":[1,2,3]})"));
        BEAST_EXPECT(strictOk(R"({"b":true})"));
        BEAST_EXPECT(strictOk(R"({"b":false})"));
        BEAST_EXPECT(strictOk(R"({"n":null})"));

        // json's whitespace set, between tokens and around the document
        BEAST_EXPECT(strictOk(" { \"key\" : \"val\" } "));
        BEAST_EXPECT(strictOk("{\r\n\t\"key\":\"val\"\r\n}"));

        // an escaped backslash ends the escape, so the 'u' after it is an
        // ordinary character and not a \u escape
        BEAST_EXPECT(strictOk(R"({"key":"\\utest"})"));

        // a solidus that does not open a comment is the parser's business
        BEAST_EXPECT(strictOk(R"({"key":"a/b"})"));

        // plain integers, including at the edges of the grammar
        BEAST_EXPECT(strictOk(R"({"n":[0,-1,10,9007199254740991]})"));

        // This validates framing and lexical form only. Trailing commas,
        // single quotes and unquoted keys are all rejected too, but by the
        // parser or by sanitize_jsontx rather than here.
        BEAST_EXPECT(strictOk(R"({"key":"val",})"));
        BEAST_EXPECT(strictOk(R"({'key':'val'})"));
        BEAST_EXPECT(strictOk(R"({key:"val"})"));
    }

    void
    testStrictRejects()
    {
        testcase("jsontx_strict rejects");

        // comments outside the document, in either style
        BEAST_EXPECT(!strictOk("{\"key\":\"value\"}\n// real comment"));
        BEAST_EXPECT(!strictOk("{\"key\":\"value\"} /* real comment */"));
        BEAST_EXPECT(!strictOk("// leading comment\n{\"key\":\"value\"}"));

        // ...but a comment marker inside a string is just text
        BEAST_EXPECT(strictOk(R"({"key":"value // not a comment"})"));

        // \u escapes never appear in a preimage: the signer has to be able to
        // read what they are signing
        BEAST_EXPECT(!strictOk(R"({"key":"\u0048ello"})"));
        BEAST_EXPECT(!strictOk(R"({"key":"hello\u0041"})"));
        BEAST_EXPECT(!strictOk(R"({"key":"\u0000"})"));

        // nothing may ride along after the document
        BEAST_EXPECT(!strictOk(R"({"a":"b"} "trailing")"));
        BEAST_EXPECT(!strictOk(R"({"a":"b"} {"c":"d"})"));

        // unbalanced brackets
        BEAST_EXPECT(!strictOk(R"({"a":"b")"));
        BEAST_EXPECT(!strictOk(R"({"a":"b"}})"));

        // unterminated string
        BEAST_EXPECT(!strictOk(R"({"a":"b})"));

        // a bare scalar is not a bracketed document
        BEAST_EXPECT(!strictOk(R"("just a string")"));
        BEAST_EXPECT(!strictOk("42"));
        BEAST_EXPECT(!strictOk("true"));
        BEAST_EXPECT(!strictOk("null"));

        BEAST_EXPECT(!strictOk(""));
        BEAST_EXPECT(!strictOk("   "));

        // the root is an object and nothing but whitespace comes before it
        BEAST_EXPECT(!strictOk(R"([1,2,3])"));
        BEAST_EXPECT(!strictOk(R"(x{"a":"b"})"));
        BEAST_EXPECT(!strictOk(R"("lead" {"a":"b"})"));
        BEAST_EXPECT(!strictOk("\xEF\xBB\xBF{}"));  // byte order mark

        // a number is spelled exactly as its value: no fraction, no exponent,
        // no sign but a leading '-', no leading zero, no negative zero
        BEAST_EXPECT(!strictOk(R"({"n":1.0})"));
        BEAST_EXPECT(!strictOk(R"({"n":1e6})"));
        BEAST_EXPECT(!strictOk(R"({"n":1E6})"));
        BEAST_EXPECT(!strictOk(R"({"n":4503599627370497.5})"));
        BEAST_EXPECT(!strictOk(R"({"n":012})"));
        BEAST_EXPECT(!strictOk(R"({"n":-0})"));
        BEAST_EXPECT(!strictOk(R"({"n":+1})"));
        BEAST_EXPECT(!strictOk(R"({"n":-})"));
        BEAST_EXPECT(!strictOk(R"({"n":--1})"));
        BEAST_EXPECT(!strictOk(R"({"n":[1,2.5]})"));

        // ...but digits inside a string are just text
        BEAST_EXPECT(strictOk(R"({"n":"1.5e3"})"));

        // raw control characters inside a string
        BEAST_EXPECT(!strictOk("{\"a\":\"x\ty\"}"));
        BEAST_EXPECT(!strictOk("{\"a\":\"x\x1b[2Jy\"}"));
        BEAST_EXPECT(!strictOk(std::string("{\"a\":\"x\0y\"}", 11)));
    }

    void
    testIsoRoundTrip()
    {
        testcase("ISO 8601 round-trip");

        BEAST_EXPECT(jsontx_iso("2000-01-01T00:00:00.000Z") == 0);
        BEAST_EXPECT(jsontx_iso_str(0) == "2000-01-01T00:00:00.000Z");

        auto rt = [](char const* s) {
            return jsontx_iso_str(jsontx_iso(s)) == s;
        };
        BEAST_EXPECT(rt("2000-01-01T00:00:00.000Z"));
        BEAST_EXPECT(rt("2025-06-15T12:30:45.123Z"));
        BEAST_EXPECT(rt("2000-02-29T00:00:00.000Z"));  // leap year
        BEAST_EXPECT(rt("2400-02-29T00:00:00.000Z"));  // 400-year leap year
        BEAST_EXPECT(rt("9999-12-31T23:59:59.999Z"));

        // the last instant with a 24 character spelling
        BEAST_EXPECT(jsontx_iso("9999-12-31T23:59:59.999Z") == jsontx_max_time);
        BEAST_EXPECT(
            jsontx_iso_str(jsontx_max_time) == "9999-12-31T23:59:59.999Z");
        BEAST_EXPECT(threw([] { (void)jsontx_iso_str(jsontx_max_time + 1); }));

        // milliseconds are carried, not truncated
        BEAST_EXPECT(jsontx_iso("2000-01-01T00:00:00.001Z") == 1);
        BEAST_EXPECT(jsontx_iso("2000-01-02T00:00:00.000Z") == 86400000);
    }

    void
    testIsoRejects()
    {
        testcase("ISO 8601 rejection");

        auto bad = [](char const* s) {
            return threw([&] { (void)jsontx_iso(s); });
        };

        // the shape is exactly YYYY-MM-DDTHH:MM:SS.sssZ, always 24 characters
        BEAST_EXPECT(bad("2000-01-01T00:00:00"));      // no fraction, no Z
        BEAST_EXPECT(bad("2000-01-01T00:00:00.Z"));    // no fraction
        BEAST_EXPECT(bad("2000-01-01T00:00:00.000"));  // no Z
        BEAST_EXPECT(bad("2000-01-01T00:00:00.000+00:00"));  // offset
        BEAST_EXPECT(bad("2000-01-01 00:00:00.000Z"));       // space for T
        BEAST_EXPECT(bad("2000/01/01T00:00:00.000Z"));
        BEAST_EXPECT(bad("not-a-date"));
        BEAST_EXPECT(bad(""));

        // out of range components
        BEAST_EXPECT(bad("2000-00-01T00:00:00.000Z"));  // month 0
        BEAST_EXPECT(bad("2000-13-01T00:00:00.000Z"));  // month 13
        BEAST_EXPECT(bad("2000-01-00T00:00:00.000Z"));  // day 0
        BEAST_EXPECT(bad("2000-02-30T00:00:00.000Z"));  // Feb 30
        BEAST_EXPECT(bad("2001-02-29T00:00:00.000Z"));  // non-leap Feb 29
        BEAST_EXPECT(bad("1900-02-29T00:00:00.000Z"));  // century non-leap
        BEAST_EXPECT(bad("2000-01-01T24:00:00.000Z"));  // hour 24
        BEAST_EXPECT(bad("2000-01-01T00:60:00.000Z"));  // minute 60
        BEAST_EXPECT(bad("2000-01-01T00:00:60.000Z"));  // leap second

        // before the ripple epoch
        BEAST_EXPECT(bad("1999-12-31T23:59:59.999Z"));
    }

    void
    testSanitizeRoundTrip()
    {
        testcase("sanitize/unsanitize round-trip");

        // already canonical
        BEAST_EXPECT(roundTrips(
            R"({"TransactionType":"Payment","Sequence":1,"Fee":"10","SigningPubKey":"ED0000","Account":"rTest"})"));

        // source order is irrelevant: the canonical form sorts by field code
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED0000","TransactionType":"Payment"})"));

        // sfTime is an ISO instant in the preimage and a u64 on the wire
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED0000","Time":"2000-01-01T00:00:00.000Z","TransactionType":"Payment"})"));

        // interior whitespace is carried by the delta
        BEAST_EXPECT(roundTrips(R"({ "Account" : "rTest" , "Fee" : "10" })"));

        // pretty-printed, which is what a signer actually reads
        BEAST_EXPECT(
            roundTrips("{\n"
                       "    \"Account\": \"rTest\",\n"
                       "    \"Fee\": \"10\",\n"
                       "    \"Sequence\": 1,\n"
                       "    \"SigningPubKey\": \"ED\",\n"
                       "    \"Time\": \"2000-01-01T00:00:00.000Z\",\n"
                       "    \"TransactionType\": \"Payment\"\n"
                       "}"));

        // CRLF, which is what a browser textarea produces
        BEAST_EXPECT(
            roundTrips("{\r\n"
                       "  \"Account\": \"rTest\",\r\n"
                       "  \"Fee\": \"10\",\r\n"
                       "  \"TransactionType\": \"Payment\"\r\n"
                       "}"));

        // nested objects and arrays
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Memos":[{"Memo":{"MemoData":"48656C6C6F"}}],"Sequence":1,"TransactionType":"Payment"})"));

        // a u32 above the signed range stays bare and exact
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Flags":2147483648,"Sequence":1})"));

        // field names match case-insensitively and re-emit canonically
        BEAST_EXPECT(roundTrips(R"({"account":"rTest","FEE":"10"})"));
    }

    void
    testCanonicalForm()
    {
        testcase("canonical form");

        auto const compact =
            sanitize_jsontx(R"({"Account":"rTest","Fee":"10"})");
        auto const spaced =
            sanitize_jsontx(R"({ "Account" : "rTest" , "Fee" : "10" })").first;
        auto const reordered =
            sanitize_jsontx(R"({"Fee":"10","Account":"rTest"})").first;
        auto const cased =
            sanitize_jsontx(R"({"account":"rTest","fee":"10"})").first;

        // whitespace, member order and spelling all wash out
        BEAST_EXPECT(compact.first == spaced);
        BEAST_EXPECT(compact.first == reordered);
        BEAST_EXPECT(compact.first == cased);

        // the canonical form is a fixed point, which is what lets jsontx_verify
        // compare a node-derived form against a signer-derived one
        BEAST_EXPECT(sanitize_jsontx(compact.first).first == compact.first);

        // and both halves sit inside the caps the verifier applies
        BEAST_EXPECT(compact.first.size() <= jsontx_max_text);
        BEAST_EXPECT(compact.second.size() <= jsontx_max_diff);
    }

    void
    testSanitizeRejects()
    {
        testcase("sanitize_jsontx rejection");

        auto bad = [](std::string const& raw) {
            return threw([&] { (void)sanitize_jsontx(raw); });
        };

        // not json at all
        BEAST_EXPECT(bad("not json"));
        BEAST_EXPECT(bad(""));

        // a preimage is an object
        BEAST_EXPECT(bad(R"("just a string")"));
        BEAST_EXPECT(bad(R"([1,2,3])"));

        // every member must name a serializable field
        BEAST_EXPECT(bad(R"({"Account":"rTest","Fee":"10","BogusField":"x"})"));

        // two spellings of one field would canonicalize to the same member
        BEAST_EXPECT(bad(R"({"Fee":"10","fee":"20"})"));

        // null has no canonical spelling as a field value
        BEAST_EXPECT(bad(R"({"Account":"rTest","Fee":null})"));

        // framing violations reach sanitize_jsontx through jsontx_strict
        BEAST_EXPECT(bad("{\"Account\":\"rTest\"} // hi"));
        BEAST_EXPECT(
            bad(R"({"Account":"rTest","Fee":"10","Domain":"\u0041"})"));

        // over the text cap
        {
            std::string large = "{\"Account\":\"";
            large.append(jsontx_max_text, 'r');
            large += "\"}";
            BEAST_EXPECT(bad(large));
        }

        // an sfTime that is not exactly what jsontx_iso_str would produce
        BEAST_EXPECT(bad(R"({"Account":"rTest","Time":"2000-01-01"})"));
    }

    void
    testDeltaRejection()
    {
        testcase("unsanitize_jsontx rejection");

        // unsanitize_jsontx never parses its first argument, so any text will
        // do as a source for the decoder's own error paths
        std::string const san = "abcdefghij";

        // a delta must reconstruct something
        BEAST_EXPECT(unsanitizeThrows(san, ""));

        // only two ops exist
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x02", 1)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x7f", 1)));

        // truncated: an op with no varint after it
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x00", 1)));

        // zero-length literal
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x00\x00", 2)));

        // literal claiming more bytes than the delta holds
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string("\x00\x81\x01", 3) + "short"));

        // non-minimal varint: a continuation byte followed by zero
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x00\x80\x00", 3)));

        // overlong varint: more than four bytes
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string("\x00\xff\xff\xff\xff\x01", 6)));

        // two literals in a row; the encoder always merges them
        BEAST_EXPECT(unsanitizeThrows(
            san,
            std::string("\x00\x02", 2) + "ab" + std::string("\x00\x02", 2) +
                "cd"));

        // a copy shorter than the encoder's match threshold
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x00\x01", 3)));

        // a copy abutting the previous copy; the encoder always merges them
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string("\x01\x00\x04\x01\x04\x04", 6)));

        // offset past the end of the canonical form
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\xff\xff\x04", 4)));

        // length running past the end from a valid offset
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x08\x08", 3)));

        // oversize inputs are refused before any decoding
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string(jsontx_max_diff + 1, '\x00')));
        BEAST_EXPECT(unsanitizeThrows(
            std::string(jsontx_max_text + 1, 'x'),
            std::string("\x01\x00\x04", 3)));

        // a well-formed copy is accepted, so the cases above fail for their
        // own reasons and not because the fixture itself is malformed
        BEAST_EXPECT(
            unsanitize_jsontx(san, std::string("\x01\x00\x05", 3)) == "abcde");
    }

    void
    testNumbers()
    {
        testcase("number handling");

        std::int64_t out = 0;

        // small integers arrive as intValue / uintValue
        BEAST_EXPECT(jsontx_exact(Json::Value(0), out) && out == 0);
        BEAST_EXPECT(jsontx_exact(Json::Value(-1), out) && out == -1);
        BEAST_EXPECT(jsontx_exact(Json::Value(42u), out) && out == 42);

        // larger ones fall through to realValue and stay exact below 2^53
        BEAST_EXPECT(
            jsontx_exact(Json::Value(9007199254740991.0), out) &&
            out == 9007199254740991LL);
        BEAST_EXPECT(
            jsontx_exact(Json::Value(-9007199254740991.0), out) &&
            out == -9007199254740991LL);

        // at and past 2^53 distinct decimal integers share a double
        BEAST_EXPECT(!jsontx_exact(Json::Value(9007199254740992.0), out));
        BEAST_EXPECT(!jsontx_exact(Json::Value(-9007199254740992.0), out));

        // fractions, infinities and non-numbers are not integers
        BEAST_EXPECT(!jsontx_exact(Json::Value(1.5), out));
        BEAST_EXPECT(!jsontx_exact(
            Json::Value(std::numeric_limits<double>::infinity()), out));
        BEAST_EXPECT(!jsontx_exact(Json::Value(true), out));
        BEAST_EXPECT(!jsontx_exact(Json::Value("7"), out));

        // jsontx_num renders what jsontx_exact accepts
        BEAST_EXPECT(jsontx_num(Json::Value(0)) == "0");
        BEAST_EXPECT(jsontx_num(Json::Value(1)) == "1");
        BEAST_EXPECT(jsontx_num(Json::Value(-1)) == "-1");
        BEAST_EXPECT(jsontx_num(Json::Value(42)) == "42");
        BEAST_EXPECT(jsontx_num(Json::Value(1000000)) == "1000000");
        BEAST_EXPECT(
            jsontx_num(Json::Value(9007199254740991.0)) == "9007199254740991");
        BEAST_EXPECT(
            threw([] { (void)jsontx_num(Json::Value(9007199254740992.0)); }));
        BEAST_EXPECT(threw([] { (void)jsontx_num(Json::Value(1.5)); }));

        // jsontx_u64 additionally refuses negatives rather than wrapping them
        BEAST_EXPECT(jsontx_u64(Json::Value(0)) == 0);
        BEAST_EXPECT(jsontx_u64(Json::Value(100)) == 100);
        BEAST_EXPECT(threw([] { (void)jsontx_u64(Json::Value(-1)); }));
        BEAST_EXPECT(threw([] { (void)jsontx_u64(Json::Value(1.5)); }));
        BEAST_EXPECT(threw([] {
            (void)jsontx_u64(
                Json::Value(std::numeric_limits<double>::infinity()));
        }));

        // STUInt64::getJson renders hex, unpadded and lowercase, except for
        // the sMD_BaseTen fields which render base ten
        BEAST_EXPECT(jsontx_u64_str(sfTime, 0) == "0");
        BEAST_EXPECT(jsontx_u64_str(sfTime, 255) == "ff");
        BEAST_EXPECT(jsontx_u64_str(sfTime, 1000) == "3e8");
        BEAST_EXPECT(jsontx_u64_str(sfMaximumAmount, 255) == "255");
        BEAST_EXPECT(jsontx_u64_str(sfMaximumAmount, 1000) == "1000");
    }

    void
    testFieldLookup()
    {
        testcase("jsontx_field");

        auto const* account = &jsontx_field("Account");
        BEAST_EXPECT(account->fieldName == "Account");
        BEAST_EXPECT(account->fieldCode > 0);

        // the lookup is case-insensitive and always yields the one canonical
        // SField, which is what makes the canonical spelling well defined
        BEAST_EXPECT(&jsontx_field("account") == account);
        BEAST_EXPECT(&jsontx_field("ACCOUNT") == account);
        BEAST_EXPECT(&jsontx_field("AcCoUnT") == account);

        BEAST_EXPECT(jsontx_field("Fee").fieldName == "Fee");
        BEAST_EXPECT(
            jsontx_field("TransactionType").fieldName == "TransactionType");
        BEAST_EXPECT(jsontx_field("Time") == sfTime);
        BEAST_EXPECT(jsontx_field("JsonTxDelta") == sfJsonTxDelta);

        // anything unknown is sfInvalid, which sanitize_jsontx turns into a
        // rejection rather than a silently dropped member
        BEAST_EXPECT(jsontx_field("NoSuchField") == sfInvalid);
        BEAST_EXPECT(jsontx_field("") == sfInvalid);

        // folding is ASCII only, whatever the process locale: a Latin-1 or
        // UTF-8 capital never folds onto a field name
        BEAST_EXPECT(jsontx_lower("AbC\xC9\xC3\x89") == "abc\xC9\xC3\x89");
        BEAST_EXPECT(
            jsontx_field("\xC1"
                         "ccount") == sfInvalid);
        BEAST_EXPECT(jsontx_field("Acc\xC3\x93unt") == sfInvalid);
    }

    void
    testBounds()
    {
        testcase("consensus bounds");

        // These are consensus rules from the moment featureJsonTx activates,
        // so pin them: changing one changes which deltas verify.
        BEAST_EXPECT(jsontx_max_text == 8192);
        BEAST_EXPECT(jsontx_max_diff == 2048);
        BEAST_EXPECT(jsontx_max_ops == 1024);
        BEAST_EXPECT(jsontx_min_copy == 4);
        BEAST_EXPECT(jsontx_max_cand == 64);
        BEAST_EXPECT(jsontx_epoch_day == 10957);

        // sanitize_jsontx applies the delta caps itself, so a document it
        // accepts is never one unsanitize_jsontx would then refuse. This is
        // reached well before the text cap: indentation costs delta bytes
        // that the canonical form does not pay.
        auto indented = [](int memos) {
            std::string raw =
                "{\n    \"Account\": \"rTest\",\n    \"Memos\": [";
            for (int i = 0; i < memos; ++i)
                raw += std::string(i ? "," : "") +
                    "\n        {\n"
                    "            \"Memo\": {\n"
                    "                \"MemoData\": \"AA\"\n"
                    "            }\n"
                    "        }";
            raw += "\n    ]\n}";
            return raw;
        };

        BEAST_EXPECT(roundTrips(indented(20)));

        auto const tooFormatted = indented(40);
        BEAST_EXPECT(tooFormatted.size() < jsontx_max_text);
        BEAST_EXPECT(threw([&] { (void)sanitize_jsontx(tooFormatted); }));
    }

    static Rules
    noRules()
    {
        return Rules{std::unordered_set<uint256, beast::uhash<>>{}};
    }

    // What doSubmit does with a { tx, sig } pair, minus the RPC plumbing:
    // canonicalize, parse, and attach the signature and delta.
    static std::shared_ptr<STTx const>
    buildJsonTx(std::string const& raw, Buffer const& sig)
    {
        auto const [san, diff] = sanitize_jsontx(raw);
        Json::Value jv;
        if (Json::Reader r; !r.parse(san, jv))
            Throw<std::runtime_error>("unparsable canonical form");
        std::optional<std::uint64_t> ms;
        if (jv.isMember(sfTime.fieldName))
        {
            ms = jsontx_iso(jv[sfTime.fieldName].asString());
            jv.removeMember(sfTime.fieldName);
        }
        STParsedJSONObject parsed("tx_json", jv);
        if (!parsed.object)
            Throw<std::runtime_error>(
                parsed.error[jss::error_message].asString());
        if (ms)
            parsed.object->setFieldU64(sfTime, *ms);
        parsed.object->setFieldVL(sfTxnSignature, sig);
        parsed.object->setFieldVL(sfJsonTxDelta, makeSlice(diff));
        return std::make_shared<STTx const>(std::move(*parsed.object));
    }

    // the same transaction with f applied to its fields, rebuilt the way a
    // node would receive it: off the wire
    template <class F>
    static STTx
    mutate(STTx const& tx, F&& f)
    {
        STObject obj(tx);
        f(obj);
        Serializer ser;
        obj.add(ser);
        SerialIter si(ser.slice());
        return STTx(si);
    }

    static std::string
    paymentJson(
        std::string const& account,
        std::string const& pk,
        std::string const& amount)
    {
        return "{\n"
               "  \"TransactionType\": \"Payment\",\n"
               "  \"Account\": \"" +
            account +
            "\",\n"
            "  \"Destination\": \"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\",\n"
            "  \"Amount\": \"" +
            amount +
            "\",\n"
            "  \"Fee\": \"10\",\n"
            "  \"Sequence\": 7,\n"
            "  \"NetworkID\": 21337,\n"
            "  \"Time\": \"2026-09-26T01:02:03.456Z\",\n"
            "  \"SigningPubKey\": \"" +
            pk +
            "\"\n"
            "}";
    }

    void
    testVerify()
    {
        testcase("jsontx_verify end to end");

        // not a structured binding: clang before 16 cannot capture one in a
        // lambda, and the mutations below do
        auto const keys =
            generateKeyPair(KeyType::ed25519, generateSeed("jsontx"));
        auto const& pk = keys.first;
        auto const& sk = keys.second;
        auto const account = toBase58(calcAccountID(pk));
        auto const pkHex = strHex(pk.slice());

        auto const raw = paymentJson(account, pkHex, "1000000");
        auto const sig = sign(pk, sk, makeSlice(raw));
        auto const stx = buildJsonTx(raw, sig);

        // the preimage is recovered exactly, before and after the wire
        BEAST_EXPECT(jsontx_verify(*stx) == raw);
        {
            Serializer ser;
            stx->add(ser);
            SerialIter si(ser.slice());
            STTx const wire{si};
            BEAST_EXPECT(wire.getTransactionID() == stx->getTransactionID());
            BEAST_EXPECT(jsontx_verify(wire) == raw);
        }
        BEAST_EXPECT(
            stx->getFieldU64(sfTime) == jsontx_iso("2026-09-26T01:02:03.456Z"));

        // the binary check never accepts it, whatever the rules
        BEAST_EXPECT(
            !stx->checkSign(STTx::RequireFullyCanonicalSig::yes, noRules()));

        auto const bad = [](STTx const& t) {
            return threw([&] { (void)jsontx_verify(t); });
        };

        // strip the delta: nothing to reconstruct from, and the signature is
        // not over the binary signing hash
        {
            auto const t = mutate(
                *stx, [](STObject& o) { o.makeFieldAbsent(sfJsonTxDelta); });
            BEAST_EXPECT(bad(t));
            BEAST_EXPECT(
                !t.checkSign(STTx::RequireFullyCanonicalSig::yes, noRules()));
        }

        // a second delta spelling the same preimage - here, all literal - is
        // refused, so a relay cannot mint a second id for one signature
        {
            std::string lit(1, '\0');
            for (auto v = raw.size(); v; v >>= 7)
                lit += static_cast<char>((v & 0x7F) | (v > 0x7F ? 0x80 : 0));
            lit += raw;
            BEAST_EXPECT(
                unsanitize_jsontx(sanitize_jsontx(raw).first, lit) == raw);
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfJsonTxDelta, makeSlice(lit));
            });
            BEAST_EXPECT(t.getTransactionID() != stx->getTransactionID());
            BEAST_EXPECT(bad(t));
        }

        // the same fields reformatted: a different preimage, which this
        // signature does not cover
        {
            auto const compact = sanitize_jsontx(raw).first;
            auto const diff = sanitize_jsontx(compact).second;
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfJsonTxDelta, makeSlice(diff));
            });
            BEAST_EXPECT(bad(t));
        }

        // a signature the key made over a different transaction does not
        // carry over, even with that transaction's own delta
        {
            auto const other = paymentJson(account, pkHex, "999999999");
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfTxnSignature, sign(pk, sk, makeSlice(other)));
                o.setFieldVL(
                    sfJsonTxDelta, makeSlice(sanitize_jsontx(other).second));
            });
            BEAST_EXPECT(bad(t));
        }

        // any change to a signed field breaks the binding
        {
            auto const t = mutate(*stx, [](STObject& o) {
                o.setFieldAmount(sfAmount, STAmount(XRPAmount(2000000)));
            });
            BEAST_EXPECT(bad(t));
        }
        {
            auto const t = mutate(*stx, [](STObject& o) {
                o.setFieldU64(sfTime, o.getFieldU64(sfTime) + 1);
            });
            BEAST_EXPECT(bad(t));
        }
        {
            // a field the signer never saw
            auto const t = mutate(
                *stx, [](STObject& o) { o.setFieldU32(sfSourceTag, 1); });
            BEAST_EXPECT(bad(t));
        }

        // multi-signing has no single preimage to bind
        {
            auto const t = mutate(*stx, [](STObject& o) {
                o.setFieldArray(sfSigners, STArray{});
            });
            BEAST_EXPECT(bad(t));
        }

        // only ed25519 signs a JsonTx
        {
            auto const [spk, ssk] =
                generateKeyPair(KeyType::secp256k1, generateSeed("jsontx"));
            auto const sraw = paymentJson(
                toBase58(calcAccountID(spk)), strHex(spk.slice()), "1000000");
            auto const t = buildJsonTx(sraw, sign(spk, ssk, makeSlice(sraw)));
            BEAST_EXPECT(bad(*t));
        }
    }

    void
    testBinarySignatureWithDelta()
    {
        testcase("binary signature never authorises a delta");

        auto const [pk, sk] =
            generateKeyPair(KeyType::ed25519, generateSeed("binary"));
        auto const raw =
            paymentJson(toBase58(calcAccountID(pk)), strHex(pk.slice()), "5");

        // an ordinary binary-signed payment
        Json::Value jv;
        BEAST_EXPECT(Json::Reader{}.parse(sanitize_jsontx(raw).first, jv));
        jv.removeMember(sfTime.fieldName);
        STParsedJSONObject parsed("tx_json", jv);
        BEAST_EXPECT(parsed.object.has_value());
        if (!parsed.object)
            return;
        STTx tx(std::move(*parsed.object));
        tx.sign(pk, sk);
        BEAST_EXPECT(
            tx.checkSign(STTx::RequireFullyCanonicalSig::yes, noRules()));

        // sfJsonTxDelta is not a signing field, so the binary signature still
        // covers the transaction with one appended - which is exactly why the
        // binary check must refuse it rather than mint a new id
        auto const withDelta = mutate(tx, [](STObject& o) {
            o.setFieldVL(sfJsonTxDelta, Slice("\x01\x00\x04", 3));
        });
        BEAST_EXPECT(withDelta.getSigningHash() == tx.getSigningHash());
        BEAST_EXPECT(withDelta.getTransactionID() != tx.getTransactionID());
        BEAST_EXPECT(!withDelta.checkSign(
            STTx::RequireFullyCanonicalSig::yes, noRules()));
        BEAST_EXPECT(threw([&] { (void)jsontx_verify(withDelta); }));
    }

public:
    void
    run() override
    {
        testStrictAccepts();
        testStrictRejects();
        testIsoRoundTrip();
        testIsoRejects();
        testSanitizeRoundTrip();
        testCanonicalForm();
        testSanitizeRejects();
        testDeltaRejection();
        testNumbers();
        testFieldLookup();
        testBounds();
        testVerify();
        testBinarySignatureWithDelta();
    }
};

BEAST_DEFINE_TESTSUITE(JSONTxSignatures, protocol, ripple);

}  // namespace ripple
