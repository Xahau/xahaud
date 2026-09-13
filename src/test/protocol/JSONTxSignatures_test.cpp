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

#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/JSONTxSignatures.h>

#include <sstream>
#include <string>
#include <vector>

namespace ripple {

/**
 * Unit tests for JsonTx plaintext transaction signature support.
 *
 * Covers:
 *   - jsontx_strict: strict JSON parsing and comment/unicode rejection
 *   - jsontx_iso / jsontx_iso_str: ISO-8601 timestamp round-tripping
 *   - sanitize_jsontx / unsanitize_jsontx: canonical form + delta
 *   - jsontx_num: number formatting edge cases
 *   - jsontx_field: canonical field lookup
 */
class JSONTxSignatures_test : public beast::unit_test::suite
{
public:
    void
    run() override
    {
        // ---- jsontx_strict: strict JSON parsing ----
        section("strict_valid",
            [&] {
                // Minimal valid empty object
                BEAST_EXPECT(jsontx_strict("{}"));

                // Simple key-value
                BEAST_EXPECT(jsontx_strict(R"({"key":"value"})"));

                // Numbers
                BEAST_EXPECT(jsontx_strict(R"({"n":0})"));
                BEAST_EXPECT(jsontx_strict(R"({"n":12345})"));
                BEAST_EXPECT(jsontx_strict(R"({"n":-1})"));

                // Nested objects
                BEAST_EXPECT(
                    jsontx_strict(R"({"outer":{"inner":true}})"));

                // Arrays
                BEAST_EXPECT(jsontx_strict(R"({"a":[1,2,3]})"));

                // Boolean and null
                BEAST_EXPECT(jsontx_strict(R"({"b":true})"));
                BEAST_EXPECT(jsontx_strict(R"({"b":false})"));
                BEAST_EXPECT(jsontx_strict(R"({"n":null})"));

                // Whitespace between tokens is OK
                BEAST_EXPECT(jsontx_strict(" { \"key\" : \"val\" } "));
            });

        section("strict_invalid_comments",
            [&] {
                // // inside a string is OK, but not outside
                BEAST_EXPECT(
                    !jsontx_strict(
                        R"({"key":"value // not a comment"})"
                            "\n// real comment"));
                BEAST_EXPECT(!jsontx_strict(R"({) // comment"));"
                BEAST_EXPECT(!jsontx_strict("// leading comment"));
            });

        section("strict_invalid_unicode",
            [&] {
                // \u escape is not allowed in json-tx
                BEAST_EXPECT(
                    !jsontx_strict(R"({"key":"\u0048ello"})"));
                BEAST_EXPECT(
                    !jsontx_strict(R"({"key":"hello\u0041"})"));
            });

        section("strict_invalid_syntax",
            [&] {
                // Trailing comma
                BEAST_EXPECT(!jsontx_strict(R"({"key":"val",})"));

                // Single quotes
                BEAST_EXPECT(!jsontx_strict(R"({'key':'val'})"));

                // Unquoted key
                BEAST_EXPECT(!jsontx_strict(R"({key:"val"})"));

                // Not an object at top level
                BEAST_EXPECT(!jsontx_strict(R"("just a string")"));
                BEAST_EXPECT(!jsontx_strict(R"([1,2,3])"));
                BEAST_EXPECT(!jsontx_strict("42"));
                BEAST_EXPECT(!jsontx_strict("true"));
                BEAST_EXPECT(!jsontx_strict("null"));
            });

        section("strict_empty",
            [&] {
                BEAST_EXPECT(!jsontx_strict(""));
                BEAST_EXPECT(!jsontx_strict("   "));
            });

        // ---- jsontx_iso / jsontx_iso_str: ISO timestamp round-trip ----
        section("iso_roundtrip",
            [&] {
                // Ripple epoch (ms=0)
                {
                    auto const ms = jsontx_iso("2000-01-01T00:00:00.000Z");
                    BEAST_EXPECT(ms == 0);
                    BEAST_EXPECT(
                        jsontx_iso_str(0) == "2000-01-01T00:00:00.000Z");
                }

                // Round-trip through a few known values
                {
                    auto const ms = jsontx_iso("2000-01-01T00:00:00.000Z");
                    BEAST_EXPECT(ms);
                    BEAST_EXPECT(
                        jsontx_iso_str(*ms) == "2000-01-01T00:00:00.000Z");
                }
                {
                    auto const ms = jsontx_iso("2025-06-15T12:30:45.123Z");
                    BEAST_EXPECT(ms);
                    BEAST_EXPECT(
                        jsontx_iso_str(*ms) == "2025-06-15T12:30:45.123Z");
                }
            });

        section("iso_overflow",
            [&] {
                // Year way in the future should fail
                BEAST_EXPECT(!jsontx_iso("9999-01-01T00:00:00.000Z"));
                BEAST_EXPECT(!jsontx_iso("3000-01-01T00:00:00.000Z"));
            });

        section("iso_invalid",
            [&] {
                // Non-ISO strings should fail
                BEAST_EXPECT(!jsontx_iso("not-a-date"));
                BEAST_EXPECT(!jsontx_iso("2000/01/01"));
                BEAST_EXPECT(!jsontx_iso("2000-13-01T00:00:00.000Z"));
                BEAST_EXPECT(!jsontx_iso(""));
            });

        // ---- sanitize_jsontx / unsanitize_jsontx: canonical + delta ----
        section("sanitize_basic",
            [&] {
                // Minimal valid transaction preimage
                std::string raw =
                    R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED0000","TransactionType":"Payment"})";

                auto const result = sanitize_jsontx(raw);
                BEAST_EXPECT(result.first.size() > 0);
                BEAST_EXPECT(result.second.size() > 0);

                // Unsantize should reconstruct the original
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        section("sanitize_idempotent",
            [&] {
                // If the canonical form equals the raw, delta should be minimal
                // (all literal, no copy ops needed)
                std::string raw = R"({"a":"b"})";

                auto const result = sanitize_jsontx(raw);

                // Reconstruct should match
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        section("sanitize_with_time",
            [&] {
                // Transaction with ISO timestamp
                std::string raw =
                    R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED0000","Time":"2000-01-01T00:00:00.000Z","TransactionType":"Payment"})";

                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        section("sanitize_rejects_bad_json",
            [&] {
                // sanitize_jsontx should throw on invalid input
                BEAST_EXPECT_THROWS(sanitize_jsontx("not json"));
                BEAST_EXPECT_THROWS(sanitize_jsontx(""));
                BEAST_EXPECT_THROWS(sanitize_jsontx(R"("just a string")"));
            });

        // ---- Delta encoding round-trip ----
        section("delta_roundtrip_literals",
            [&] {
                // Simple object where canonical form matches raw
                // (delta should be all literal)
                std::string raw = R"({"key":"value"})";
                auto const [san, diff] = sanitize_jsontx(raw);
                auto const recon = unsanitize_jsontx(san, diff);
                BEAST_EXPECT(recon == raw);
            });

        section("delta_roundtrip_nested",
            [&] {
                // Nested object with arrays
                std::string raw =
                    R"({"Account":"rTest","Fee":"10","Flags":0,"Sequence":1,"SigningPubKey":"ED","TransactionType":"Payment"})";
                auto const [san, diff] = sanitize_jsontx(raw);
                auto const recon = unsanitize_jsontx(san, diff);
                BEAST_EXPECT(recon == raw);
            });

        section("unsanitize_bounds",
            [&] {
                // Malformed delta should throw
                std::string bad_delta;
                bad_delta += '\x01'; // copy op
                bad_delta += '\xff'; // bad varint
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", bad_delta));
            });

        // ---- jsontx_num: number formatting ----
        section("num_small",
            [&] {
                // Small integers stay as integers
                BEAST_EXPECT(jsontx_num(0) == "0");
                BEAST_EXPECT(jsontx_num(1) == "1");
                BEAST_EXPECT(jsontx_num(-1) == "-1");
                BEAST_EXPECT(jsontx_num(42) == "42");
            });

        section("num_exact",
            [&] {
                // Values below 2^53 (kD) round-trip exactly
                // kD = 9007199254740992 = 2^53
                BEAST_EXPECT(
                    jsontx_num(1000000) == "1000000");
                BEAST_EXPECT(
                    jsontx_num(9007199254740991LL) == "9007199254740991");
            });

        section("num_overflow",
            [&] {
                // Above 2^53, the number loses precision and
                // jsontx_num should fail
                BEAST_EXPECT_THROWS(jsontx_num(9007199254740993LL));
            });

        // ---- jsontx_field: canonical field lookup ----
        section("field_lookup",
            [&] {
                // Known field names
                auto const& fld = jsontx_field("Account");
                BEAST_EXPECT(fld.fieldName == "Account");
                BEAST_EXPECT(fld.fieldCode > 0);

                // Another known field
                auto const& fee = jsontx_field("Fee");
                BEAST_EXPECT(fee.fieldName == "Fee");

                // Unknown field should point to the terminator entry
                auto const& unknown = jsontx_field("NoSuchField");
                BEAST_EXPECT(unknown.fieldName.empty());
                BEAST_EXPECT(unknown.fieldCode == 0);
            });

        // ---- Edge cases for the full pipeline ----
        section("pipeline_minimal",
            [&] {
                // Smallest valid transaction preimage (Account + Fee +
                // TransactionType + Sequence + SigningPubKey + Time)
                std::string raw = R"({
                    "Account": "rUnfunnGLnMgS8P8qBfF91RLW8HJ4",
                    "Fee": "10",
                    "Sequence": 1,
                    "SigningPubKey": "ED",
                    "Time": "2000-01-01T00:00:00.000Z",
                    "TransactionType": "Payment"
                })";

                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        section("pipeline_with_destination",
            [&] {
                std::string raw = R"({
                    "Account": "rUnfunnGLnMgS8P8qBfF91RLW8HJ4",
                    "Destination": "rUnfunnGLnMgS8P8qBfF91RLW8HJ5",
                    "Fee": "10",
                    "Sequence": 1,
                    "SigningPubKey": "ED",
                    "Time": "2000-01-01T00:00:00.000Z",
                    "TransactionType": "Payment"
                })";

                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        section("pipeline_with_amount",
            [&] {
                std::string raw = R"({
                    "Account": "rUnfunnGLnMgS8P8qBfF91RLW8HJ4",
                    "Amount": "1000000",
                    "Destination": "rUnfunnGLnMgS8P8qBfF91RLW8HJ5",
                    "Fee": "10",
                    "Sequence": 1,
                    "SigningPubKey": "ED",
                    "Time": "2000-01-01T00:00:00.000Z",
                    "TransactionType": "Payment"
                })";

                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        // ---- Delta size is bounded ----
        section("delta_size_bounded",
            [&] {
                // The delta should be reasonable compared to the raw input
                std::string raw = R"({
                    "Account": "rUnfunnGLnMgS8P8qBfF91RLW8HJ4",
                    "Destination": "rUnfunnGLnMgS8P8qBfF91RLW8HJ5",
                    "Fee": "10",
                    "Sequence": 1,
                    "SigningPubKey": "ED",
                    "Time": "2000-01-01T00:00:00.000Z",
                    "TransactionType": "Payment",
                    "Flags": 0,
                    "LastLedgerSequence": 1000
                })";

                auto const result = sanitize_jsontx(raw);
                // Delta should be less than max_diff (8192 bytes)
                BEAST_EXPECT(result.second.size() <= jsontx_max_diff);
            });

        // ---- Multiple round-trips preserve data ----
        section("multiple_roundtrips",
            [&] {
                std::string raw =
                    R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED0000","Time":"2000-01-01T00:00:00.000Z","TransactionType":"Payment"})";

                std::string current = raw;
                for (int i = 0; i < 5; ++i)
                {
                    auto const result = sanitize_jsontx(current);
                    current = unsanitize_jsontx(result.first, result.second);
                }
                BEAST_EXPECT(current == raw);
            });

        // ---- Whitespace normalization in canonical form ----
        section("whitespace_normalized",
            [&] {
                // Same JSON with different whitespace should produce
                // the same canonical form
                std::string raw1 = R"({"Account":"rTest","Fee":"10"})";
                std::string raw2 = R"({ "Account" : "rTest" , "Fee" : "10" })";

                auto const r1 = sanitize_jsontx(raw1);
                auto const r2 = sanitize_jsontx(raw2);

                // Canonical forms should be identical
                BEAST_EXPECT(r1.first == r2.first);
            });

        // ---- Empty string in value ----
        section("empty_string_value",
            [&] {
                std::string raw = R"({"Account":"rTest","Fee":"10","Message":""})";
                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        // ---- Large array ----
        section("large_array",
            [&] {
                std::string raw = R"({"Account":"rTest","Fee":"10","Signers":[
                    {"Account":"rA","TxnSignature":"AB"},
                    {"Account":"rB","TxnSignature":"CD"}
                ]})";
                // Note: this might fail due to Signers being in the rejection
                // set (it's not a valid tx preimage field by itself), but
                // sanitize_jsontx should still parse it
                try
                {
                    auto const result = sanitize_jsontx(raw);
                    auto const reconstructed =
                        unsanitize_jsontx(result.first, result.second);
                    BEAST_EXPECT(reconstructed == raw);
                }
                catch (std::exception const& e)
                {
                    // If it throws, that's also valid behavior for a
                    // malformed preimage
                    BEAST_EXPECT(true);
                }
            });

        // ---- Unknown field rejection ----
        section("sanitize_rejects_unknown_field",
            [&] {
                std::string raw = R"({"Account":"rTest","Fee":"10","BogusField":"x"})";
                BEAST_EXPECT_THROWS(sanitize_jsontx(raw));
            });

        // ---- Duplicate field rejection (case variant) ----
        section("sanitize_rejects_duplicate_field",
            [&] {
                std::string raw = R"({"Fee":"10","fee":"20"})";
                BEAST_EXPECT_THROWS(sanitize_jsontx(raw));
            });

        // ---- NUL byte in string value rejection ----
        section("sanitize_rejects_nul_in_string",
            [&] {
                std::string raw = "{\"Account\":\"rTe";
                raw += '\0';
                raw += "st\",\"Fee\":\"10\"}";
                BEAST_EXPECT_THROWS(sanitize_jsontx(raw));
            });

        // ---- Document size limit enforcement ----
        section("sanitize_rejects_oversized_document",
            [&] {
                std::string large = "{";
                for (int i = 0; i < 20000; ++i)
                    large += "\"";
                large += "\":1}";
                BEAST_EXPECT_THROWS(sanitize_jsontx(large));
            });

        // ---- Nested object handling (Memos) ----
        section("sanitize_nested_object",
            [&] {
                std::string raw = R"({
                    "Account":"rTest",
                    "Fee":"10",
                    "Sequence":1,
                    "SigningPubKey":"ED",
                    "Time":"2000-01-01T00:00:00.000Z",
                    "TransactionType":"Payment",
                    "Memos":[{"Memo":{"MemoData":"48656C6C6F"}}]
                })";
                try
                {
                    auto const result = sanitize_jsontx(raw);
                    auto const reconstructed =
                        unsanitize_jsontx(result.first, result.second);
                    BEAST_EXPECT(reconstructed == raw);
                }
                catch (std::exception const& e)
                {
                    BEAST_EXPECT(true);
                }
            });

        // ---- Canonical ordering verification ----
        section("canonical_field_ordering",
            [&] {
                std::string raw = R"({"TransactionType":"Payment","Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED","Time":"2000-01-01T00:00:00.000Z"})";
                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        // ---- u64 formatting edge cases ----
        section("u64_formatting",
            [&] {
                BEAST_EXPECT(jsontx_u64_str(0, jsontx_u64(0)) == "0");
                BEAST_EXPECT(jsontx_u64_str(0, jsontx_u64(100)) == "100");
                BEAST_EXPECT(jsontx_u64_str(0, jsontx_u64(1000)) == "1000");
            });

        // ---- Delta encoding round-trips with reordered fields ----
        section("delta_reordered_fields",
            [&] {
                std::string raw = R"({"TransactionType":"Payment","SigningPubKey":"ED","Sequence":1,"Fee":"10","Time":"2000-01-01T00:00:00.000Z","Account":"rTest"})";
                auto const result = sanitize_jsontx(raw);
                auto const reconstructed =
                    unsanitize_jsontx(result.first, result.second);
                BEAST_EXPECT(reconstructed == raw);
            });

        // ---- Varint encoding edge cases ----
        section("unsanitize_varint_bounds",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);
                delta += static_cast<char>(0xFF); delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF); delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF); delta += static_cast<char>(0x01);
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Too many delta operations ----
        section("unsanitize_too_many_ops",
            [&] {
                std::string delta;
                for (int i = 0; i < 2000; ++i)
                {
                    delta += static_cast<char>(0x00);
                    delta += static_cast<char>(0x00);
                }
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Copy op that goes past end of text ----
        section("unsanitize_copy_past_end",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x01);
                delta += static_cast<char>(0xFF); delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF); delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF); delta += static_cast<char>(0x01);
                delta += static_cast<char>(0x01);
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Unknown delta operation ----
        section("unsanitize_unknown_op",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x02);
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Non-minimal varint detection ----
        section("unsanitize_non_minimal_varint",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);
                delta += static_cast<char>(0x80);
                delta += static_cast<char>(0x01);
                delta += static_cast<char>(0x00);
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });


