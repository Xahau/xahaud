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

#ifndef RIPPLE_PROTOCOL_CREDITNOTE_H_INCLUDED
#define RIPPLE_PROTOCOL_CREDITNOTE_H_INCLUDED

#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STAmount.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ripple {

//------------------------------------------------------------------------------
// Credit notes (featureCreditNote)
//
// A note is a bearer instrument the issuer signs blind, typically offline:
// "whoever holds this may take Amount from my account until Expiration".
// It is a JsonTx document in the sense of featureJsonTx - the same character
// set, number rules and signing prefix as a plaintext-signed transaction - so
// anything that can sign a JsonTx preimage can sign a note, and the bytes the
// issuer read are the bytes the ledger checks. There is no canonical form and
// no delta: NoteClaim carries the note verbatim.
//
// The document is exactly one object with exactly these six members, in any
// order, spelled exactly so (no case folding - what you see is what you get):
//
//   "NetworkID"   bare integer   the network the note redeems on
//   "Account"     r-address      the issuer; who pays
//   "Amount"      bare integer   drops of the native currency, or
//                 object         {"currency","issuer","value"}, all strings,
//                                an issued currency as a Payment spells it
//   "IssueTime"   ISO 8601       Date().toISOString(), as sfTime renders
//   "Expiration"  ISO 8601       after IssueTime, at most creditNoteMaxLifeMs
//                                later
//   "Bearer"      hex            an ed25519 public key (ED + 64 hex digits,
//                                either case), as PublicKey renders: the
//                                bearer key, see below
//
// TransactionType is not a member, and IssueTime is not a transaction field,
// so no note is a valid transaction preimage and no transaction preimage is
// a valid note: the shared signing prefix needs no further separation.
//
// THE BEARER KEY. For every note the issuer makes up a fresh random ed25519
// key pair: the public half is the note's Bearer member, so the issuer's
// signature covers it, and the secret half is handed to the recipient with
// the note. Whoever holds that secret is the bearer. To redeem, the bearer
// proves it in the NoteClaim itself: sfBearerSignature is a signature by the
// bearer key over
//
//   noteClaimSigningData(note, account)
//     = jsontx_sign_prefix || note bytes || sfAccount (the 20-byte id)
//
// so it is bound to this note and to the account redeeming it (which need
// not be anyone's in particular - the bearer picks it). The note itself can
// therefore travel in the clear: seeing it, or a NoteClaim carrying it, is
// not enough to redeem it. The secret never touches the ledger.
//
// The bearer secret is a 16-byte ed25519 seed in the XRPL convention - the
// private key is the first 32 bytes of SHA-512(seed), exactly what an sEd...
// seed holds - so any XRPL tool, and WebCrypto with one SHA-512, can derive
// the key. The hand-off is one string of four '.'-separated parts, the last
// three fixed-width hex so the recipient splits on the last three dots:
//
//   <note bytes>.<issuer signature hex>.<issuer public key hex>.<seed hex>
//
// Before accepting a note the recipient derives the key from the seed and
// checks it is the note's Bearer: otherwise they cannot redeem it, and only
// a key so derived is known to be a proper one. The ledger does not vet
// the point; a degenerate Bearer the issuer chose is the issuer's to lose.
//
// Replay protection is the issuer's AccountRoot sfLastNoteTime: a note can
// be redeemed only while its IssueTime is strictly greater, and redeeming
// records its IssueTime there. Each note redeems at most once, and redeeming
// one retires every unredeemed note the issuer signed before it.
//
// CONSENSUS SURFACE. Everything in parseCreditNote and noteClaimSigningData
// decides whether a note is valid, and is a consensus rule once
// featureCreditNote activates.
//------------------------------------------------------------------------------

struct CreditNote
{
    std::uint32_t networkID;
    AccountID account;
    STAmount amount;
    std::uint64_t issueTime;   // milliseconds since the ripple epoch
    std::uint64_t expiration;  // milliseconds since the ripple epoch
    PublicKey bearer;          // the bearer key; ed25519
};

/** Parses and validates the exact bytes of a note.

    Accepts only the document described above, after jsontx_strict has
    accepted it as a JsonTx document. Checks the shape, the account and
    currency spellings, that Amount is positive and legal, that Bearer is an
    ed25519 public key, and that the validity window is well formed and no
    longer than creditNoteMaxLifeMs. Everything that needs a ledger - the
    signing key's authority, the window against the close time,
    sfLastNoteTime, funds and trust lines - is left to the NoteClaim
    transactor. Throws std::runtime_error with the reason.
*/
CreditNote
parseCreditNote(std::string_view raw);

/** The bytes the bearer key signs to redeem `note` from `claimant`.

    jsontx_sign_prefix || note || claimant. The note's own signature is over
    the first two parts alone, by a different key; no valid note or JsonTx
    preimage ends in twenty arbitrary bytes, so neither message can be
    passed off as the other.
*/
std::string
noteClaimSigningData(std::string_view note, AccountID const& claimant);

}  // namespace ripple

#endif
