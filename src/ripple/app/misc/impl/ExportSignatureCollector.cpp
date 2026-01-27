//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 Ripple Labs Inc.

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

#include <ripple/app/main/Application.h>
#include <ripple/app/misc/ExportSignatureCollector.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/Sign.h>

namespace ripple {

ExportSignatureCollector::ExportSignatureCollector(beast::Journal journal)
    : j_(journal)
{
}

void
ExportSignatureCollector::addSignature(
    uint256 const& txnHash,
    PublicKey const& validator,
    STObject signer,
    LedgerIndex currentSeq)
{
    std::lock_guard lock(mutex_);

    // Track first-seen time for cleanup
    if (firstSeenLedger_.find(txnHash) == firstSeenLedger_.end())
    {
        firstSeenLedger_[txnHash] = currentSeq;
        JLOG(j_.debug()) << "Export: first signature for " << txnHash
                         << " at ledger " << currentSeq;
    }

    // Add or update signature for this validator
    auto& signerMap = signatures_[txnHash];
    auto [it, inserted] = signerMap.emplace(validator, std::move(signer));

    if (inserted)
    {
        JLOG(j_.trace()) << "Export: added signature from "
                         << toBase58(TokenType::NodePublic, validator)
                         << " for " << txnHash
                         << " (total: " << signerMap.size() << ")";
    }
}

STArray
ExportSignatureCollector::getSignatures(uint256 const& txnHash) const
{
    std::lock_guard lock(mutex_);

    STArray signers(sfSigners);

    auto it = signatures_.find(txnHash);
    if (it != signatures_.end())
    {
        for (auto const& [pk, signer] : it->second)
        {
            signers.push_back(signer);
        }
    }

    return signers;
}

std::size_t
ExportSignatureCollector::signatureCount(uint256 const& txnHash) const
{
    std::lock_guard lock(mutex_);

    auto it = signatures_.find(txnHash);
    if (it != signatures_.end())
        return it->second.size();
    return 0;
}

std::size_t
ExportSignatureCollector::getUNLSize(ReadView const& view, Application& app)
    const
{
    // For first 256 ledgers, UNLReport may not exist
    // In standalone mode, we're the only validator
    auto const seq = view.info().seq;
    if (seq < 256 || app.config().standalone())
        return 1;

    // Try to get UNL size from UNLReport
    auto const unlReportKey = keylet::UNLReport();
    auto const sle = view.read(unlReportKey);
    if (sle && sle->isFieldPresent(sfActiveValidators))
    {
        return sle->getFieldArray(sfActiveValidators).size();
    }

    // Fallback: use validator list count
    auto const count = app.validators().count();
    return count > 0 ? count : 1;
}

bool
ExportSignatureCollector::hasQuorum(
    uint256 const& txnHash,
    ReadView const& view,
    Application& app) const
{
    auto const sigCount = signatureCount(txnHash);
    auto const unlSize = getUNLSize(view, app);

    // Quorum is 80% of UNL, rounded up
    auto const threshold = (unlSize * 80 + 99) / 100;

    JLOG(j_.trace()) << "Export: hasQuorum check for " << txnHash
                     << " sigCount=" << sigCount << " unlSize=" << unlSize
                     << " threshold=" << threshold;

    return sigCount >= threshold;
}

std::vector<uint256>
ExportSignatureCollector::getExportsWithQuorum(
    ReadView const& view,
    Application& app) const
{
    std::lock_guard lock(mutex_);

    std::vector<uint256> ready;
    auto const unlSize = getUNLSize(view, app);
    auto const threshold = (unlSize * 80 + 99) / 100;

    for (auto const& [txnHash, signerMap] : signatures_)
    {
        if (signerMap.size() >= threshold)
        {
            ready.push_back(txnHash);
            JLOG(j_.info())
                << "Export: quorum reached for " << txnHash << " ("
                << signerMap.size() << "/" << unlSize << " signatures)";
        }
    }

    return ready;
}

std::vector<uint256>
ExportSignatureCollector::getPendingExports() const
{
    std::lock_guard lock(mutex_);

    std::vector<uint256> pending;
    pending.reserve(signatures_.size());

    for (auto const& [txnHash, _] : signatures_)
    {
        pending.push_back(txnHash);
    }

    return pending;
}

bool
ExportSignatureCollector::hasSignatureFrom(
    uint256 const& txnHash,
    PublicKey const& validator) const
{
    std::lock_guard lock(mutex_);

    auto txnIt = signatures_.find(txnHash);
    if (txnIt == signatures_.end())
        return false;

    return txnIt->second.find(validator) != txnIt->second.end();
}

std::optional<STObject>
ExportSignatureCollector::getSignatureFrom(
    uint256 const& txnHash,
    PublicKey const& validator) const
{
    std::lock_guard lock(mutex_);

    auto txnIt = signatures_.find(txnHash);
    if (txnIt == signatures_.end())
        return std::nullopt;

    auto sigIt = txnIt->second.find(validator);
    if (sigIt == txnIt->second.end())
        return std::nullopt;

    return sigIt->second;
}

void
ExportSignatureCollector::clearForTxn(uint256 const& txnHash)
{
    std::lock_guard lock(mutex_);

    auto sigCount = signatures_.erase(txnHash);
    auto seqCount = firstSeenLedger_.erase(txnHash);
    exportedTxnData_.erase(txnHash);
    verified_.erase(txnHash);

    if (sigCount > 0 || seqCount > 0)
    {
        JLOG(j_.debug()) << "Export: cleared signatures for " << txnHash;
    }
}

void
ExportSignatureCollector::cleanupStale(
    LedgerIndex currentSeq,
    LedgerIndex maxAge)
{
    std::lock_guard lock(mutex_);

    std::vector<uint256> toRemove;

    for (auto const& [txnHash, firstSeen] : firstSeenLedger_)
    {
        if (currentSeq > firstSeen + maxAge)
        {
            toRemove.push_back(txnHash);
        }
    }

    for (auto const& txnHash : toRemove)
    {
        JLOG(j_.warn()) << "Export: cleaning up stale signatures for "
                        << txnHash
                        << " (age: " << (currentSeq - firstSeenLedger_[txnHash])
                        << " ledgers)";

        signatures_.erase(txnHash);
        firstSeenLedger_.erase(txnHash);
        exportedTxnData_.erase(txnHash);
        verified_.erase(txnHash);
    }

    if (!toRemove.empty())
    {
        JLOG(j_.info()) << "Export: cleaned up " << toRemove.size()
                        << " stale exports";
    }
}

void
ExportSignatureCollector::stashTxnData(
    uint256 const& txnHash,
    Serializer txnData)
{
    std::lock_guard lock(mutex_);

    // Only stash if we don't already have it
    if (exportedTxnData_.find(txnHash) == exportedTxnData_.end())
    {
        exportedTxnData_.emplace(txnHash, std::move(txnData));
        JLOG(j_.trace()) << "Export: stashed txn data for " << txnHash;
    }
}

bool
ExportSignatureCollector::verifyAndAddSignature(
    uint256 const& txnHash,
    PublicKey const& validator,
    STObject signer,
    LedgerIndex currentSeq)
{
    std::lock_guard lock(mutex_);

    // Track first-seen time for cleanup
    if (firstSeenLedger_.find(txnHash) == firstSeenLedger_.end())
    {
        firstSeenLedger_[txnHash] = currentSeq;
        JLOG(j_.debug()) << "Export: first signature for " << txnHash
                         << " at ledger " << currentSeq;
    }

    // Check if we already have this signature
    auto& signerMap = signatures_[txnHash];
    if (signerMap.find(validator) != signerMap.end())
    {
        JLOG(j_.trace()) << "Export: already have signature from "
                         << toBase58(TokenType::NodePublic, validator)
                         << " for " << txnHash;
        return true;  // Already have it
    }

    // Try to verify if we have the txn data
    bool verified = false;
    auto txnIt = exportedTxnData_.find(txnHash);
    if (txnIt != exportedTxnData_.end())
    {
        try
        {
            // Parse the stashed transaction
            SerialIter sit(txnIt->second.slice());
            auto stpTrans = std::make_shared<STTx const>(std::ref(sit));

            // Get signer account from the signer object
            auto signingAcc = signer.getAccountID(sfAccount);
            auto sigPubKey = signer.getFieldVL(sfSigningPubKey);
            auto signature = signer.getFieldVL(sfTxnSignature);

            // Build the multisig data and verify
            Serializer sigData = buildMultiSigningData(*stpTrans, signingAcc);
            verified = ripple::verify(
                PublicKey(makeSlice(sigPubKey)),
                sigData.slice(),
                makeSlice(signature),
                true);

            if (!verified)
            {
                JLOG(j_.warn())
                    << "Export: signature verification FAILED for " << txnHash
                    << " from " << toBase58(TokenType::NodePublic, validator);
                return false;  // Don't add invalid signature
            }

            JLOG(j_.trace())
                << "Export: signature verified for " << txnHash << " from "
                << toBase58(TokenType::NodePublic, validator);
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn()) << "Export: signature verification exception for "
                            << txnHash << ": " << e.what();
            return false;  // Don't add if we can't verify
        }
    }
    else
    {
        // No txn data yet - add unverified (will verify later or in Transactor)
        JLOG(j_.trace()) << "Export: adding unverified signature for "
                         << txnHash << " (no txn data yet)";
    }