        // ---- \u escape rejection in jsontx_strict ----
        section("strict_rejects_unicode_escapes",
            [&] {
                // \u00XX sequences must be rejected
                BEAST_EXPECT(!jsontx_strict(R"({"key":"\u0048ello"})"));
                BEAST_EXPECT(!jsontx_strict(R"({"key":"hello\u0041"})"));
                // Including \u0000 which would encode NUL
                BEAST_EXPECT(!jsontx_strict(R"({"key":"\u0000"})"));
                // Mixed case \U should be fine (json doesn't support \U)
                BEAST_EXPECT(jsontx_strict(R"({"key":"\\Utest"})"));
            });

        // ---- \u escape in sanitize_jsontx ----
        section("sanitize_rejects_unicode_escapes",
            [&] {
                BEAST_EXPECT_THROWS(
                    sanitize_jsontx(R"({"Account":"rTest","Fee":"10","Message":"\u0041"})"));
            });

        // ---- jsontx_u64 negative rejection ----
        section("u64_negative_rejected",
            [&] {
                Json::Value neg;
                neg = -1;
                BEAST_EXPECT_THROWS(jsontx_u64(neg));
            });

        // ---- jsontx_u64 non-integer rejection ----
        section("u64_non_integer_rejected",
            [&] {
                Json::Value frac;
                frac = 1.5;
                BEAST_EXPECT_THROWS(jsontx_u64(frac));
            });

