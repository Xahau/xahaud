//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#ifndef RIPPLE_PROTOCOL_JSONTX_H_INCLUDED
#define RIPPLE_PROTOCOL_JSONTX_H_INCLUDED

#include <xrpl/basics/Blob.h>
#include <xrpl/basics/Expected.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/protocol/STTx.h>

namespace ripple {
namespace jsonTx {

/** Returns true iff the STObject carries a non-empty sfJsonTxBody.

    A transaction with sfJsonTxBody present uses the "json-tx" signing
    scheme: the signature in sfTxnSignature covers the raw ASCII bytes
    of sfJsonTxBody instead of the classical signing payload.
*/
[[nodiscard]] bool
hasBody(STObject const& obj) noexcept;

/** Borrow a Slice over the ASCII body bytes.

    Returns an empty slice if sfJsonTxBody is not present. The slice is
    valid for as long as the STObject the field belongs to.
*/
[[nodiscard]] Slice
body(STObject const& obj);

/** SHA-512-Half of the body bytes.

    This is the deterministic "ASCII signing digest" used by json-tx:
    the bytes the client sees as their message are hashed with SHA-512
    and truncated to 256 bits, the same digest convention rippled uses
    elsewhere. Returns a zero-valued hash if sfJsonTxBody is absent.
*/
[[nodiscard]] uint256
bodyHash(STObject const& obj);

/** Signature check only: verify sfTxnSignature against the raw bytes of
    sfJsonTxBody using sfSigningPubKey.

    The classical signing payload is NOT used. This is the json-tx
    analogue of STTx::checkSingleSign and is intended to be called from
    the same code path (e.g. STTx::checkSign).

    Precondition: `stx` carries a non-empty sfJsonTxBody.
*/
[[nodiscard]] Expected<void, std::string>
checkSignature(STTx const& stx);

/** Structural-equivalence check: parse sfJsonTxBody as JSON and confirm
    it serialises to the same canonical binary as the other structural
    fields of `stx` (excluding sfTxnSignature and sfJsonTxBody).

    This is a local-check style rule -- it should run alongside
    passesLocalChecks, not inside the signature verification path.

    Precondition: `stx` carries a non-empty sfJsonTxBody.
*/
[[nodiscard]] Expected<void, std::string>
checkStructuralEquivalence(STTx const& stx);

}  // namespace jsonTx
}  // namespace ripple

#endif
