//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
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

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/ledger/View.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Sign.h>

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

    // Validate signer payload binds to the validator key.
    if (!signer.isFieldPresent(sfSigningPubKey) ||
        !signer.isFieldPresent(sfAccount) ||
        !signer.isFieldPresent(sfTxnSignature))
    {
        JLOG(j_.warn()) << "Export: addSignature rejected malformed signer for "
                        << txnHash << " from "
                        << toBase58(TokenType::NodePublic, validator);
        return;
    }

    auto const sigPubKey = signer.getFieldVL(sfSigningPubKey);
    if (sigPubKey.empty() || !publicKeyType(makeSlice(sigPubKey)))
    {
        JLOG(j_.warn()) << "Export: addSignature rejected invalid pubkey for "
                        << txnHash << " from "
                        << toBase58(TokenType::NodePublic, validator);
        return;
    }

    auto const signingAcc = signer.getAccountID(sfAccount);
    PublicKey const signerPubKey{makeSlice(sigPubKey)};
    if (signerPubKey != validator || signingAcc != calcAccountID(validator))
    {
        JLOG(j_.warn())
            << "Export: addSignature rejected identity mismatch for " << txnHash
            << " from " << toBase58(TokenType::NodePublic, validator);
        return;
    }

    // Verify immediately if tx data is available.
    auto verifyWithStashedData =
        [&](STObject const& candidate) -> std::optional<bool> {
        auto txnIt = exportedTxnData_.find(txnHash);
        if (txnIt == exportedTxnData_.end())
            return std::nullopt;

        try
        {
            SerialIter sit(txnIt->second.slice());
            auto stpTrans = std::make_shared<STTx const>(std::ref(sit));

            auto const candAcc = candidate.getAccountID(sfAccount);
            auto const candPub = candidate.getFieldVL(sfSigningPubKey);
            auto const candSig = candidate.getFieldVL(sfTxnSignature);
            Serializer sigData = buildMultiSigningData(*stpTrans, candAcc);

            return ripple::verify(
                PublicKey(makeSlice(candPub)),
                sigData.slice(),
                makeSlice(candSig),
                true);
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn())
                << "Export: addSignature verification exception for " << txnHash
                << ": " << e.what();
            return false;
        }
    };

    auto const verified = verifyWithStashedData(signer);
    if (verified && !*verified)
    {
        JLOG(j_.warn())
            << "Export: addSignature rejected invalid signature for " << txnHash
            << " from " << toBase58(TokenType::NodePublic, validator);
        return;
    }

    auto ensureFirstSeen = [&]() {
        if (firstSeenLedger_.find(txnHash) == firstSeenLedger_.end())
        {
            firstSeenLedger_[txnHash] = currentSeq;
            JLOG(j_.debug()) << "Export: first signature for " << txnHash
                             << " at ledger " << currentSeq;
        }
    };

    auto& signerMap = signatures_[txnHash];
    auto verIt = verified_.find(txnHash);
    bool const alreadyVerified = verIt != verified_.end() &&
        verIt->second.find(validator) != verIt->second.end();

    if (auto existing = signerMap.find(validator); existing != signerMap.end())
    {
        // Never downgrade a verified cached signature to an unverified one.
        if (!verified && alreadyVerified)
        {
            JLOG(j_.trace())
                << "Export: addSignature ignored unverified duplicate for "
                << txnHash << " from "
                << toBase58(TokenType::NodePublic, validator);
            return;
        }

        ensureFirstSeen();
        existing->second = std::move(signer);
        if (verified && *verified)
            verified_[txnHash].insert(validator);
        else if (auto it = verified_.find(txnHash); it != verified_.end())
        {
            it->second.erase(validator);
            if (it->second.empty())
                verified_.erase(it);
        }

        JLOG(j_.trace()) << "Export: updated signature from "
                         << toBase58(TokenType::NodePublic, validator)
                         << " for " << txnHash
                         << " (verified=" << (verified && *verified)
                         << ", total=" << signerMap.size() << ")";
        return;
    }

    ensureFirstSeen();
    signerMap.emplace(validator, std::move(signer));
    if (verified && *verified)
        verified_[txnHash].insert(validator);

    JLOG(j_.trace()) << "Export: added signature from "
                     << toBase58(TokenType::NodePublic, validator) << " for "
                     << txnHash << " (verified=" << (verified && *verified)
                     << ", total=" << signerMap.size() << ")";
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
ExportSignatureCollector::verifiedSignatureCount(uint256 const& txnHash) const
{
    std::lock_guard lock(mutex_);

    auto it = verified_.find(txnHash);
    if (it != verified_.end())
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
    auto const verifiedCount = verifiedSignatureCount(txnHash);
    auto const totalCount = signatureCount(txnHash);
    auto const unlSize = getUNLSize(view, app);

    auto const threshold = calculateQuorumThreshold(unlSize);

    JLOG(j_.trace()) << "Export: hasQuorum check for " << txnHash
                     << " verified=" << verifiedCount
                     << " total=" << totalCount << " unlSize=" << unlSize
                     << " threshold=" << threshold;

    return verifiedCount >= threshold;
}

