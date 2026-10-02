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

#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STTx.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ripple {

//------------------------------------------------------------------------------
// jsontx: plaintext-JSON signing support (featureJsonTx)
//
// A JsonTx is authorised by an ed25519 signature over
//
//     jsontx_sign_prefix || preimage
//
// where the preimage is the exact JSON text the signer read. The node rebuilds
// the preimage from the binary transaction plus sfJsonTxDelta, a small delta
// against a canonical form the node derives itself. The delta is
// attacker-controlled: it is not covered by the signature it helps
// reconstruct. Every bound below is therefore explicit, and unsanitize_jsontx
// accepts only the exact encoding sanitize_jsontx would have produced.
//
// CONSENSUS SURFACE. Once featureJsonTx activates, everything that decides
// whether a preimage verifies is a consensus rule and can only change behind
// a further amendment. That is:
//
//   - every constant and every function in this file, including the delta
//     encoder's exact output (jsontx_verify compares it byte for byte);
//   - STObject::getJson(JsonOptions::none) and the getJson of every ST type
//     that can appear in a transaction (STAmount, STUInt64, STPathSet,
//     STIssue, STVector256, ...), because the node's canonical form is
//     derived from it;
//   - Json::FastWriter, which serializes that JSON before canonicalization;
//   - STParsedJSON, only in so far as the submit RPC builds the transaction
//     from the canonical form: a change there changes which preimages can be
//     submitted, not which verify.
//
// None of that code was consensus-critical before. A change to how any field
// renders as JSON - including one merged from upstream rippled - changes
// which signatures verify and must be amendment-gated. JSONTxSignatures_test
// pins this for every field of every transaction format.
//------------------------------------------------------------------------------

inline constexpr std::size_t jsontx_max_text = 8192;  // canonical and original
inline constexpr std::size_t jsontx_max_diff = 2048;  // delta bytes
inline constexpr std::size_t jsontx_max_ops =
    jsontx_max_diff / 2;                             // delta instructions
inline constexpr std::size_t jsontx_min_copy = 4;    // encoder match threshold
inline constexpr std::size_t jsontx_max_cand = 64;   // encoder candidate cap
inline constexpr std::size_t jsontx_max_depth = 32;  // JSON nesting

// The largest magnitude a bare JSON integer may have: 2^53 - 1, the range
// RFC 8259 section 6 calls interoperable and RFC 7493 (I-JSON) section 2.2
// requires. Past it an IEEE 754 double - which is what JSON.parse and most
// other parsers produce - can no longer hold every integer, so the number a
// wallet shows after parsing the preimage may not be the number that
// executes. Larger values are written as strings, which is how getJson
// renders every amount and UInt64 anyway.
inline constexpr std::uint64_t jsontx_max_int = (std::uint64_t{1} << 53) - 1;

// Domain separation. Without it the signed message is bare JSON text, and any
// wallet feature that signs a user-visible text message with the account key
// (several do, over the raw bytes) could be driven to sign a live Payment
// presented as a "log in" message. 0xFF can never occur in UTF-8, so no text
// signer can produce these bytes; the rest follows HashPrefix convention.
inline constexpr std::string_view jsontx_sign_prefix{
    "\xFF"
    "JTX",
    4};

// The exact message a JsonTx signer signs for `preimage`.
std::string
jsontx_signing_data(std::string_view preimage);

// ASCII-only case folding; never consults the locale.
std::string
jsontx_lower(std::string_view s);

// Case-insensitive field-name -> canonical SField, over the serializable
// fields doServerDefinitions publishes. sfInvalid if unknown.
SField const&
jsontx_field(std::string const& name);

// days from 1970-01-01 (Howard Hinnant's civil calendar algorithm)
constexpr std::int64_t
jsontx_days(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    std::int64_t const era = (y >= 0 ? y : y - 399) / 400;
    unsigned const yoe = static_cast<unsigned>(y - era * 400);
    unsigned const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

inline constexpr std::int64_t jsontx_epoch_day = jsontx_days(2000, 1, 1);
static_assert(jsontx_epoch_day == 10957);  // matches chrono.h epoch_offset

// 9999-12-31T23:59:59.999Z: past this toISOString() switches to expanded years
// and the fixed 24 character shape no longer holds
inline constexpr std::uint64_t jsontx_max_time =
    (static_cast<std::uint64_t>(jsontx_days(9999, 12, 31) - jsontx_epoch_day) *
         86400 +
     86399) *
        1000 +
    999;

// Strict Date().toISOString() -> milliseconds since the ripple epoch.
std::uint64_t
jsontx_iso(std::string_view s);

// The exact inverse over [0, jsontx_max_time].
std::string
jsontx_iso_str(std::uint64_t ms);

// How STUInt64::getJson spells v for field f (hex, or base ten for
// sMD_BaseTen fields).
std::string
jsontx_u64_str(SField const& f, std::uint64_t v);

// Validates that `raw` is a JsonTx document: exactly one JSON object, nothing
// before or after it but whitespace, printable ASCII only, no escapes, no
// comments, no booleans or nulls, no duplicate keys, integers spelled
// -?(0|[1-9][0-9]*), never -0, and no larger in magnitude than
// jsontx_max_int, nesting at most jsontx_max_depth. Throws.
void
jsontx_strict(std::string_view raw);

// The canonical form of `raw` alone: whitespace stripped, field names in
// their Xahau spelling, members ordered by field code, numbers formatted per
// field type, and - at the root only - an omitted Sequence written as
// "Sequence":0 when the document has a Time or a TicketSequence. Throws on
// anything it cannot canonicalize.
std::string
jsontx_canonical(std::string_view raw);

// Returns { canonical, delta } where applying delta to canonical with
// unsanitize_jsontx reproduces `raw` byte for byte. Throws.
std::pair<std::string, std::string>
sanitize_jsontx(std::string_view raw);

// Applies an UNTRUSTED delta to a canonical form the node derived itself.
std::string
unsanitize_jsontx(std::string_view sanitized, std::string_view diff);

// The complete untrusted-side check, shared by the submit RPC and the
// relay/consensus path. Returns the reconstructed preimage or throws.
std::string
jsontx_verify(STTx const& stx, std::string_view diff);

// As above with the delta taken from sfJsonTxDelta. This is what
// checkValidity calls in place of STTx::checkSign for a transaction
// carrying a delta.
std::string
jsontx_verify(STTx const& stx);

}  // namespace ripple
#endif