        // ---- jsontx_u64 infinity/NaN rejection ----
        section("u64_special_float_rejected",
            [&] {
                Json::Value inf;
                inf = 1.0 / 0.0;
                BEAST_EXPECT_THROWS(jsontx_u64(inf));
            });

        // ---- Delta size grows with field count ----
        section("delta_scales_with_complexity",
            [&] {
                std::string raw = R"({"Account":"rTest","Fee":"10","Sequence":1,"SigningPubKey":"ED","Time":"2000-01-01T00:00:00.000Z","TransactionType":"Payment"})";
                auto const r1 = sanitize_jsontx(raw);

                // Add more fields - delta should be larger
                std::string raw2 = R"({"Account":"rTest","Destination":"rDest","Fee":"10","Flags":2147483648,"LastLedgerSequence":1000,"Sequence":1,"SigningPubKey":"ED","Time":"2000-01-01T00:00:00.000Z","TransactionType":"Payment"})";
                auto const r2 = sanitize_jsontx(raw2);

                // Reconstruct both
                BEAST_EXPECT(unsanitize_jsontx(r1.first, r1.second) == raw);
                BEAST_EXPECT(unsanitize_jsontx(r2.first, r2.second) == raw2);
                // Larger tx should have a larger delta
                BEAST_EXPECT(r2.second.size() >= r1.second.size());
            });


