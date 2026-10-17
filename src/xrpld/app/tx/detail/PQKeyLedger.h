//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
    Copyright (c) 2026 XRPL Labs

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

#ifndef RIPPLE_TX_PQKEYLEDGER_H_INCLUDED
#define RIPPLE_TX_PQKEYLEDGER_H_INCLUDED

#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/basics/Blob.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/XRPAmount.h>

#include <optional>
#include <vector>

namespace ripple {

/** The ltPUBLIC_KEY ledger object (featurePQSignatures).

    Every function here is a no-op unless featurePQSignatures is enabled in
    the view's rules, so callers in consensus code need no extra gating.

    Reference counting follows ltHOOK_DEFINITION: it is a garbage-collection
    hint, not something correctness depends on. The count is incremented
    when a slot that names the key (AccountRoot master key, RegularKey,
    SignerEntry) is written while the object exists and decremented when
    such a slot is cleared while it exists; the object is deleted at zero.
    A slot written *before* the key was revealed is simply not counted, and
    the worst case for a stale reference is that the key must be revealed
    inline again (paying the byte fee again). Verification of a reference
    always requires the object to be present, so a stale count can never
    make a bad signature pass.
*/
namespace pqkey {

/// The ltPUBLIC_KEY object for the key whose AccountID is `id`, if any
std::shared_ptr<SLE const>
read(ReadView const& view, AccountID const& id);

/// The inline (0xF0 | type | key) encoding stored for `id`, if any
std::optional<Blob>
readInlineKey(ReadView const& view, AccountID const& id);

/** A lookup for STTx::checkSign that resolves references against `view`.
    The view must outlive the returned function. */
STTx::PQKeyLookup
makeLookup(ReadView const& view);

/** Bytes of inline PQ keys in `tx` that `view` does not yet hold an
    object for. The publication charge is one drop per byte. */
std::uint64_t
unrevealedKeyBytes(ReadView const& view, STTx const& tx);

/** Create an ltPUBLIC_KEY object (ReferenceCount = 1) for every inline PQ
    key in `tx` that the view does not yet hold. Called by Transactor after
    the fee is charged, on the tes and the tec path alike. */
TER
reveal(ApplyView& view, STTx const& tx, beast::Journal j);

/** If `view` holds a key object for `id`, increment its reference count. */
void
addReference(ApplyView& view, AccountID const& id, beast::Journal j);

/** If `view` holds a key object for `id`, decrement its reference count,
    deleting the object when it reaches zero. */
void
removeReference(ApplyView& view, AccountID const& id, beast::Journal j);

/** Net reference adjustment when a set of referencing slots changes from
    `before` to `after` (e.g. a SignerList replacement): ids only in
    `before` lose a reference, ids only in `after` gain one. Both vectors
    may be unsorted and may contain duplicates. */
void
adjustReferences(
    ApplyView& view,
    std::vector<AccountID> before,
    std::vector<AccountID> after,
    beast::Journal j);

}  // namespace pqkey
}  // namespace ripple

#endif
