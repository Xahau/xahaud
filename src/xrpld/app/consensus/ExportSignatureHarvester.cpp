//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/XRPLF/rippled
    Copyright 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/consensus/ExportSignatureHarvester.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Sign.h>

#include <cstring>

namespace ripple {

bool
verifyExportSignatureAgainstTx(
    STTx const& exportTx,
    PublicKey const& validator,
    Slice sigSlice,
    uint256 const& txHash,
    beast::Journal j,
    char const* source)
{
    if (!exportTx.isFieldPresent(sfExportedTxn))
    {
        JLOG(j.warn()) << "Export: cannot verify sig"
                       << " txHash=" << txHash << " source=" << source
                       << " reason=missing-sfExportedTxn";
        return false;
    }

    try
    {
        auto const& exportedObj = const_cast<STTx&>(exportTx)
                                      .peekAtField(sfExportedTxn)
                                      .downcast<STObject>();

        Serializer innerSer;
        exportedObj.add(innerSer);
        SerialIter sit(innerSer.slice());
        STTx innerTx(std::ref(sit));

        auto const signerAcctID = calcAccountID(validator);
        auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
        if (!verify(validator, sigData.slice(), sigSlice))
        {
            JLOG(j.warn()) << "Export: invalid multisign sig"
                           << " txHash=" << txHash << " source=" << source
                           << " validator=" << calcNodeID(validator)
                           << " reason=signature-verify-failed";
            return false;
        }
        return true;
    }
    catch (std::exception const& e)
    {
        JLOG(j.warn()) << "Export: failed to verify sig"
                       << " txHash=" << txHash << " source=" << source
                       << " validator=" << calcNodeID(validator)
                       << " error=" << e.what();
        return false;
    }
}

std::size_t
harvestExportSignatures(
    ExportSignatureHarvestInput const& input,
    ExportSigCollector& collector,
    beast::Journal j)
{
    if (input.exportSignatures.empty())
        return 0;

    if (input.exportSignatures.size() > input.maxEntries)
    {
        JLOG(j.warn()) << "Export: rejecting proposal signatures"
                       << " reason=too-many"
                       << " source=" << input.source
                       << " count=" << input.exportSignatures.size()
                       << " max=" << input.maxEntries
                       << " sender=" << calcNodeID(input.senderPK)
                       << " prevLedger=" << input.proposalPrevLedger;
        return 0;
    }

    if (!input.isActiveSigner(input.senderPK))
        return 0;

    if (input.activeViewSourceLedgerHash)
    {
        if (input.proposalPrevLedger != *input.activeViewSourceLedgerHash)
            return 0;
    }

    for (auto const& blob : input.exportSignatures)
    {
        if (blob.size() < 65)
            continue;

        auto const pkSlice = makeSlice(blob).substr(32, 33);
        if (!publicKeyType(pkSlice))
            continue;

        if (PublicKey{pkSlice} != input.senderPK)
        {
            JLOG(j.warn()) << "Export: rejecting proposal signatures"
                           << " reason=embedded-pubkey-mismatch"
                           << " source=" << input.source
                           << " sender=" << calcNodeID(input.senderPK)
                           << " embedded=" << calcNodeID(PublicKey{pkSlice})
                           << " prevLedger=" << input.proposalPrevLedger;
            return 0;
        }
    }

    std::size_t stored = 0;

    for (auto const& blob : input.exportSignatures)
    {
        if (blob.size() < 65)
            continue;

        uint256 txHash;
        std::memcpy(txHash.data(), blob.data(), 32);

        if (blob.size() <= 65)
            continue;

        if (collector.hasVerifiedSignature(txHash, input.senderPK))
            continue;

        auto const fullSlice = makeSlice(blob);
        auto const sigSlice = fullSlice.substr(65);

        auto const txIt = input.exportTxns.find(txHash);
        if (txIt == input.exportTxns.end())
        {
            JLOG(j.debug())
                << "Export: storing unverified sig"
                << " txHash=" << txHash << " source=" << input.source
                << " signer=" << calcNodeID(input.senderPK)
                << " reason=tx-not-in-open-ledger"
                << " currentClosedSeq=" << input.currentClosedSeq;
            Buffer sigBuf(sigSlice.data(), sigSlice.size());
            collector.addUnverifiedSignature(
                txHash, input.senderPK, sigBuf, input.currentClosedSeq);
            ++stored;
            continue;
        }

        if (!verifyExportSignatureAgainstTx(
                *txIt->second,
                input.senderPK,
                sigSlice,
                txHash,
                j,
                "open ledger"))
        {
            continue;
        }

        Buffer sigBuf(sigSlice.data(), sigSlice.size());
        collector.addVerifiedSignature(
            txHash, input.senderPK, sigBuf, input.currentClosedSeq);
        ++stored;
    }

    if (stored > 0)
    {
        JLOG(j.debug()) << "Export: harvested proposal signatures"
                        << " stored=" << stored
                        << " advertised=" << input.exportSignatures.size()
                        << " source=" << input.source
                        << " sender=" << calcNodeID(input.senderPK)
                        << " currentClosedSeq=" << input.currentClosedSeq;
    }
    return stored;
}

}  // namespace ripple