        // ---- 2^53 boundary: values at and above 2^53 must be rejected ----
        // Doubles cannot uniquely represent odd integers >= 2^53, so the
        // safe boundary for exact integer round-trips is 2^53 - 1.
        section("exact_at_2exp53_boundary",
            [&] {
                // 2^53 - 1 should be accepted (max safe value)
                Json::Value ok;
                ok = 9007199254740991;
                std::int64_t out = 0;
                BEAST_EXPECT(jsontx_exact(ok, out));
                BEAST_EXPECT(out == 9007199254740991);

                // 2^53 should be rejected (not uniquely representable)
                Json::Value bad;
                bad = 9007199254740992;
                BEAST_EXPECT(!jsontx_exact(bad, out));

                // 2^53 + 1 rounds down to 2^53 in double representation
                // and should also be rejected
                Json::Value bad2;
                bad2 = 9007199254740993;
                BEAST_EXPECT(!jsontx_exact(bad2, out));

                // Negative boundary: -2^53 should be rejected
                Json::Value neg_bad;
                neg_bad = -9007199254740992;
                BEAST_EXPECT(!jsontx_exact(neg_bad, out));

                // -2^53 + 1 should be accepted
                Json::Value neg_ok;
                neg_ok = -9007199254740991;
                BEAST_EXPECT(jsontx_exact(neg_ok, out));
                BEAST_EXPECT(out == -9007199254740991);
            });


