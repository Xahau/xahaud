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
#include <xrpl/protocol/STAmount.h>

#include <cstdint>
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
// The document is exactly one object with exactly these five members, in any
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
//
// TransactionType is not a member, and IssueTime is not a transaction field,
// so no note is a valid transaction preimage and no transaction preimage is
// a valid note: the shared signing prefix needs no further separation.
//
// Replay protection is the issuer's AccountRoot sfLastNoteTime: a note can
// be redeemed only while its IssueTime is strictly greater, and redeeming
// records its IssueTime there. Each note redeems at most once, and redeeming
// one retires every unredeemed note the issuer signed before it.
//
// CONSENSUS SURFACE. Everything in parseCreditNote decides whether a note is
// valid, and is a consensus rule once featureCreditNote activates.
//------------------------------------------------------------------------------

struct CreditNote
{
    std::uint32_t networkID = 0;
    AccountID account;
    STAmount amount;
    std::uint64_t issueTime = 0;   // milliseconds since the ripple epoch
    std::uint64_t expiration = 0;  // milliseconds since the ripple epoch
};

/** Parses and validates the exact bytes of a note.

    Accepts only the document described above, after jsontx_strict has
    accepted it as a JsonTx document. Checks the shape, the account and
    currency spellings, that Amount is positive and legal, and that the
    validity window is well formed and no longer than creditNoteMaxLifeMs.
    Everything that needs a ledger - the signing key's authority, the window
    against the close time, sfLastNoteTime, funds and trust lines - is left to
    the NoteClaim transactor. Throws std::runtime_error with the reason.
*/
CreditNote
parseCreditNote(std::string_view raw);

}  // namespace ripple

#endif