    // Add the signature
    signerMap.emplace(validator, std::move(signer));

    if (verified)
    {
        verified_[txnHash].insert(validator);
    }

    JLOG(j_.trace()) << "Export: added signature from "
                     << toBase58(TokenType::NodePublic, validator) << " for "
                     << txnHash << " (total: " << signerMap.size()
                     << ", verified=" << verified << ")";

    return true;
}

bool
ExportSignatureCollector::isSignatureVerified(
    uint256 const& txnHash,
    PublicKey const& validator) const
{
    std::lock_guard lock(mutex_);

    auto it = verified_.find(txnHash);
    if (it == verified_.end())
        return false;

    return it->second.find(validator) != it->second.end();
}

bool
ExportSignatureCollector::verifySignature(
    uint256 const& txnHash,
    PublicKey const& validator)
{
    std::lock_guard lock(mutex_);

    // Already verified?
    auto verIt = verified_.find(txnHash);
    if (verIt != verified_.end() &&
        verIt->second.find(validator) != verIt->second.end())
    {
        return true;
    }

    // Get the signature
    auto sigIt = signatures_.find(txnHash);
    if (sigIt == signatures_.end())
        return false;

    auto signerIt = sigIt->second.find(validator);
    if (signerIt == sigIt->second.end())
        return false;

    // Get the txn data
    auto txnIt = exportedTxnData_.find(txnHash);
    if (txnIt == exportedTxnData_.end())
    {
        JLOG(j_.warn()) << "Export: cannot verify signature - no txn data for "
                        << txnHash;
        return false;
    }

    try
    {
        // Parse the stashed transaction
        SerialIter sit(txnIt->second.slice());
        auto stpTrans = std::make_shared<STTx const>(std::ref(sit));

        // Get signer info
        auto const& signer = signerIt->second;
        auto signingAcc = signer.getAccountID(sfAccount);
        auto sigPubKey = signer.getFieldVL(sfSigningPubKey);
        auto signature = signer.getFieldVL(sfTxnSignature);

        // Build the multisig data and verify
        Serializer sigData = buildMultiSigningData(*stpTrans, signingAcc);
        bool verified = ripple::verify(
            PublicKey(makeSlice(sigPubKey)),
            sigData.slice(),
            makeSlice(signature),
            true);

        if (verified)
        {
            verified_[txnHash].insert(validator);
            JLOG(j_.trace())
                << "Export: late-verified signature for " << txnHash << " from "
                << toBase58(TokenType::NodePublic, validator);
        }
        else
        {
            JLOG(j_.warn())
                << "Export: late signature verification FAILED for " << txnHash
                << " from " << toBase58(TokenType::NodePublic, validator);
        }

        return verified;
    }
    catch (std::exception const& e)
    {
        JLOG(j_.warn()) << "Export: late verification exception for " << txnHash
                        << ": " << e.what();
        return false;
    }
}

}  // namespace ripple