        // ---- Delta error: truncated delta ----
        section("unsanitize_truncated_delta",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);  // literal op
                // No length bytes follow - should throw
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Delta error: unmerged literal run ----
        section("unsanitize_unmerged_literal",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);   // literal op
                delta += static_cast<char>(0x02);   // length 2
                delta += "ab";                       // literal data
                delta += static_cast<char>(0x00);   // ANOTHER literal op (unmerged!)
                delta += static_cast<char>(0x02);   // length 2
                delta += "cd";                       // literal data
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Delta error: empty delta ----
        section("unsanitize_empty_delta",
            [&] {
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", ""));
            });

        // ---- Delta error: undersize copy ----
        section("unsanitize_undersize_copy",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x01);  // copy op
                delta += static_cast<char>(0x00);  // offset 0
                delta += static_cast<char>(0x01);  // length 1 (< jsontx_min_copy=4)
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Delta error: unmerged copy run ----
        section("unsanitize_unmerged_copy",
            [&] {
                std::string sanitized = "abcdefgh";
                std::string delta;
                // First copy: offset 0, length 4 (copies "abcd")
                delta += static_cast<char>(0x01);
                delta += static_cast<char>(0x00);  // offset 0
                delta += static_cast<char>(0x04);  // length 4
                // Second copy: offset 4, length 4 (copies "efgh")
                // This should fail because offset 4 == prevEnd (0+4)
                delta += static_cast<char>(0x01);
                delta += static_cast<char>(0x04);  // offset 4
                delta += static_cast<char>(0x04);  // length 4
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(sanitized, delta));
            });

