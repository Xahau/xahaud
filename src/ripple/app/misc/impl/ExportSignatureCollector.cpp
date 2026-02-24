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
#include <ripple/app/misc/Manifest.h>
#include <ripple/app/misc/ValidatorKeys.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/Sign.h>

namespace ripple {

bool
isExportValidatorTrusted(
    ReadView const& view,
    Application& app,
    PublicKey const& validator,
    beast::Journal const& j)
{
    if (app.config().standalone())
        return true;

    auto const master = app.validatorManifests().getMasterKey(validator);

    auto const trustedLocal = app.validators().trusted(validator) ||
        (master != validator && app.validators().trusted(master));

    // Early-ledger fallback: trust local configured UNL.
    if (view.info().seq < 256)
        return trustedLocal;

    auto const unlReport = view.read(keylet::UNLReport());
    if (!unlReport || !unlReport->isFieldPresent(sfActiveValidators))
    {
        JLOG(j.debug()) << "Export: UNLReport missing; using local trust set";
        return trustedLocal;
    }

    auto const signerId = calcAccountID(validator);
    auto const masterId = calcAccountID(master);
    auto const& active = unlReport->getFieldArray(sfActiveValidators);
    for (auto const& av : active)
    {
        auto const id = av.getAccountID(sfAccount);
        if (id == signerId || id == masterId)
            return true;
    }

    return false;
}

std::size_t
getExportUNLSize(ReadView const& view, Application& app)
{
    if (app.config().standalone())
        return 1;

    auto const unlReport = view.read(keylet::UNLReport());
    if (unlReport && unlReport->isFieldPresent(sfActiveValidators))
        return unlReport->getFieldArray(sfActiveValidators).size();

    auto const localTrusted = app.validators().getTrustedMasterKeys().size();
    return localTrusted > 0 ? localTrusted : 1;
}

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
    return getExportUNLSize(view, app);
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

std::vector<std::pair<uint256, STObject>>
signPendingExports(
    ReadView const& view,
    Application& app,
    beast::Journal const& j)
{
    std::vector<std::pair<uint256, STObject>> result;

    if (!view.rules().enabled(featureExportRNG))
        return result;

    JLOG(j.trace()) << "signPendingExports: started";

    auto const seq = view.info().seq;

    // If we're not a validator we do nothing here
    if (app.getValidationPublicKey().empty())
        return result;

    auto const& keys = app.getValidatorKeys();

    if (keys.configInvalid())
        return result;

    PublicKey pkSigning = app.getValidationPublicKey();
    auto const pk = app.validatorManifests().getMasterKey(pkSigning);

    // Only continue if we're on the UNLReport
    if (!isExportValidatorTrusted(view, app, pk, j))
        return result;

    AccountID signingAcc = calcAccountID(pkSigning);

    Keylet const exportedDirKeylet{keylet::exportedDir()};
    if (dirIsEmpty(view, exportedDirKeylet))
        return result;

    std::shared_ptr<SLE const> sleDirNode{};
    unsigned int uDirEntry{0};
    uint256 dirEntry{beast::zero};

    if (!cdirFirst(
            view, exportedDirKeylet.key, sleDirNode, uDirEntry, dirEntry))
        return result;

    do
    {
        Keylet const itemKeylet{ltCHILD, dirEntry};
        auto sleItem = view.read(itemKeylet);
        if (!sleItem)
        {
            JLOG(j.warn()) << "signPendingExports: directory node in ledger "
                           << seq << " has index to object that is missing: "
                           << to_string(dirEntry);
            continue;
        }

        LedgerEntryType const nodeType{
            safe_cast<LedgerEntryType>((*sleItem)[sfLedgerEntryType])};

        if (nodeType != ltEXPORTED_TXN)
        {
            JLOG(j.warn()) << "signPendingExports: exported directory "
                              "contained non ltEXPORTED_TXN type";
            continue;
        }

        auto const& exported = const_cast<ripple::STLedgerEntry&>(*sleItem)
                                   .getField(sfExportedTxn)
                                   .downcast<STObject>();

        // Parse the exported transaction to get its hash
        auto s = std::make_shared<ripple::Serializer>();
        exported.add(*s);
        SerialIter sitTrans(s->slice());
        try
        {
            auto const& stpTrans =
                std::make_shared<STTx const>(std::ref(sitTrans));

            if (!stpTrans->isFieldPresent(sfAccount) ||
                stpTrans->getAccountID(sfAccount) == beast::zero)
            {
                JLOG(j.warn())
                    << "signPendingExports: sfAccount missing or zero.";
                continue;
            }

            auto txnHash = stpTrans->getTransactionID();

            // Get the collector and stash txn data for signature verification.
            // This must happen before checking for cached signature so that
            // peer signatures can be verified against this txn data.
            auto& collector = app.getExportSignatureCollector();
            collector.stashTxnData(txnHash, *s);

            // Check if we already have our signature cached in the collector.
            // This enables continuous broadcasting: we sign once, then keep
            // re-broadcasting our cached signature every ledger until the
            // export is finalized (ltEXPORTED_TXN deleted).
            auto cachedSig = collector.getSignatureFrom(txnHash, pkSigning);

            if (cachedSig)
            {
                // Use cached signature - no need to re-sign
                JLOG(j.trace()) << "signPendingExports: using cached signature "
                                   "for "
                                << txnHash;
                result.emplace_back(txnHash, *cachedSig);
                continue;
            }

            // First time seeing this export - sign it now
            JLOG(j.debug())
                << "signPendingExports: signing fresh for " << txnHash;

            // Build the multisig for the exported transaction
            Serializer sigData = buildMultiSigningData(*stpTrans, signingAcc);
            auto multisig =
                ripple::sign(keys.publicKey, keys.secretKey, sigData.slice());

            // Create the sfSigner object
            STObject signer(sfSigner);
            signer.setFieldVL(sfSigningPubKey, keys.publicKey);
            signer.setAccountID(sfAccount, signingAcc);
            signer.setFieldVL(sfTxnSignature, multisig);

            JLOG(j.trace())
                << "signPendingExports: signed export " << txnHash
                << " with validator " << toBase58(TokenType::NodePublic, pk);

            result.emplace_back(txnHash, std::move(signer));
        }
        catch (std::exception& e)
        {
            JLOG(j.warn()) << "signPendingExports: Failure: " << e.what()
                           << "\n";
        }

    } while (
        cdirNext(view, exportedDirKeylet.key, sleDirNode, uDirEntry, dirEntry));

    JLOG(j.debug()) << "signPendingExports: signed " << result.size()
                    << " exports";

    return result;
}

}  // namespace ripple
