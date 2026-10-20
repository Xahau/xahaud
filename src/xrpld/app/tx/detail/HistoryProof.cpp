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

#include <xrpld/app/tx/detail/HistoryProof.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/st.h>

namespace ripple {

std::optional<uint256>
HistoryProof::verify(ReadView const& view, std::uint64_t leafIndex, Slice blob)
{
    auto const sle = view.read(keylet::historyTree());
    if (!sle)
        return std::nullopt;

    auto const proof = history::parseProof(blob);
    if (!proof)
        return std::nullopt;

    std::vector<uint256> peaks;
    if (sle->isFieldPresent(sfHistoryPeaks))
        peaks = static_cast<std::vector<uint256>>(
            sle->getFieldV256(sfHistoryPeaks));

    uint256 leaf;
    if (!history::verifyProof(
            peaks, sle->getFieldU64(sfHistoryLeafCount), leafIndex, *proof, &leaf))
        return std::nullopt;

    return leaf;
}

NotTEC
HistoryProof::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureHistoryTree))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto const& tx = ctx.tx;

    if (tx.getFlags() & tfUniversalMask)
        return temINVALID_FLAG;

    bool const replay = (ctx.flags & tapHISTORY_REPLAY) != 0;

    if (!tx.isFieldPresent(sfHistoryProof))
    {
        // Only the stored, clipped form lacks the proof, and only a replay of
        // an already validated ledger may apply that form.
        if (!replay)
        {
            JLOG(ctx.j.warn()) << "HistoryProof: missing proof.";
            return temMALFORMED;
        }
    }
    else
    {
        auto const blob = tx.getFieldVL(sfHistoryProof);

        // STTx's constructors already enforce this; keep it as a belt for
        // transactions assembled in-process.
        if (sha512Half(makeSlice(blob)) != tx.getFieldH256(sfHistoryProofDigest))
            return temMALFORMED;

        if (!history::parseProof(makeSlice(blob)))
        {
            JLOG(ctx.j.warn()) << "HistoryProof: unparseable proof.";
            return temMALFORMED;
        }
    }

    return preflight2(ctx);
}

TER
HistoryProof::preclaim(PreclaimContext const& ctx)
{
    auto const& tx = ctx.tx;

    if (tx.isFieldPresent(sfDestination))
    {
        auto const sleDest =
            ctx.view.read(keylet::account(tx[sfDestination]));
        if (!sleDest)
            return tecNO_TARGET;
    }

    if (!tx.isFieldPresent(sfHistoryProof))
    {
        // Replay of a validated ledger (preflight admitted this form only
        // under tapHISTORY_REPLAY): the proof was checked when the ledger
        // was first built, trust it.
        return tesSUCCESS;
    }

    if (!ctx.view.read(keylet::historyTree()))
        return tecINTERNAL;  // feature enabled, object missing

    if (!verify(
            ctx.view,
            tx.getFieldU64(sfHistoryLeafIndex),
            makeSlice(tx.getFieldVL(sfHistoryProof))))
    {
        JLOG(ctx.j.debug()) << "HistoryProof: proof does not verify.";
        return tecNO_PERMISSION;
    }

    return tesSUCCESS;
}

TER
HistoryProof::doApply()
{
    // The proof was verified in preclaim; everything else is the Hook's.
    return tesSUCCESS;
}

XRPAmount
HistoryProof::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // The proof is not stored, so it costs relay bandwidth and a verification
    // rather than ledger space: one drop per 32 bytes.
    XRPAmount extra{0};
    if (tx.isFieldPresent(sfHistoryProof))
        extra = XRPAmount{
            static_cast<std::int64_t>(tx.getFieldVL(sfHistoryProof).size() / 32)};

    return Transactor::calculateBaseFee(view, tx) + extra;
}

}  // namespace ripple
