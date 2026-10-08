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
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/InnerObjectFormats.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/STCurrency.h>
#include <xrpl/protocol/STIssue.h>
#include <xrpl/protocol/STNumber.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/STXChainBridge.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/protocol/st.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace ripple {

/**
 * Unit tests for JsonTx plaintext transaction signature support.
 *
 * Everything under test here becomes a consensus rule when featureJsonTx
 * activates, so besides behaviour these tests pin constants, the encoder's
 * exact output, and - in testEveryFormat - the JSON rendering of every field
 * of every transaction format, which jsontx_verify depends on.
 */
class JSONTxSignatures_test : public beast::unit_test::suite
{
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

    // The exception message, or "" if f() did not throw.
    template <class F>
    static std::string
    why(F&& f)
    {
        try
        {
            f();
        }
        catch (std::exception const& e)
        {
            return e.what();
        }
        return "";
    }

    static bool
    strictOk(std::string_view s)
    {
        return !threw([&] { jsontx_strict(s); });
    }

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

    static std::string
    canon(std::string const& raw)
    {
        return jsontx_canonical(raw);
    }

    static bool
    canonThrows(std::string const& raw)
    {
        return threw([&] { (void)jsontx_canonical(raw); });
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
        BEAST_EXPECT(strictOk(R"({"outer":{"inner":"x"}})"));
        BEAST_EXPECT(strictOk(R"({"a":[1,2,3]})"));
        BEAST_EXPECT(strictOk(R"({"a":[]})"));
        BEAST_EXPECT(strictOk(R"({"a":[[],[[]]]})"));

        // json's whitespace set, between tokens and around the document
        BEAST_EXPECT(strictOk(" { \"key\" : \"val\" } "));
        BEAST_EXPECT(strictOk("{\r\n\t\"key\":\"val\"\r\n}\n"));

        // every printable ASCII byte but '"' and '\' may sit in a string
        {
            std::string s = "{\"k\":\"";
            for (int c = 0x20; c <= 0x7E; ++c)
                if (c != '"' && c != '\\')
                    s += static_cast<char>(c);
            s += "\"}";
            BEAST_EXPECT(strictOk(s));
        }

        // a comment marker inside a string is just text
        BEAST_EXPECT(strictOk(R"({"key":"value // not a comment /* */"})"));

        // integers across the whole interoperable range, kept as digits
        BEAST_EXPECT(strictOk(
            R"({"n":[0,-1,10,4294967296,9007199254740991,-9007199254740991]})"));

        // the same name in different objects is not a duplicate
        BEAST_EXPECT(strictOk(R"({"a":{"x":"1"},"b":{"x":"1"}})"));

        // exactly at the depth cap
        {
            std::string s;
            for (std::size_t i = 0; i < jsontx_max_depth; ++i)
                s += i ? "[" : "{\"a\":";
            for (std::size_t i = 0; i < jsontx_max_depth; ++i)
                s += i + 1 < jsontx_max_depth ? "]" : "}";
            BEAST_EXPECT(strictOk(s));
        }
    }

    void
    testStrictRejects()
    {
        testcase("jsontx_strict rejects");

        // comments outside the document, in either style
        BEAST_EXPECT(!strictOk("{\"key\":\"value\"}\n// real comment"));
        BEAST_EXPECT(!strictOk("{\"key\":\"value\"} /* real comment */"));
        BEAST_EXPECT(!strictOk("// leading comment\n{\"key\":\"value\"}"));
        BEAST_EXPECT(!strictOk("{/* c */\"key\":\"value\"}"));

        // no escapes at all: none can ever verify, and \u in particular can
        // hide what is being signed
        BEAST_EXPECT(!strictOk(R"({"key":"\u0048ello"})"));
        BEAST_EXPECT(!strictOk(R"({"key":"\\utest"})"));
        BEAST_EXPECT(!strictOk(R"({"key":"a\"b"})"));
        BEAST_EXPECT(!strictOk(R"({"key":"a\/b"})"));
        BEAST_EXPECT(!strictOk(R"({"key":"a\nb"})"));

        // nothing may ride along after the document
        BEAST_EXPECT(!strictOk(R"({"a":"b"} "trailing")"));
        BEAST_EXPECT(!strictOk(R"({"a":"b"} {"c":"d"})"));
        BEAST_EXPECT(!strictOk(R"({"a":"b"}})"));

        // unbalanced or unterminated
        BEAST_EXPECT(!strictOk(R"({"a":"b")"));
        BEAST_EXPECT(!strictOk(R"({"a":"b})"));
        BEAST_EXPECT(!strictOk(R"({"a":["b"})"));
        BEAST_EXPECT(!strictOk(R"({"a":["b"}])"));

        // real json grammar, not just framing
        BEAST_EXPECT(!strictOk(R"({"key":"val",})"));
        BEAST_EXPECT(!strictOk(R"({"a":[1,]})"));
        BEAST_EXPECT(!strictOk(R"({'key':'val'})"));
        BEAST_EXPECT(!strictOk(R"({key:"val"})"));
        BEAST_EXPECT(!strictOk(R"({"key" "val"})"));
        BEAST_EXPECT(!strictOk(R"({"a":"b" "c":"d"})"));
        BEAST_EXPECT(!strictOk(R"({,"a":"b"})"));

        // the root is an object and nothing but whitespace comes before it
        BEAST_EXPECT(!strictOk(R"("just a string")"));
        BEAST_EXPECT(!strictOk("42"));
        BEAST_EXPECT(!strictOk(R"([1,2,3])"));
        BEAST_EXPECT(!strictOk(R"(x{"a":"b"})"));
        BEAST_EXPECT(!strictOk("\xEF\xBB\xBF{}"));  // byte order mark
        BEAST_EXPECT(!strictOk(""));
        BEAST_EXPECT(!strictOk("   "));

        // no transaction field is a boolean or a null
        BEAST_EXPECT(!strictOk(R"({"b":true})"));
        BEAST_EXPECT(!strictOk(R"({"b":false})"));
        BEAST_EXPECT(!strictOk(R"({"n":null})"));

        // a number is spelled exactly as its value
        BEAST_EXPECT(!strictOk(R"({"n":1.0})"));
        BEAST_EXPECT(!strictOk(R"({"n":1e6})"));
        BEAST_EXPECT(!strictOk(R"({"n":1E6})"));
        BEAST_EXPECT(!strictOk(R"({"n":4503599627370497.5})"));
        BEAST_EXPECT(!strictOk(R"({"n":012})"));
        BEAST_EXPECT(!strictOk(R"({"n":00})"));
        BEAST_EXPECT(!strictOk(R"({"n":-0})"));
        BEAST_EXPECT(!strictOk(R"({"n":+1})"));
        BEAST_EXPECT(!strictOk(R"({"n":-})"));
        BEAST_EXPECT(!strictOk(R"({"n":--1})"));
        BEAST_EXPECT(!strictOk(R"({"n":1-})"));
        BEAST_EXPECT(!strictOk(R"({"n":0x10})"));
        BEAST_EXPECT(!strictOk(R"({"n":[1,2.5]})"));
        BEAST_EXPECT(strictOk(R"({"n":"1.5e3"})"));

        // ...and no bigger than a double holds exactly: 2^53 - 1 either way
        BEAST_EXPECT(!strictOk(R"({"n":9007199254740992})"));
        BEAST_EXPECT(!strictOk(R"({"n":-9007199254740992})"));
        BEAST_EXPECT(!strictOk(R"({"n":10000000000000000})"));
        BEAST_EXPECT(!strictOk(R"({"n":18446744073709551616})"));
        BEAST_EXPECT(!strictOk(
            R"({"n":999999999999999999999999999999999999999999999999})"));
        // a string holds any digits
        BEAST_EXPECT(strictOk(R"({"n":"18446744073709551616"})"));

        // printable ASCII only, inside and outside strings
        BEAST_EXPECT(!strictOk("{\"a\":\"x\ty\"}"));
        BEAST_EXPECT(!strictOk("{\"a\":\"x\x1b[2Jy\"}"));
        BEAST_EXPECT(!strictOk(std::string("{\"a\":\"x\0y\"}", 11)));
        BEAST_EXPECT(!strictOk("{\"a\":\"x\x7fy\"}"));
        BEAST_EXPECT(!strictOk("{\"a\":\"caf\xC3\xA9\"}"));
        BEAST_EXPECT(
            !strictOk("{\"a\":\"\xE2\x80\xAE"
                      "evil\"}"));  // RLO
        BEAST_EXPECT(!strictOk("{\"a\":\"x\xFFy\"}"));
        BEAST_EXPECT(!strictOk("{\"a\":\"b\"}\xC2\xA0"));  // NBSP after

        // an exact duplicate: jsoncpp would silently keep the second
        BEAST_EXPECT(!strictOk(R"({"Fee":"10","Fee":"20"})"));
        BEAST_EXPECT(!strictOk(R"({"o":{"x":"1","y":"2","x":"1"}})"));

        // one past the depth cap
        {
            std::string s;
            for (std::size_t i = 0; i <= jsontx_max_depth; ++i)
                s += i ? "[" : "{\"a\":";
            for (std::size_t i = 0; i <= jsontx_max_depth; ++i)
                s += i < jsontx_max_depth ? "]" : "}";
            BEAST_EXPECT(!strictOk(s));
        }

        // over the size cap
        {
            std::string s = "{\"a\":\"";
            s.append(jsontx_max_text, 'r');
            s += "\"}";
            BEAST_EXPECT(!strictOk(s));
        }
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
        BEAST_EXPECT(rt("2000-02-29T00:00:00.000Z"));
        BEAST_EXPECT(rt("2100-02-28T23:59:59.999Z"));
        BEAST_EXPECT(rt("2400-02-29T00:00:00.000Z"));
        BEAST_EXPECT(rt("9999-12-31T23:59:59.999Z"));

        BEAST_EXPECT(jsontx_iso("9999-12-31T23:59:59.999Z") == jsontx_max_time);
        BEAST_EXPECT(
            jsontx_iso_str(jsontx_max_time) == "9999-12-31T23:59:59.999Z");
        BEAST_EXPECT(threw([] { (void)jsontx_iso_str(jsontx_max_time + 1); }));

        BEAST_EXPECT(jsontx_iso("2000-01-01T00:00:00.001Z") == 1);
        BEAST_EXPECT(jsontx_iso("2000-01-02T00:00:00.000Z") == 86400000);

        // agrees with the ripple epoch the rest of the code uses: 2026-09-28
        // is day 9767 after 2000-01-01
        BEAST_EXPECT(
            jsontx_iso("2026-09-28T00:00:00.000Z") == 9767ull * 86400000);

        // every day boundary for a few centuries round-trips
        bool all = true;
        for (std::uint64_t d = 0; d < 146097 * 2; d += 7)
        {
            auto const ms = d * 86400000 + (d % 86400) * 1000 + d % 1000;
            all = all && jsontx_iso(jsontx_iso_str(ms)) == ms;
        }
        BEAST_EXPECT(all);
    }

    void
    testIsoRejects()
    {
        testcase("ISO 8601 rejection");

        auto bad = [](char const* s) {
            return threw([&] { (void)jsontx_iso(s); });
        };

        BEAST_EXPECT(bad("2000-01-01T00:00:00"));
        BEAST_EXPECT(bad("2000-01-01T00:00:00.Z"));
        BEAST_EXPECT(bad("2000-01-01T00:00:00.000"));
        BEAST_EXPECT(bad("2000-01-01T00:00:00.000+00:00"));
        BEAST_EXPECT(bad("2000-01-01 00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-01-01t00:00:00.000z"));
        BEAST_EXPECT(bad("2000/01/01T00:00:00.000Z"));
        BEAST_EXPECT(bad("+02000-01-01T00:00:00.00Z"));
        BEAST_EXPECT(bad("not-a-date"));
        BEAST_EXPECT(bad(""));

        BEAST_EXPECT(bad("2000-00-01T00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-13-01T00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-01-00T00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-01-32T00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-04-31T00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-02-30T00:00:00.000Z"));
        BEAST_EXPECT(bad("2001-02-29T00:00:00.000Z"));
        BEAST_EXPECT(bad("2100-02-29T00:00:00.000Z"));
        BEAST_EXPECT(bad("2000-01-01T24:00:00.000Z"));
        BEAST_EXPECT(bad("2000-01-01T00:60:00.000Z"));
        BEAST_EXPECT(bad("2000-01-01T00:00:60.000Z"));

        BEAST_EXPECT(bad("1999-12-31T23:59:59.999Z"));
    }

    void
    testSanitizeRoundTrip()
    {
        testcase("sanitize/unsanitize round-trip");

        BEAST_EXPECT(roundTrips(
            R"({"TransactionType":"Payment","Sequence":1,"Fee":"10","SigningPubKey":"ED0000","Account":"rTest"})"));
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED0000","TransactionType":"Payment"})"));
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Time":"2000-01-01T00:00:00.000Z"})"));
        BEAST_EXPECT(roundTrips(R"({ "Account" : "rTest" , "Fee" : "10" })"));

        BEAST_EXPECT(
            roundTrips("{\n"
                       "    \"Account\": \"rTest\",\n"
                       "    \"Fee\": \"10\",\n"
                       "    \"Sequence\": 1,\n"
                       "    \"SigningPubKey\": \"ED\",\n"
                       "    \"Time\": \"2000-01-01T00:00:00.000Z\",\n"
                       "    \"TransactionType\": \"Payment\"\n"
                       "}"));
        BEAST_EXPECT(
            roundTrips("{\r\n"
                       "  \"Account\": \"rTest\",\r\n"
                       "  \"Fee\": \"10\",\r\n"
                       "  \"TransactionType\": \"Payment\"\r\n"
                       "}\r\n"));

        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Memos":[{"Memo":{"MemoData":"48656C6C6F"}}],"Sequence":1})"));
        BEAST_EXPECT(roundTrips(
            R"({"Account":"rTest","Fee":"10","Flags":2147483648,"Sequence":1})"));
        BEAST_EXPECT(roundTrips(R"({"account":"rTest","FEE":"10"})"));

        // bare numbers where the canonical form wants strings
        BEAST_EXPECT(roundTrips(
            R"({"Amount": 9007199254740991, "Fee": 12, "Sequence": 0})"));
        BEAST_EXPECT(roundTrips(
            R"({"Amount": "100000000000000000", "Fee": 12, "Sequence": 0})"));

        // a path payment
        BEAST_EXPECT(roundTrips(
            R"({"Paths":[[{"account":"rA","type":1},{"currency":"USD","issuer":"rB","type":48}]]})"));
    }

    void
    testCanonicalForm()
    {
        testcase("canonical form");

        auto const compact =
            sanitize_jsontx(R"({"Account":"rTest","Fee":"10"})");
        auto const spaced = canon(R"({ "Account" : "rTest" , "Fee" : "10" })");
        auto const reordered = canon(R"({"Fee":"10","Account":"rTest"})");
        auto const cased = canon(R"({"account":"rTest","fee":"10"})");

        BEAST_EXPECT(compact.first == spaced);
        BEAST_EXPECT(compact.first == reordered);
        BEAST_EXPECT(compact.first == cased);
        BEAST_EXPECT(canon(compact.first) == compact.first);
        BEAST_EXPECT(compact.first == R"({"Fee":"10","Account":"rTest"})");

        // jsontx_canonical is exactly the first half of sanitize_jsontx
        auto const doc = std::string(
            "{\n  \"Sequence\": 5,\n  \"TransactionType\": \"Payment\"\n}");
        BEAST_EXPECT(canon(doc) == sanitize_jsontx(doc).first);
        BEAST_EXPECT(
            canon(doc) == R"({"TransactionType":"Payment","Sequence":5})");

        // members of a non-field object sort bytewise, values are strings
        BEAST_EXPECT(
            canon(R"({"Amount":{"value":1,"issuer":"rI","currency":"USD"}})") ==
            R"({"Amount":{"currency":"USD","issuer":"rI","value":"1"}})");

        // array order is significant and kept
        BEAST_EXPECT(
            canon(
                R"({"Memos":[{"Memo":{"MemoType":"02"}},{"Memo":{"MemoType":"01"}}]})") ==
            R"({"Memos":[{"Memo":{"MemoType":"02"}},{"Memo":{"MemoType":"01"}}]})");
    }

    void
    testSanitizeRejects()
    {
        testcase("sanitize_jsontx rejection");

        auto bad = [](std::string const& raw) {
            return threw([&] { (void)sanitize_jsontx(raw); });
        };

        BEAST_EXPECT(bad("not json"));
        BEAST_EXPECT(bad(""));
        BEAST_EXPECT(bad(R"({"Account":"rTest","Fee":"10","BogusField":"x"})"));
        BEAST_EXPECT(bad(R"({"Fee":"10","fee":"20"})"));
        BEAST_EXPECT(bad(R"({"Fee":"10","Fee":"10"})"));
        BEAST_EXPECT(bad(R"({"Account":"rTest","Fee":null})"));
        BEAST_EXPECT(bad("{\"Account\":\"rTest\"} // hi"));
        BEAST_EXPECT(bad(R"({"Domain":"\u0041"})"));
        BEAST_EXPECT(bad(R"({"Account":"rTest","Time":"2000-01-01"})"));

        // Time is only ever an ISO string, never its number of milliseconds
        BEAST_EXPECT(bad(R"({"Time":0})"));

        // field types decide shape
        BEAST_EXPECT(bad(R"({"Memos":{"Memo":{}}})"));  // array wanted
        BEAST_EXPECT(bad(R"({"Memos":["x"]})"));        // objects in it
        BEAST_EXPECT(bad(R"({"Memos":[{"Memo":{},"Memo2":{}}]})"));
        BEAST_EXPECT(bad(R"({"Memos":[{}]})"));          // exactly one
        BEAST_EXPECT(bad(R"({"Memo":"x"})"));            // object wanted
        BEAST_EXPECT(bad(R"({"Sequence":{"a":"b"}})"));  // scalar wanted
        BEAST_EXPECT(bad(R"({"Account":["r"]})"));
        BEAST_EXPECT(bad(R"({"Amount":{"value":{"x":"1"}}})"));
        BEAST_EXPECT(bad(R"({"Amount":[]})"));
        BEAST_EXPECT(bad(R"({"Paths":[{"account":"r"}]})"));
        BEAST_EXPECT(bad(R"({"Paths":[[["x"]]]})"));
        BEAST_EXPECT(bad(R"({"Hashes":[1]})"));
        BEAST_EXPECT(bad(R"({"Hashes":"00"})"));
    }

    void
    testDeltaRejection()
    {
        testcase("unsanitize_jsontx rejection");

        std::string const san = "abcdefghij";

        BEAST_EXPECT(unsanitizeThrows(san, ""));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x02", 1)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x7f", 1)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x00", 1)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x00", 2)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x00\x00", 2)));
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string("\x00\x81\x01", 3) + "short"));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x00\x80\x00", 3)));
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string("\x00\xff\xff\xff\xff\x01", 6)));
        BEAST_EXPECT(unsanitizeThrows(
            san,
            std::string("\x00\x02", 2) + "ab" + std::string("\x00\x02", 2) +
                "cd"));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x00\x01", 3)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x00\x03", 3)));
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string("\x01\x00\x04\x01\x04\x04", 6)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\xff\xff\x04", 4)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x0b\x04", 3)));
        BEAST_EXPECT(unsanitizeThrows(san, std::string("\x01\x08\x08", 3)));
        BEAST_EXPECT(
            unsanitizeThrows(san, std::string(jsontx_max_diff + 1, '\x00')));
        BEAST_EXPECT(unsanitizeThrows(
            std::string(jsontx_max_text + 1, 'x'),
            std::string("\x01\x00\x04", 3)));

        // The smallest op is three bytes, so jsontx_max_diff already admits
        // no more than 682 ops and jsontx_max_ops is defence in depth. The
        // densest legal delta - a one-byte literal alternating with a
        // non-abutting copy - decodes, and one op more does not fit.
        {
            std::string const src(64, 'x');
            std::string d;
            std::size_t n = 0;
            for (; d.size() + 3 <= jsontx_max_diff; ++n)
                d += n % 2 ? std::string("\x01\x00\x04", 3)
                           : std::string("\x00\x01y", 3);
            BEAST_EXPECT(n == jsontx_max_diff / 3);
            BEAST_EXPECT(n < jsontx_max_ops);
            BEAST_EXPECT(!unsanitizeThrows(src, d));
            BEAST_EXPECT(
                unsanitizeThrows(src, d + std::string("\x01\x00\x04", 3)));
        }

        // expansion past the text cap from copies alone
        {
            std::string const src(4000, 'x');
            std::string d;
            // copy 4000, literal, copy 4000, literal, copy 4000 > 8192
            for (int i = 0; i < 3; ++i)
            {
                d += std::string("\x01\x00\xa0\x1f", 4);  // off 0, len 4000
                d += std::string("\x00\x01-", 3);
            }
            BEAST_EXPECT(unsanitizeThrows(src, d));
        }

        // well-formed controls, so the cases above fail for their own reasons
        BEAST_EXPECT(
            unsanitize_jsontx(san, std::string("\x01\x00\x05", 3)) == "abcde");
        BEAST_EXPECT(
            unsanitize_jsontx(
                san, std::string("\x01\x00\x04\x01\x05\x04", 6)) == "abcdfghi");
        BEAST_EXPECT(
            unsanitize_jsontx(san, std::string("\x00\x01Z\x01\x06\x04", 6)) ==
            "Zghij");
    }

    void
    testNumbers()
    {
        testcase("number handling");

        // the review case: bare integers past 32 bits used to be refused by
        // jsoncpp before canonicalization ever ran; now the whole
        // interoperable range works
        BEAST_EXPECT(
            canon(R"({"Amount":4294967296})") == R"({"Amount":"4294967296"})");
        BEAST_EXPECT(
            canon(R"({"Amount":9007199254740991})") ==
            R"({"Amount":"9007199254740991"})");
        BEAST_EXPECT(canon(R"({"Amount":-5})") == R"({"Amount":"-5"})");
        BEAST_EXPECT(
            canon(R"({"Amount":-9007199254740991})") ==
            R"({"Amount":"-9007199254740991"})");

        // past it the value goes in a string, where it canonicalizes to the
        // same bytes a bare number would have
        BEAST_EXPECT(canonThrows(R"({"Amount":9007199254740992})"));
        BEAST_EXPECT(canonThrows(R"({"Amount":100000000000000000})"));
        BEAST_EXPECT(
            canon(R"({"Amount":"100000000000000000"})") ==
            R"({"Amount":"100000000000000000"})");
        BEAST_EXPECT(
            canon(R"({"Amount":9007199254740991})") ==
            canon(R"({"Amount":"9007199254740991"})"));
        BEAST_EXPECT(why([] {
                         (void)jsontx_canonical(
                             R"({"Amount":9007199254740992})");
                     }).find("2^53") != std::string::npos);

        // small unsigned fields stay bare, in range, non-negative
        BEAST_EXPECT(
            canon(R"({"Flags":4294967295})") == R"({"Flags":4294967295})");
        BEAST_EXPECT(canonThrows(R"({"Flags":4294967296})"));
        BEAST_EXPECT(canonThrows(R"({"Flags":-1})"));
        BEAST_EXPECT(
            canon(R"({"TransferFee":65535})") == R"({"TransferFee":65535})");
        BEAST_EXPECT(canonThrows(R"({"TransferFee":65536})"));
        BEAST_EXPECT(canon(R"({"TickSize":255})") == R"({"TickSize":255})");
        BEAST_EXPECT(canonThrows(R"({"TickSize":256})"));
        BEAST_EXPECT(canonThrows(R"({"TickSize":9007199254740991})"));

        // UInt64 as a bare number renders as getJson would: hex, lowercase,
        // unpadded - or base ten for sMD_BaseTen fields
        BEAST_EXPECT(canon(R"({"Cookie":255})") == R"({"Cookie":"ff"})");
        BEAST_EXPECT(
            canon(R"({"Cookie":9007199254740991})") ==
            R"({"Cookie":"1fffffffffffff"})");
        BEAST_EXPECT(canonThrows(R"({"Cookie":-1})"));
        BEAST_EXPECT(
            canon(R"({"MaximumAmount":9007199254740991})") ==
            R"({"MaximumAmount":"9007199254740991"})");
        // the rest of its range is written the way getJson writes it, as a
        // string, which is taken verbatim; the binding check decides
        BEAST_EXPECT(canonThrows(R"({"Cookie":18446744073709551615})"));
        BEAST_EXPECT(
            canon(R"({"Cookie":"ffffffffffffffff"})") ==
            R"({"Cookie":"ffffffffffffffff"})");
        BEAST_EXPECT(
            canon(R"({"MaximumAmount":"18446744073709551615"})") ==
            R"({"MaximumAmount":"18446744073709551615"})");
        BEAST_EXPECT(canon(R"({"Cookie":"FF"})") == R"({"Cookie":"FF"})");

        BEAST_EXPECT(jsontx_u64_str(sfTime, 0) == "0");
        BEAST_EXPECT(jsontx_u64_str(sfTime, 255) == "ff");
        BEAST_EXPECT(jsontx_u64_str(sfTime, 1000) == "3e8");
        BEAST_EXPECT(jsontx_u64_str(sfMaximumAmount, 255) == "255");
        BEAST_EXPECT(
            jsontx_u64_str(sfCookie, ~std::uint64_t{0}) == "ffffffffffffffff");
    }

    void
    testFieldLookup()
    {
        testcase("jsontx_field");

        auto const* account = &jsontx_field("Account");
        BEAST_EXPECT(account->fieldName == "Account");
        BEAST_EXPECT(&jsontx_field("account") == account);
        BEAST_EXPECT(&jsontx_field("ACCOUNT") == account);
        BEAST_EXPECT(&jsontx_field("AcCoUnT") == account);

        BEAST_EXPECT(jsontx_field("Time") == sfTime);
        BEAST_EXPECT(jsontx_field("JsonTxDelta") == sfJsonTxDelta);
        BEAST_EXPECT(jsontx_field("LastTxnTime") == sfLastTxnTime);
        BEAST_EXPECT(jsontx_field("NoSuchField") == sfInvalid);
        BEAST_EXPECT(jsontx_field("") == sfInvalid);

        BEAST_EXPECT(jsontx_lower("AbC\xC9\xC3\x89") == "abc\xC9\xC3\x89");
        BEAST_EXPECT(
            jsontx_field("\xC1"
                         "ccount") == sfInvalid);
        BEAST_EXPECT(jsontx_field("Acc\xC3\x93unt") == sfInvalid);

        // every lookup the table could make is unambiguous
        std::set<std::string> seen;
        bool unique = true;
        for (auto const& [code, f] : SField::knownCodeToField)
            if (f->isUseful() && f->isBinary() && f->fieldType < 10000 &&
                !f->fieldName.empty())
                unique =
                    unique && seen.insert(jsontx_lower(f->fieldName)).second;
        BEAST_EXPECT(unique);
    }

    void
    testBounds()
    {
        testcase("consensus bounds");

        BEAST_EXPECT(jsontx_max_text == 8192);
        BEAST_EXPECT(jsontx_max_diff == 2048);
        BEAST_EXPECT(jsontx_max_ops == 1024);
        BEAST_EXPECT(jsontx_min_copy == 4);
        BEAST_EXPECT(jsontx_max_cand == 64);
        BEAST_EXPECT(jsontx_max_depth == 32);
        BEAST_EXPECT(jsontx_max_int == 9007199254740991ull);
        BEAST_EXPECT(jsontx_epoch_day == 10957);
        BEAST_EXPECT(jsontx_sign_prefix == std::string_view("\xFFJTX", 4));
        BEAST_EXPECT(jsontx_signing_data("{}") == std::string("\xFFJTX{}", 6));

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

        // a canonical form can outgrow its document - bare numbers in
        // string-valued places gain quotes - and past the cap is refused
        // rather than produced
        {
            std::string raw = "{\"Paths\":[[";
            while (raw.size() + 14 <= jsontx_max_text)
                raw += "{\"type\":1},";
            raw.back() = ']';
            raw += "]}";
            BEAST_EXPECT(raw.size() <= jsontx_max_text);
            BEAST_EXPECT(strictOk(raw));
            BEAST_EXPECT(why([&] {
                             (void)jsontx_canonical(raw);
                         }).find("too large") != std::string::npos);
        }
    }

    void
    testImpliedSequence()
    {
        testcase("omitted Sequence with a Time or TicketSequence");

        std::string const t = "\"2026-09-28T00:00:00.000Z\"";

        // implied as 0, and always present in the canonical form
        BEAST_EXPECT(
            canon("{\"Account\":\"rA\",\"Time\":" + t + "}") ==
            "{\"Sequence\":0,\"Time\":" + t + ",\"Account\":\"rA\"}");
        BEAST_EXPECT(
            canon(R"({"Account":"rA","TicketSequence":5})") ==
            R"({"Sequence":0,"TicketSequence":5,"Account":"rA"})");
        // the trigger matches case-insensitively, like every field name
        BEAST_EXPECT(
            canon("{\"account\":\"rA\",\"TIME\":" + t + "}") ==
            "{\"Sequence\":0,\"Time\":" + t + ",\"Account\":\"rA\"}");
        BEAST_EXPECT(
            canon(R"({"ticketsequence":5})") ==
            R"({"Sequence":0,"TicketSequence":5})");

        // the two spellings are one canonical form; each round-trips to its
        // own text, through its own delta
        std::string const omitted =
            "{\n  \"Account\": \"rA\",\n  \"Time\": " + t + "\n}";
        std::string const spelled =
            "{\n  \"Account\": \"rA\",\n  \"Sequence\": 0,\n  \"Time\": " + t +
            "\n}";
        BEAST_EXPECT(canon(omitted) == canon(spelled));
        BEAST_EXPECT(canon(canon(omitted)) == canon(omitted));
        BEAST_EXPECT(roundTrips(omitted));
        BEAST_EXPECT(roundTrips(spelled));
        BEAST_EXPECT(
            sanitize_jsontx(omitted).second != sanitize_jsontx(spelled).second);

        // a Sequence that is there is never touched - including a lowercase
        // one, which would otherwise surface as a duplicate
        BEAST_EXPECT(
            canon("{\"Sequence\":7,\"Time\":" + t + "}") ==
            "{\"Sequence\":7,\"Time\":" + t + "}");
        BEAST_EXPECT(
            canon("{\"sequence\":7,\"Time\":" + t + "}") ==
            "{\"Sequence\":7,\"Time\":" + t + "}");
        BEAST_EXPECT(
            canon("{\"Sequence\":0,\"Time\":" + t + "}") ==
            "{\"Sequence\":0,\"Time\":" + t + "}");

        // with neither, nothing is implied; the submit RPC refuses it
        BEAST_EXPECT(canon(R"({"Account":"rA"})") == R"({"Account":"rA"})");
        BEAST_EXPECT(canon(R"({"Fee":"10"})") == R"({"Fee":"10"})");

        // the root only: an inner object is never given one
        BEAST_EXPECT(
            canon(
                R"({"Sequence":1,"Memos":[{"Memo":{"TicketSequence":1}}]})") ==
            R"({"Sequence":1,"Memos":[{"Memo":{"TicketSequence":1}}]})");
        BEAST_EXPECT(
            canon(
                "{\"Sequence\":1,\"Memos\":[{\"Memo\":{\"Time\":" + t +
                "}}]}") ==
            "{\"Sequence\":1,\"Memos\":[{\"Memo\":{\"Time\":" + t + "}}]}");

        // a malformed Time still fails as itself, not as a sequence problem
        BEAST_EXPECT(why([] {
                         (void)jsontx_canonical(R"({"Time":"yesterday"})");
                     }).find("Time") != std::string::npos);
    }

    void
    testEncoderPinned()
    {
        testcase("encoder output is pinned");

        // The delta is compared byte for byte in jsontx_verify, so its exact
        // encoding is a consensus rule. Any change here needs an amendment.
        std::string const raw =
            "{\n"
            "  \"TransactionType\": \"Payment\",\n"
            "  \"Account\": \"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh\",\n"
            "  \"Amount\": \"1000000\",\n"
            "  \"Fee\": \"10\",\n"
            "  \"Sequence\": 0,\n"
            "  \"Time\": \"2026-09-28T00:00:00.000Z\"\n"
            "}";
        auto const [san, diff] = sanitize_jsontx(raw);
        BEAST_EXPECT(
            san ==
            R"({"TransactionType":"Payment","Sequence":0,"Time":"2026-09-28T00:00:00.000Z","Amount":"1000000","Fee":"10","Account":"rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh"})");
        BEAST_EXPECT(strHex(diff) == PINNED_DELTA);
        BEAST_EXPECT(unsanitize_jsontx(san, diff) == raw);
    }

    static constexpr char const* PINNED_DELTA =
        "00047B0A202001011200012001130A00030A2020016A0A00012001742400042C0A20"
        "20014C0900012001550A00030A2020015F0600012001650500030A2020011D0B0006"
        "20302C0A2020012A0700012001311A00020A7D";

    void
    testPaths()
    {
        testcase("Paths");

        // The review case: path steps are not ledger fields, and getJson
        // spells them in lowercase with a numeric type.
        auto const alice = calcAccountID(
            generateKeyPair(KeyType::ed25519, generateSeed("alice")).first);
        auto const gw = calcAccountID(
            generateKeyPair(KeyType::ed25519, generateSeed("gw")).first);
        auto const usd = to_currency("USD");

        STPathSet ps(sfPaths);
        ps.push_back(
            STPath({STPathElement(alice, std::nullopt, std::nullopt)}));
        ps.push_back(STPath(
            {STPathElement(std::nullopt, usd, gw),
             STPathElement(alice, std::nullopt, std::nullopt)}));

        STObject obj(sfTransaction);
        obj.set(std::move(ps));
        auto const text =
            Json::FastWriter{}.write(obj.getJson(JsonOptions::none));

        auto const can = canon(text);
        BEAST_EXPECT(canon(can) == can);
        BEAST_EXPECT(
            can.find("\"account\":\"" + toBase58(alice)) != std::string::npos);
        BEAST_EXPECT(can.find("\"Account\"") == std::string::npos);

        // and it parses back to the same paths
        Json::Value jv;
        BEAST_EXPECT(Json::Reader{}.parse(can, jv));
        auto const back = STPathSet(sfPaths);
        STParsedJSONObject parsed("tx_json", jv);
        BEAST_EXPECT(parsed.object.has_value());
        if (parsed.object)
            BEAST_EXPECT(
                parsed.object->getFieldPathSet(sfPaths) ==
                obj.getFieldPathSet(sfPaths));
    }

    //--------------------------------------------------------------------------
    // End to end

    // What doSubmit does with a { tx, sig } pair, minus the RPC plumbing.
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

    static Buffer
    jsonSign(PublicKey const& pk, SecretKey const& sk, std::string const& raw)
    {
        return sign(pk, sk, makeSlice(jsontx_signing_data(raw)));
    }

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

    static Rules
    noRules()
    {
        return Rules{std::unordered_set<uint256, beast::uhash<>>{}};
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
            "  \"Amount\": " +
            amount +
            ",\n"
            "  \"Fee\": \"10\",\n"
            "  \"Sequence\": 0,\n"
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

        auto const keys =
            generateKeyPair(KeyType::ed25519, generateSeed("jsontx"));
        auto const& pk = keys.first;
        auto const& sk = keys.second;
        auto const account = toBase58(calcAccountID(pk));
        auto const pkHex = strHex(pk.slice());

        // a bare integer amount above 2^32, which the review found unsignable
        auto const raw = paymentJson(account, pkHex, "5000000000");
        auto const sig = jsonSign(pk, sk, raw);
        auto const stx = buildJsonTx(raw, sig);

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
        BEAST_EXPECT(stx->getFieldAmount(sfAmount) == XRPAmount(5000000000));
        BEAST_EXPECT(stx->isTimeSequenced());

        BEAST_EXPECT(
            !stx->checkSign(STTx::RequireFullyCanonicalSig::yes, noRules()));

        auto const bad = [](STTx const& t) {
            return threw([&] { (void)jsontx_verify(t); });
        };

        // Domain separation: a signature over the bare text - what a wallet's
        // "sign message" feature would produce for the same document - does
        // not authorise it.
        {
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfTxnSignature, sign(pk, sk, makeSlice(raw)));
            });
            BEAST_EXPECT(bad(t));
            BEAST_EXPECT(why([&] {
                             (void)jsontx_verify(t);
                         }).find("signature") != std::string::npos);
        }

        // strip the delta
        {
            auto const t = mutate(
                *stx, [](STObject& o) { o.makeFieldAbsent(sfJsonTxDelta); });
            BEAST_EXPECT(bad(t));
            BEAST_EXPECT(
                !t.checkSign(STTx::RequireFullyCanonicalSig::yes, noRules()));
        }

        // a second delta spelling the same preimage
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
            BEAST_EXPECT(why([&] {
                             (void)jsontx_verify(t);
                         }).find("does not match") != std::string::npos);
        }

        // the same fields reformatted
        {
            auto const compact = sanitize_jsontx(raw).first;
            auto const diff = sanitize_jsontx(compact).second;
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfJsonTxDelta, makeSlice(diff));
            });
            BEAST_EXPECT(bad(t));
        }

        // a signature over a different transaction, with its own delta
        {
            auto const other = paymentJson(account, pkHex, "\"999999999\"");
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfTxnSignature, jsonSign(pk, sk, other));
                o.setFieldVL(
                    sfJsonTxDelta, makeSlice(sanitize_jsontx(other).second));
            });
            BEAST_EXPECT(bad(t));
        }

        // any change to a signed field breaks the binding
        for (auto const& change : std::vector<std::function<void(STObject&)>>{
                 [](STObject& o) {
                     o.setFieldAmount(sfAmount, STAmount(XRPAmount(2000000)));
                 },
                 [](STObject& o) {
                     o.setFieldU64(sfTime, o.getFieldU64(sfTime) + 1);
                 },
                 [](STObject& o) { o.setFieldU32(sfSequence, 1); },
                 [](STObject& o) { o.setFieldU32(sfNetworkID, 21338); },
                 [](STObject& o) { o.setFieldU32(sfSourceTag, 1); },
                 [](STObject& o) { o.makeFieldAbsent(sfTime); },
                 [](STObject& o) {
                     o.setFieldArray(sfMemos, STArray(sfMemos));
                 }})
        {
            BEAST_EXPECT(bad(mutate(*stx, change)));
        }

        // A key swap: a third party re-signs the captured preimage with their
        // own key. SigningPubKey is inside the preimage, so it cannot match.
        {
            // A named pair, not a structured binding: clang before 16 cannot
            // capture a binding in a lambda.
            auto const mallory =
                generateKeyPair(KeyType::ed25519, generateSeed("mallory"));
            auto const& pk2 = mallory.first;
            auto const& sk2 = mallory.second;
            auto const t = mutate(*stx, [&](STObject& o) {
                o.setFieldVL(sfSigningPubKey, pk2.slice());
                o.setFieldVL(sfTxnSignature, jsonSign(pk2, sk2, raw));
            });
            BEAST_EXPECT(bad(t));
        }

        // multi-signing has no single preimage to bind
        {
            auto const t = mutate(*stx, [](STObject& o) {
                o.setFieldArray(sfSigners, STArray{sfSigners});
            });
            BEAST_EXPECT(bad(t));
        }

        // only ed25519 signs a JsonTx
        {
            auto const [spk, ssk] =
                generateKeyPair(KeyType::secp256k1, generateSeed("jsontx"));
            auto const sraw = paymentJson(
                toBase58(calcAccountID(spk)), strHex(spk.slice()), "1000000");
            auto const t = buildJsonTx(sraw, jsonSign(spk, ssk, sraw));
            BEAST_EXPECT(bad(*t));
        }

        // a malleated ed25519 signature (S + L) is not canonical and is
        // refused, so a relay cannot mint a second id from one signature
        {
            static constexpr std::uint8_t L[32] = {
                0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
                0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
            Blob s(sig.data(), sig.data() + sig.size());
            unsigned carry = 0;
            for (int i = 0; i < 32; ++i)
            {
                unsigned const v = s[32 + i] + L[i] + carry;
                s[32 + i] = static_cast<std::uint8_t>(v);
                carry = v >> 8;
            }
            auto const t = mutate(
                *stx, [&](STObject& o) { o.setFieldVL(sfTxnSignature, s); });
            BEAST_EXPECT(t.getTransactionID() != stx->getTransactionID());
            BEAST_EXPECT(bad(t));
        }

        // The same payment with Sequence left out. It signs, submits and
        // verifies as itself; both spellings describe one transaction, but a
        // signature over one never authorises the other.
        {
            std::string omitted = raw;
            auto const line = std::string("  \"Sequence\": 0,\n");
            omitted.erase(omitted.find(line), line.size());
            BEAST_EXPECT(omitted.find("Sequence") == std::string::npos);

            auto const otx = buildJsonTx(omitted, jsonSign(pk, sk, omitted));
            BEAST_EXPECT(jsontx_verify(*otx) == omitted);
            BEAST_EXPECT(otx->getFieldU32(sfSequence) == 0);
            BEAST_EXPECT(otx->isTimeSequenced());
            BEAST_EXPECT(otx->getSigningHash() == stx->getSigningHash());
            BEAST_EXPECT(otx->getTransactionID() != stx->getTransactionID());

            // omitted text's signature with the spelled text's delta
            BEAST_EXPECT(bad(mutate(*otx, [&](STObject& o) {
                o.setFieldVL(
                    sfJsonTxDelta, makeSlice(sanitize_jsontx(raw).second));
            })));
            // and the reverse
            BEAST_EXPECT(bad(mutate(*stx, [&](STObject& o) {
                o.setFieldVL(
                    sfJsonTxDelta, makeSlice(sanitize_jsontx(omitted).second));
            })));
        }

        // no delta at all
        BEAST_EXPECT(bad(mutate(
            *stx, [](STObject& o) { o.makeFieldAbsent(sfJsonTxDelta); })));
    }

    void
    testBinarySignatureWithDelta()
    {
        testcase("binary signature never authorises a delta");

        auto const [pk, sk] =
            generateKeyPair(KeyType::ed25519, generateSeed("binary"));
        auto const raw =
            paymentJson(toBase58(calcAccountID(pk)), strHex(pk.slice()), "5");

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

        auto const withDelta = mutate(tx, [](STObject& o) {
            o.setFieldVL(sfJsonTxDelta, Slice("\x01\x00\x04", 3));
        });
        BEAST_EXPECT(withDelta.getSigningHash() == tx.getSigningHash());
        BEAST_EXPECT(withDelta.getTransactionID() != tx.getTransactionID());
        BEAST_EXPECT(!withDelta.checkSign(
            STTx::RequireFullyCanonicalSig::yes, noRules()));
        BEAST_EXPECT(threw([&] { (void)jsontx_verify(withDelta); }));
    }

    void
    testTimeSequenced()
    {
        testcase("STTx::isTimeSequenced");

        auto make = [](std::uint32_t seq,
                       std::optional<std::uint32_t> ticket,
                       std::optional<std::uint64_t> time) {
            STObject o(sfTransaction);
            o.setFieldU16(sfTransactionType, ttACCOUNT_SET);
            o.setAccountID(sfAccount, AccountID(1));
            o.setFieldAmount(sfFee, STAmount(XRPAmount(10)));
            o.setFieldVL(sfSigningPubKey, Slice{});
            o.setFieldU32(sfSequence, seq);
            if (ticket)
                o.setFieldU32(sfTicketSequence, *ticket);
            if (time)
                o.setFieldU64(sfTime, *time);
            return STTx(std::move(o));
        };

        BEAST_EXPECT(make(0, std::nullopt, 5).isTimeSequenced());
        BEAST_EXPECT(make(0, std::nullopt, 0).isTimeSequenced());
        BEAST_EXPECT(!make(1, std::nullopt, 5).isTimeSequenced());
        BEAST_EXPECT(!make(0, 3, 5).isTimeSequenced());
        BEAST_EXPECT(!make(0, std::nullopt, std::nullopt).isTimeSequenced());
        BEAST_EXPECT(!make(0, 3, std::nullopt).isTimeSequenced());
    }

    //--------------------------------------------------------------------------
    // Every field of every transaction format
    //
    // jsontx_verify derives its canonical form from getJson, and the submit
    // RPC derives the transaction from the canonical form through
    // STParsedJSON. For a JsonTx to be signable at all, and for it to keep
    // verifying, each field type must render to a canonical fixed point that
    // parses back to identical binary. This fills every field of every
    // format with a representative value and checks exactly that, so a
    // change to any type's JSON rendering - the consensus surface the header
    // describes - fails here first.

    struct Filler
    {
        AccountID a1, a2;
        int depth = 0;
        std::vector<std::string> unfilled{};

        SField const*
        elementOf(SField const& array) const
        {
            // STArray elements are inner objects named after the array
            // (Memos -> Memo), with a few irregular plurals
            static std::map<std::string, std::string> const irregular{
                {"SignerEntries", "SignerEntry"},
                {"PriceDataSeries", "PriceData"},
                {"AcceptedCredentials", "Credential"},
                {"AuthAccounts", "AuthAccount"}};
            std::string name = array.fieldName;
            if (auto const i = irregular.find(name); i != irregular.end())
                name = i->second;
            else if (!name.empty() && name.back() == 's')
                name.pop_back();
            auto const& f = SField::getField(name);
            if (f == sfInvalid || f.fieldType != STI_OBJECT)
                return nullptr;
            return &f;
        }

        void
        fillField(STObject& o, SField const& f)
        {
            switch (f.fieldType)
            {
                case STI_UINT8:
                    o.set(STUInt8(f, 3));
                    return;
                case STI_UINT16:
                    o.set(STUInt16(f, 7));
                    return;
                case STI_UINT32:
                    o.set(STUInt32(f, f == sfSequence ? 0 : 5));
                    return;
                case STI_UINT64:
                    o.set(STUInt64(
                        f,
                        f == sfTime ? jsontx_iso("2026-09-28T00:00:00.000Z")
                                    : 0x0123456789abcdefull));
                    return;
                case STI_UINT128:
                    o.set(STUInt128(
                        f, uint128{"00112233445566778899AABBCCDDEEFF"}));
                    return;
                case STI_UINT160:
                    o.set(STUInt160(
                        f,
                        uint160{"00112233445566778899AABBCCDDEEFF00112233"}));
                    return;
                case STI_UINT192:
                    o.set(STUInt192(
                        f,
                        uint192{"00112233445566778899AABBCCDDEEFF00112233445566"
                                "77"}));
                    return;
                case STI_UINT256:
                    o.set(STUInt256(
                        f,
                        uint256{"00112233445566778899AABBCCDDEEFF00112233445566"
                                "778899AABBCCDDEEFF"}));
                    return;
                case STI_AMOUNT:
                    if (f == sfFee || f.fieldCode % 2 == 0)
                        o.set(STAmount(f, XRPAmount(12)));
                    else
                        o.set(
                            STAmount(f, Issue(to_currency("USD"), a2), 15, -1));
                    return;
                case STI_VL:
                    o.set(STBlob(f, "\xDE\xAD", 2));
                    return;
                case STI_ACCOUNT:
                    o.set(STAccount(f, f.fieldCode % 2 ? a1 : a2));
                    return;
                case STI_NUMBER:
                    o.set(STNumber(f, Number(12345, -2)));
                    return;
                case STI_OBJECT: {
                    auto const* t = InnerObjectFormats::getInstance()
                                        .findSOTemplateBySField(f);
                    if (!t || depth > 2)
                    {
                        // no inner template (sfManifest): its members are
                        // checked elsewhere, so an empty object will do
                        o.set(STObject(f));
                        return;
                    }
                    STObject inner(*t, f);
                    ++depth;
                    fill(inner, *t);
                    --depth;
                    o.set(std::move(inner));
                    return;
                }
                case STI_ARRAY: {
                    STArray arr(f);
                    if (auto const* el = elementOf(f); el && depth <= 2)
                        if (auto const* t = InnerObjectFormats::getInstance()
                                                .findSOTemplateBySField(*el))
                        {
                            STObject inner(*t, *el);
                            ++depth;
                            fill(inner, *t);
                            --depth;
                            arr.push_back(std::move(inner));
                        }
                    o.set(std::move(arr));
                    return;
                }
                case STI_PATHSET: {
                    STPathSet ps(f);
                    ps.push_back(STPath(
                        {STPathElement(a1, std::nullopt, std::nullopt),
                         STPathElement(std::nullopt, to_currency("USD"), a2)}));
                    o.set(std::move(ps));
                    return;
                }
                case STI_VECTOR256:
                    o.set(STVector256(
                        f, {uint256{1}, uint256{~std::uint64_t{0}}}));
                    return;
                case STI_ISSUE:
                    o.set(STIssue(f, Issue(to_currency("USD"), a2)));
                    return;
                case STI_XCHAIN_BRIDGE:
                    o.set(STXChainBridge(a1, xrpIssue(), a2, xrpIssue()));
                    return;
                case STI_CURRENCY:
                    o.set(STCurrency(f, to_currency("USD")));
                    return;
                default:
                    break;
            }
            unfilled.push_back(f.fieldName);
        }

        void
        fill(STObject& o, SOTemplate const& t)
        {
            for (auto const& e : t)
            {
                auto const& f = e.sField();
                // added by the signer or the submit RPC, never in a preimage
                if (f == sfTxnSignature || f == sfSigners ||
                    f == sfJsonTxDelta || f == sfTransactionType ||
                    f == sfSigningPubKey)
                    continue;
                fillField(o, f);
            }
        }
    };

    void
    testEveryFormat()
    {
        testcase("every field of every transaction format");

        auto const [pk, sk] =
            generateKeyPair(KeyType::ed25519, generateSeed("formats"));
        Filler filler{
            calcAccountID(pk),
            calcAccountID(
                generateKeyPair(KeyType::ed25519, generateSeed("other"))
                    .first)};

        std::size_t formats = 0, verified = 0;
        for (auto const& item : TxFormats::getInstance())
        {
            ++formats;
            std::string const name = item.getName();

            STObject obj(item.getSOTemplate(), sfTransaction);
            obj.setFieldU16(sfTransactionType, item.getType());
            filler.fill(obj, item.getSOTemplate());
            obj.setFieldVL(sfSigningPubKey, pk.slice());

            std::optional<STTx> tx;
            if (auto const e = why([&] { tx.emplace(STObject(obj)); });
                !e.empty())
            {
                fail(name + ": cannot build: " + e);
                continue;
            }

            auto txj = tx->STObject::getJson(JsonOptions::none);
            txj[sfTime.fieldName] = jsontx_iso_str(tx->getFieldU64(sfTime));
            auto const text = Json::FastWriter{}.write(txj);

            // (a) the node can canonicalize its own rendering
            std::string can;
            if (auto const e = why([&] { can = jsontx_canonical(text); });
                !e.empty())
            {
                fail(name + ": canonical form: " + e);
                continue;
            }

            // (b) which is a fixed point
            expect(jsontx_canonical(can) == can, name + ": not a fixed point");

            // (c) and parses back to identical binary, as doSubmit does
            Json::Value jv;
            expect(Json::Reader{}.parse(can, jv), name + ": reparse");
            auto const ms = jsontx_iso(jv[sfTime.fieldName].asString());
            jv.removeMember(sfTime.fieldName);
            STParsedJSONObject parsed("tx_json", jv);
            if (!parsed.object)
            {
                fail(
                    name + ": STParsedJSON: " +
                    parsed.error[jss::error_message].asString());
                continue;
            }
            parsed.object->setFieldU64(sfTime, ms);
            std::optional<STTx> back;
            if (auto const e =
                    why([&] { back.emplace(std::move(*parsed.object)); });
                !e.empty())
            {
                fail(name + ": rebuild: " + e);
                continue;
            }
            expect(
                back->getSerializer().peekData() ==
                    tx->getSerializer().peekData(),
                name + ": does not round-trip to the same binary");

            // (d) jsontx_verify's size pre-check assumes binary is never
            // more than twice the canonical text, with another 2x in hand
            expect(
                tx->getSerializer().size() <= 2 * can.size(),
                name + ": binary larger than twice its canonical form");

            // (e) and it signs and verifies, pretty-printed
            auto const raw = Json::StyledWriter{}.write(txj);
            if (sanitize_jsontx(raw).second.size() <= jsontx_max_diff)
            {
                ++verified;
                auto const stx = buildJsonTx(raw, jsonSign(pk, sk, raw));
                expect(
                    why([&] {
                        expect(jsontx_verify(*stx) == raw, name + ": verify");
                    }).empty(),
                    name + ": verify threw");
            }
        }

        BEAST_EXPECT(formats > 40);
        // StyledWriter's indentation fits the delta for all but the very
        // largest formats
        expect(
            verified * 10 >= formats * 9,
            std::to_string(verified) + " of " + std::to_string(formats) +
                " formats signed and verified");
        log << verified << " of " << formats << " formats signed+verified\n";
        expect(
            filler.unfilled.empty(), "no generator for: " + [&] {
                std::string s;
                for (auto const& n : filler.unfilled)
                    s += n + " ";
                return s;
            }());
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
        testImpliedSequence();
        testEncoderPinned();
        testPaths();
        testVerify();
        testBinarySignatureWithDelta();
        testTimeSequenced();
        testEveryFormat();
    }
};

BEAST_DEFINE_TESTSUITE(JSONTxSignatures, protocol, ripple);

}  // namespace ripple