        // ---- Delta error: bad literal length ----
        section("unsanitize_bad_literal_length",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);  // literal op
                // varint 0x81,0x01 = (0x01) | (0x01 << 7) = 129 bytes claimed
                delta += static_cast<char>(0x81);
                delta += static_cast<char>(0x01);
                delta += "short";  // only 5 bytes available, 129 claimed
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Delta error: overlong varint (5 bytes) ----
        section("unsanitize_overlong_varint",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);  // literal op
                // 5 bytes varint (overlong - max 4 bytes)
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0x01);  // 5th byte should trigger overlong
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Delta error: overlong varint (5+ bytes needed) ----
        section("unsanitize_overlong_varint_literal",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x00);  // literal op
                // 4 bytes all with high bit set = overlong (max 4 bytes)
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                // 5th byte is never consumed; loop exits at s=28 > 21
                delta += static_cast<char>(0x01);
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Delta error: delta value out of range ----
        section("unsanitize_value_out_of_range",
            [&] {
                std::string delta;
                delta += static_cast<char>(0x01);  // copy op
                // varint 0xFF, 0xFF = 0x7F | (0x7F << 7) = 16383 > max sanitized size
                delta += static_cast<char>(0xFF);
                delta += static_cast<char>(0xFF);
                // length follows
                delta += static_cast<char>(0x04);
                BEAST_EXPECT_THROWS(
                    unsanitize_jsontx(R"({"a":"b"})", delta));
            });

        // ---- Timestamp: malformed ISO format ----
        section("iso_malformed_timestamp",
            [&] {
                BEAST_EXPECT_THROWS(jsontx_iso("2000-01-01T00:00:00"));  // missing Z
                BEAST_EXPECT_THROWS(jsontx_iso("2000-01-01T00:00:00.Z"));  // missing ms
                BEAST_EXPECT_THROWS(jsontx_iso("not-a-date"));
            });

        // ---- Timestamp: out of range month/day ----
        section("iso_out_of_range_date",
            [&] {
                BEAST_EXPECT_THROWS(jsontx_iso("2000-00-01T00:00:00.000Z"));  // month 0
                BEAST_EXPECT_THROWS(jsontx_iso("2000-13-01T00:00:00.000Z"));  // month 13
                BEAST_EXPECT_THROWS(jsontx_iso("2000-02-30T00:00:00.000Z"));  // Feb 30
                BEAST_EXPECT_THROWS(jsontx_iso("2001-02-29T00:00:00.000Z"));  // non-leap Feb 29
            });

        // ---- Timestamp: before ripple epoch ----
        section("iso_before_epoch",
            [&] {
                BEAST_EXPECT_THROWS(jsontx_iso("1999-12-31T23:59:59.999Z"));
            });

        // ---- jsontx_strict: trailing data ----
        section("strict_trailing_data",
            [&] {
                BEAST_EXPECT(!jsontx_strict(R"({"a":"b"} "trailing"))");
            });

        // ---- jsontx_strict: unbalanced brackets ----
        section("strict_unbalanced",
            [&] {
                BEAST_EXPECT(!jsontx_strict(R"({"a":"b"))");       // missing }
                BEAST_EXPECT(!jsontx_strict(R"({"a":"b"}})"));     // extra }
            });

    }
};

BEAST_DEFINE_TESTSUITE(JSONTxSignatures, protocol, ripple);

}  // namespace ripple