std::vector<uint256>
ExportSignatureCollector::getExportsWithQuorum(
    ReadView const& view,
    Application& app) const
{
    std::lock_guard lock(mutex_);

    std::vector<uint256> ready;
    auto const unlSize = getUNLSize(view, app);
    auto const threshold = calculateQuorumThreshold(unlSize);

    for (auto const& [txnHash, signerMap] : signatures_)
    {
        auto verIt = verified_.find(txnHash);
        auto const verifiedCount =
            verIt != verified_.end() ? verIt->second.size() : 0u;
        if (verifiedCount >= threshold)
        {
            ready.push_back(txnHash);
            JLOG(j_.info())
                << "Export: quorum reached for " << txnHash << " (verified="
                << verifiedCount << " total=" << signerMap.size() << "/"
                << unlSize << ")";
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
    Serializer txnData,
    LedgerIndex currentSeq)
{
    std::lock_guard lock(mutex_);

    auto parseTx = [](Serializer const& data) -> bool {
        try
        {
            SerialIter sit(data.slice());
            auto tx = std::make_shared<STTx const>(std::ref(sit));
            (void)tx;
            return true;
        }
        catch (std::exception const&)
        {
            return false;
        }
    };

    if (!parseTx(txnData))
    {
        JLOG(j_.warn()) << "Export: rejected invalid txn data for " << txnHash;
        return;
    }

    bool stored = false;
    if (auto it = exportedTxnData_.find(txnHash); it == exportedTxnData_.end())
    {
        exportedTxnData_.emplace(txnHash, std::move(txnData));
        stored = true;
        JLOG(j_.trace()) << "Export: stashed txn data for " << txnHash;
    }
    else if (!parseTx(it->second))
    {
        it->second = std::move(txnData);
        stored = true;
        JLOG(j_.warn()) << "Export: replaced invalid cached txn data for "
                        << txnHash;
    }

    if (!stored)
        return;

    if (firstSeenLedger_.find(txnHash) == firstSeenLedger_.end())
    {
        firstSeenLedger_[txnHash] = currentSeq;
        JLOG(j_.trace()) << "Export: first-seen (txn data) for " << txnHash
                         << " at ledger " << currentSeq;
    }

    auto sigIt = signatures_.find(txnHash);
    if (sigIt == signatures_.end())
        return;

    std::size_t pruned = 0;
    try
    {
        SerialIter sit(exportedTxnData_.at(txnHash).slice());
        auto stpTrans = std::make_shared<STTx const>(std::ref(sit));

        auto& signerMap = sigIt->second;
        auto& verifiedSet = verified_[txnHash];
        for (auto it = signerMap.begin(); it != signerMap.end();)
        {
            auto const& validator = it->first;
            auto const& signer = it->second;

            bool valid = false;
            try
            {
                if (signer.isFieldPresent(sfSigningPubKey) &&
                    signer.isFieldPresent(sfAccount) &&
                    signer.isFieldPresent(sfTxnSignature))
                {
                    auto const sigPubKey = signer.getFieldVL(sfSigningPubKey);
                    auto const signingAcc = signer.getAccountID(sfAccount);
                    auto const signature = signer.getFieldVL(sfTxnSignature);

                    if (!sigPubKey.empty() &&
                        publicKeyType(makeSlice(sigPubKey)))
                    {
                        PublicKey const signerPk{makeSlice(sigPubKey)};
                        if (signerPk == validator &&
                            signingAcc == calcAccountID(validator))
                        {
                            Serializer sigData =
                                buildMultiSigningData(*stpTrans, signingAcc);
                            valid = ripple::verify(
                                signerPk,
                                sigData.slice(),
                                makeSlice(signature),
                                true);
                        }
                    }
                }
            }
            catch (std::exception const&)
            {
                valid = false;
            }

            if (valid)
            {
                verifiedSet.insert(validator);
                ++it;
            }
            else
            {
                verifiedSet.erase(validator);
                it = signerMap.erase(it);
                ++pruned;
            }
        }

        if (verifiedSet.empty())
            verified_.erase(txnHash);

        if (signerMap.empty())
        {
            signatures_.erase(sigIt);
            firstSeenLedger_.erase(txnHash);
        }
    }
    catch (std::exception const& e)
    {
        JLOG(j_.warn()) << "Export: failed to parse stashed txn data for "
                        << txnHash << ": " << e.what();
    }

    if (pruned > 0)
    {
        JLOG(j_.warn()) << "Export: pruned " << pruned
                        << " invalid unverified signatures for " << txnHash;
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

    auto ensureFirstSeen = [&]() {
        if (firstSeenLedger_.find(txnHash) == firstSeenLedger_.end())
        {
            firstSeenLedger_[txnHash] = currentSeq;
            JLOG(j_.debug()) << "Export: first signature for " << txnHash
                             << " at ledger " << currentSeq;
        }
    };

    // The signer payload must bind to the validator who carried it in
    // TMValidation. Otherwise a peer can misattribute signatures.
    if (!signer.isFieldPresent(sfSigningPubKey) ||
        !signer.isFieldPresent(sfAccount) ||
        !signer.isFieldPresent(sfTxnSignature))
    {
        JLOG(j_.warn()) << "Export: malformed signer payload for " << txnHash
                        << " from "
                        << toBase58(TokenType::NodePublic, validator);
        return false;
    }

    auto const sigPubKey = signer.getFieldVL(sfSigningPubKey);
    if (sigPubKey.empty() || !publicKeyType(makeSlice(sigPubKey)))
    {
        JLOG(j_.warn()) << "Export: invalid signer pubkey payload for "
                        << txnHash << " from "
                        << toBase58(TokenType::NodePublic, validator);
        return false;
    }

    auto const signingAcc = signer.getAccountID(sfAccount);
    PublicKey const signerPubKey{makeSlice(sigPubKey)};
    if (signerPubKey != validator || signingAcc != calcAccountID(validator))
    {
        JLOG(j_.warn()) << "Export: signer identity mismatch for " << txnHash
                        << " from "
                        << toBase58(TokenType::NodePublic, validator);
        return false;
    }

    auto& signerMap = signatures_[txnHash];
    auto eraseIfEmpty = [&]() {
        if (signerMap.empty())
            signatures_.erase(txnHash);
    };

    // Verify if we have stashed tx data. Returns:
    //   true  -> verified
    //   false -> verification failed
    //   nullopt -> cannot verify yet (no tx data)
    auto verifyWithStashedData =
        [&](STObject const& candidate) -> std::optional<bool> {
        auto txnIt = exportedTxnData_.find(txnHash);
        if (txnIt == exportedTxnData_.end())
            return std::nullopt;

        try
        {
            SerialIter sit(txnIt->second.slice());
            auto stpTrans = std::make_shared<STTx const>(std::ref(sit));

            auto signingAcc = candidate.getAccountID(sfAccount);
            auto sigPubKey = candidate.getFieldVL(sfSigningPubKey);
            auto signature = candidate.getFieldVL(sfTxnSignature);

            Serializer sigData = buildMultiSigningData(*stpTrans, signingAcc);
            bool const verified = ripple::verify(
                PublicKey(makeSlice(sigPubKey)),
                sigData.slice(),
                makeSlice(signature),
                true);

            if (!verified)
            {
                JLOG(j_.warn())
                    << "Export: signature verification FAILED for " << txnHash
                    << " from " << toBase58(TokenType::NodePublic, validator);
                return false;
            }

            JLOG(j_.trace())
                << "Export: signature verified for " << txnHash << " from "
                << toBase58(TokenType::NodePublic, validator);
            return true;
        }
        catch (std::exception const& e)
        {
            JLOG(j_.warn()) << "Export: signature verification exception for "
                            << txnHash << ": " << e.what();
            return false;
        }
    };

    auto verIt = verified_.find(txnHash);
    bool const alreadyVerified = verIt != verified_.end() &&
        verIt->second.find(validator) != verIt->second.end();

    // Duplicate from same validator:
    // - keep existing once cryptographically verified
    // - allow replacement while unverified so a stale/bad early copy can heal
    if (auto existing = signerMap.find(validator); existing != signerMap.end())
    {
        if (alreadyVerified)
        {
            JLOG(j_.trace())
                << "Export: ignoring duplicate verified signature "
                << "from " << toBase58(TokenType::NodePublic, validator)
                << " for " << txnHash;
            return true;
        }

        auto const verified = verifyWithStashedData(signer);
        if (verified && !*verified)
        {
            eraseIfEmpty();
            return false;  // Reject replacement with invalid signature
        }

        if (!verified)
        {
            JLOG(j_.trace())
                << "Export: replacing unverified signature for " << txnHash
                << " from " << toBase58(TokenType::NodePublic, validator)
                << " (no txn data yet)";
        }

        ensureFirstSeen();
        existing->second = std::move(signer);
        if (verified && *verified)
            verified_[txnHash].insert(validator);

        JLOG(j_.trace()) << "Export: replaced signature from "
                         << toBase58(TokenType::NodePublic, validator)
                         << " for " << txnHash
                         << " (verified=" << (verified && *verified) << ")";
        return true;
    }

    auto const verified = verifyWithStashedData(signer);
    if (verified && !*verified)
    {
        eraseIfEmpty();
        return false;  // Don't add invalid signature
    }

    if (!verified)
    {
        // No txn data yet - add unverified (will verify later or in Transactor)
        JLOG(j_.trace()) << "Export: adding unverified signature for "
                         << txnHash << " (no txn data yet)";
    }

    ensureFirstSeen();
    signerMap.emplace(validator, std::move(signer));

    if (verified && *verified)
        verified_[txnHash].insert(validator);

    JLOG(j_.trace()) << "Export: added signature from "
                     << toBase58(TokenType::NodePublic, validator) << " for "
                     << txnHash << " (total: " << signerMap.size()
                     << ", verified=" << (verified && *verified) << ")";

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

    if (!view.rules().enabled(featureExport))
        return result;

    JLOG(j.trace()) << "signPendingExports: started";

    auto const seq = view.info().seq;

    // If we're not a validator we do nothing here
    auto const validationPublicKey = app.getValidationPublicKey();
    if (!validationPublicKey)
        return result;

    auto const& validatorKeys = app.getValidatorKeys();

    if (validatorKeys.configInvalid() || !validatorKeys.keys)
        return result;

    auto const& keys = *validatorKeys.keys;
    PublicKey const& pkSigning = *validationPublicKey;
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
            collector.stashTxnData(txnHash, *s, seq);

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
