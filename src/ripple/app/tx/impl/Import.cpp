//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/misc/Manifest.h>
#include <ripple/app/tx/impl/Import.h>
#include <ripple/app/tx/impl/SetSignerList.h>
#include <ripple/basics/Log.h>
#include <ripple/basics/base64.h>
#include <ripple/json/json_reader.h>
#include <ripple/json/json_value.h>
#include <ripple/json/to_string.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Import.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/STValidation.h>
#include <ripple/protocol/st.h>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <vector>

namespace ripple {

TxConsequences
Import::makeTxConsequences(PreflightContext const& ctx)
{
    auto calculate = [](PreflightContext const& ctx) -> XRPAmount {
        auto const [inner, meta] = getInnerTxn(ctx.tx, ctx.j);
        if (!inner || !inner->isFieldPresent(sfFee))
            return beast::zero;

        if (!meta || !meta->isFieldPresent(sfTransactionResult))
            return beast::zero;

        auto const result = meta->getFieldU8(sfTransactionResult);
        if (!isTesSuccess(result) &&
            !(result >= tecCLAIM && result <= tecLAST_POSSIBLE_ENTRY))
            return beast::zero;

        STAmount const innerFee = inner->getFieldAmount(sfFee);
        if (!isXRP(innerFee))
            return beast::zero;

        return innerFee.xrp();
    };

    return TxConsequences{ctx.tx, calculate(ctx)};
}

std::pair<
    std::unique_ptr<STTx const>,      // txn
    std::unique_ptr<STObject const>>  // meta
Import::getInnerTxn(
    STTx const& outer,
    beast::Journal const& j,
    Json::Value const* xpop)
{
    // parse blob as json

    std::optional<Json::Value> xpop_storage;

    if (!xpop && outer.isFieldPresent(sfBlob))
    {
        xpop_storage = syntaxCheckXPOP(outer.getFieldVL(sfBlob), j);
        xpop = &(*xpop_storage);
    }

    if (!xpop)
        return {};

    // extract the transaction for which this xpop is a proof
    auto rawTx = strUnHex((*xpop)[jss::transaction][jss::blob].asString());
    auto meta = strUnHex((*xpop)[jss::transaction][jss::meta].asString());

    if (!rawTx)
    {
        JLOG(j.warn()) << "Import: failed to deserialize tx blob inside xpop "
                          "(invalid hex) "
                       << outer.getTransactionID();
        return {};
    }

    if (!meta)
    {
        JLOG(j.warn()) << "Import: failed to deserialize tx meta inside xpop "
                          "(invalid hex) "
                       << outer.getTransactionID();
        return {};
    }

    try
    {
        return {
            std::make_unique<STTx const>(
                SerialIter{rawTx->data(), rawTx->size()}),
            std::make_unique<STObject const>(
                SerialIter(meta->data(), meta->size()), sfMetadata)};
    }
    catch (std::exception& e)
    {
        JLOG(j.warn())
            << "Import: failed to deserialize tx blob/meta inside xpop ("
            << e.what() << ") outer txid: " << outer.getTransactionID();
        return {};
    }
}

NotTEC
Import::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureImport))
        return temDISABLED;

    if (!ctx.rules.enabled(featureHooksUpdate1) &&
        ctx.tx.isFieldPresent(sfIssuer))
        return temDISABLED;

    if (ctx.tx.isFieldPresent(sfIssuer) &&
        ctx.tx.getAccountID(sfIssuer) == ctx.tx.getAccountID(sfAccount))
    {
        JLOG(ctx.j.warn()) << "Import: Issuer cannot be the source account.";
        return temMALFORMED;
    }

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;

    if (!tx.isFieldPresent(sfBlob))
    {
        JLOG(ctx.j.warn())
            << "Import: sfBlob was missing (should be impossible) "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (tx.getFieldVL(sfBlob).size() > (512 * 1024))
    {
        JLOG(ctx.j.warn()) << "Import: blob was more than 512kib "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // parse blob as json
    auto const xpop = syntaxCheckXPOP(tx.getFieldVL(sfBlob), ctx.j);

    if (!xpop)
        return temMALFORMED;

    // we will check if we recognise the vl key in preclaim because it may be
    // from on-ledger object
    std::optional<PublicKey> masterVLKey;
    {
        std::string strPk =
            (*xpop)[jss::validation][jss::unl][jss::public_key].asString();
        auto pkHex = strUnHex(strPk);
        if (!pkHex)
        {
            JLOG(ctx.j.warn())
                << "Import: validation.unl.public_key was not valid hex.";
            return temMALFORMED;
        }

        auto const pkType = publicKeyType(makeSlice(*pkHex));
        if (!pkType)
        {
            JLOG(ctx.j.warn()) << "Import: validation.unl.public_key was not a "
                                  "recognised public key type.";
            return temMALFORMED;
        }

        masterVLKey = PublicKey(makeSlice(*pkHex));
    }

    auto const [stpTrans, meta] = getInnerTxn(tx, ctx.j, &(*xpop));

    if (!stpTrans || !meta)
        return temMALFORMED;

    if (stpTrans->isFieldPresent(sfTicketSequence))
    {
        JLOG(ctx.j.warn()) << "Import: cannot use TicketSequence XPOP.";
        return temMALFORMED;
    }

    // check if txn is emitted or a psuedo
    if (isPseudoTx(*stpTrans) || stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(ctx.j.warn()) << "Import: attempted to import xpop containing an "
                              "emitted or pseudo txn. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // ensure that the txn was tesSUCCESS / tec
    if (!meta->isFieldPresent(sfTransactionResult))
    {
        JLOG(ctx.j.warn()) << "Import: inner txn lacked transaction result... "
                           << tx.getTransactionID();
        return temMALFORMED;
    }
    else
    {
        uint8_t innerResult = meta->getFieldU8(sfTransactionResult);

        if (isTesSuccess(innerResult))
        {
            // pass
        }
        else if (
            innerResult >= tecCLAIM && innerResult <= tecLAST_POSSIBLE_ENTRY)
        {
            // pass : proof of burn on account set can be done with a tec code
        }
        else
        {
            JLOG(ctx.j.warn())
                << "Import: inner txn did not have a tesSUCCESS or tec result "
                << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    // check if the account matches the account in the xpop, if not bail early
    if (stpTrans->getAccountID(sfAccount) != tx.getAccountID(sfAccount))
    {
        JLOG(ctx.j.warn()) << "Import: import and txn inside xpop must be "
                              "signed by the same account "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // ensure inner txn is for networkid = 0 (network id must therefore be
    // missing)
    if (stpTrans->isFieldPresent(sfNetworkID))
    {
        JLOG(ctx.j.warn()) << "Import: attempted to import xpop containing a "
                              "txn with a sfNetworkID field. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // ensure inner txn is destined for the network we're on, this is according
    // to OperationLimit field
    if (!stpTrans->isFieldPresent(sfOperationLimit))
    {
        JLOG(ctx.j.warn()) << "Import: OperationLimit missing from inner xpop "
                              "txn. outer txid: "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (stpTrans->getFieldU32(sfOperationLimit) != ctx.app.config().NETWORK_ID)
    {
        JLOG(ctx.j.warn()) << "Import: Wrong network ID for OperationLimit in "
                              "inner txn. outer txid: "
                           << tx.getTransactionID();
        return telWRONG_NETWORK;
    }

    // check if the inner transaction is signed using the same keying as the
    // outer txn
    {
        auto outer = tx.getSigningPubKey();
        auto inner = stpTrans->getSigningPubKey();

        if (outer.empty() && inner.empty())
        {
            // check signer list
            bool const outerHasSigners = tx.isFieldPresent(sfSigners);
            bool const innerHasSigners = stpTrans->isFieldPresent(sfSigners);

            if (outerHasSigners && innerHasSigners)
            {
                auto const& outerSigners = tx.getFieldArray(sfSigners);
                auto const& innerSigners = stpTrans->getFieldArray(sfSigners);

                bool ok = outerSigners.size() == innerSigners.size() &&
                    innerSigners.size() > 1;
                for (uint64_t i = 0; ok && i < outerSigners.size(); ++i)
                {
                    if (outerSigners[i].getAccountID(sfAccount) !=
                            innerSigners[i].getAccountID(sfAccount) ||
                        outerSigners[i].getFieldVL(sfSigningPubKey) !=
                            innerSigners[i].getFieldVL(sfSigningPubKey))
                        ok = false;
                }

                if (!ok)
                {
                    JLOG(ctx.j.warn()) << "Import: outer and inner txns were "
                                          "(multi) signed with different keys. "
                                       << tx.getTransactionID();
                    return temMALFORMED;
                }
            }
            else
            {
                JLOG(ctx.j.warn())
                    << "Import: outer or inner txn was missing signers. "
                    << tx.getTransactionID();
                return temMALFORMED;
            }
        }
        else if (outer != inner)
        {
            JLOG(ctx.j.warn()) << "Import: outer and inner txns were signed "
                                  "with different keys. "
                               << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    // check inner txns signature
    // we do this with a custom ruleset which should be kept up to date with
    // network 0's signing rules
    const std::unordered_set<uint256, beast::uhash<>> rulesFeatures{
        featureExpandedSignerList};
    if (!stpTrans->checkSign(
            STTx::RequireFullyCanonicalSig::yes, Rules(rulesFeatures)))
    {
        JLOG(ctx.j.warn()) << "Import: inner txn signature verify failed "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // execution to here means that:
    // 1. the proof is for the same account that submitted the proof
    // 2. the inner txn has been checked for validity
    // 3. if the xpop itself is proven then the txn can go to preclaim

    //
    // XPOP verify
    //

    // check it was used to sign over the manifest
    auto const m = deserializeManifest(base64_decode(
        (*xpop)[jss::validation][jss::unl][jss::manifest].asString()));

    if (!m)
    {
        JLOG(ctx.j.warn()) << "Import: failed to deserialize manifest on txid "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // we will check the master key matches a known one in preclaim, because the
    // import vl key might be from the on-ledger object
    if (m->masterKey != masterVLKey)
    {
        JLOG(ctx.j.warn()) << "Import: manifest master key did not match top "
                              "level master key in unl section of xpop "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (!m->verify())
    {
        JLOG(ctx.j.warn()) << "Import: manifest signature invalid "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // manifest signing (ephemeral) key
    auto const signingKey = m->signingKey;

    // decode blob
    auto const data =
        base64_decode((*xpop)[jss::validation][jss::unl][jss::blob].asString());

    Json::Reader r;
    Json::Value list;
    if (!r.parse(data, list))
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob was not valid json (after base64 decoding) "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (!list.isMember(jss::sequence) || !list[jss::sequence].isInt())
    {
        JLOG(ctx.j.warn()) << "Import: unl blob json (after base64 decoding) "
                              "lacked required field (sequence) and/or types "
                           << tx.getTransactionID();
        return temMALFORMED;
    }
    if (!list.isMember(jss::expiration) || !list[jss::expiration].isInt())
    {
        JLOG(ctx.j.warn()) << "Import: unl blob json (after base64 decoding) "
                              "lacked required field (expiration) and/or types "
                           << tx.getTransactionID();
        return temMALFORMED;
    }
    if (list.isMember(jss::effective) && !list[jss::effective].isInt())
    {
        JLOG(ctx.j.warn()) << "Import: unl blob json (after base64 decoding) "
                              "lacked required field (effective) and/or types "
                           << tx.getTransactionID();
        return temMALFORMED;
    }
    if (!list.isMember(jss::validators) || !list[jss::validators].isArray())
    {
        JLOG(ctx.j.warn()) << "Import: unl blob json (after base64 decoding) "
                              "lacked required field (validators) and/or types "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const validFrom = TimeKeeper::time_point{TimeKeeper::duration{
        list.isMember(jss::effective) ? list[jss::effective].asUInt() : 0}};
    auto const validUntil = TimeKeeper::time_point{
        TimeKeeper::duration{list[jss::expiration].asUInt()}};
    auto const now = ctx.app.timeKeeper().now();
    if (validUntil <= validFrom)
    {
        JLOG(ctx.j.warn()) << "Import: unl blob validUntil <= validFrom "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (validUntil <= now)
    {
        JLOG(ctx.j.warn()) << "Import: unl blob expired "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (validFrom > now)
    {
        JLOG(ctx.j.warn()) << "Import: unl blob not yet valid "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const sig =
        strUnHex((*xpop)[jss::validation][jss::unl][jss::signature].asString());
    if (!sig || !ripple::verify(signingKey, makeSlice(data), makeSlice(*sig)))
    {
        JLOG(ctx.j.warn()) << "Import: unl blob not signed correctly "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const tx_hash =
        stpTrans->getTransactionID();  // sha512Half(HashPrefix::transactionID,
                                       // *rawTx);

    JLOG(ctx.j.trace()) << "tx_hash (computed): " << tx_hash;

    auto rawTx = strUnHex((*xpop)[jss::transaction][jss::blob].asString());
    auto const tx_meta =
        strUnHex((*xpop)[jss::transaction][jss::meta].asString());
    Serializer s(rawTx->size() + tx_meta->size() + 40);
    s.addVL(*rawTx);
    s.addVL(*tx_meta);
    s.addBitString(tx_hash);

    uint256 const computed_tx_hash_and_meta =
        sha512Half(HashPrefix::txNode, s.slice());

    // check if the proof is inside the proof tree/list
    if (!([](Json::Value const& proof, std::string hash) -> bool {
            auto const proofContains = [](Json::Value const* proof,
                                          std::string hash,
                                          int depth = 0,
                                          auto proofContains =
                                              nullptr) -> bool {
                if (depth > 32)
                    return false;

                if (!proof->isObject() && !proof->isArray())
                    return false;

                if (proof->isMember(jss::children))
                    proof = &((*proof)[jss::children]);

                for (int x = 0; x < 16; ++x)
                {
                    Json::Value const* entry = proof->isObject()
                        ? &((*proof)[std::string(1, "0123456789ABCDEF"[x])])
                        : &((*proof)[x]);

                    if (entry->isNull())
                        continue;

                    if ((entry->isString() && entry->asString() == hash) ||
                        (entry->isObject() && entry->isMember(jss::hash) &&
                         (*entry)[jss::hash] == hash) ||
                        proofContains(entry, hash, depth + 1, proofContains))
                        return true;
                }

                return false;
            };

            return proofContains(&proof, hash, 0, proofContains);
        })((*xpop)[jss::transaction][jss::proof],
           strHex(computed_tx_hash_and_meta)))
    {
        JLOG(ctx.j.warn())
            << "Import: xpop proof did not contain the specified txn hash "
            << strHex(computed_tx_hash_and_meta) << " submitted in Import TXN "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // compute the merkel root over the proof
    uint256 const computedTxRoot = ([](Json::Value const& proof) -> uint256 {
        auto hashProof = [](Json::Value const& proof,
                            int depth = 0,
                            auto const& hashProof = nullptr) -> uint256 {
            const uint256 nullhash;

            if (depth > 32)
                return nullhash;

            if (!proof.isObject() && !proof.isArray())
                return nullhash;

            sha512_half_hasher h;
            using beast::hash_append;
            hash_append(h, ripple::HashPrefix::innerNode);

            if (proof.isArray())
            {
                for (const auto& entry : proof)
                {
                    if (entry.isString())
                    {
                        uint256 hash;
                        if (hash.parseHex(entry.asString()))
                            hash_append(h, hash);
                    }
                    else
                        hash_append(h, hashProof(entry, depth + 1, hashProof));
                }
            }
            else if (proof.isObject())
            {
                for (int x = 0; x < 16; ++x)
                {
                    // Duplicate / Sanity
                    std::string const nibble(1, "0123456789ABCDEF"[x]);
                    if (!proof[jss::children].isMember(nibble))
                        hash_append(h, nullhash);
                    else if (
                        proof[jss::children][nibble][jss::children].size() ==
                        0u)
                    {
                        uint256 hash;
                        if (hash.parseHex(
                                proof[jss::children][nibble][jss::hash]
                                    .asString()))
                            hash_append(h, hash);
                    }
                    else
                        hash_append(
                            h,
                            hashProof(
                                proof[jss::children][nibble],
                                depth + 1,
                                hashProof));
                }
            }
            return static_cast<uint256>(h);
        };
        return hashProof(proof, 0, hashProof);
    })((*xpop)[jss::transaction][jss::proof]);

    auto const& lgr = (*xpop)[jss::ledger];
    if (strHex(computedTxRoot) != lgr[jss::txroot])
    {
        JLOG(ctx.j.warn()) << "Import: computed txroot does not match xpop "
                              "txroot, invalid xpop. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    auto coins = parse_uint64(lgr[jss::coins].asString());
    uint256 phash, acroot;

    // Duplicate / Sanity - syntaxCheckXPOP
    if (!coins || !phash.parseHex(lgr[jss::phash].asString()) ||
        !acroot.parseHex(lgr[jss::acroot].asString()))
    {
        JLOG(ctx.j.warn()) << "Import: error parsing coins | phash | acroot in "
                              "the ledger section of XPOP. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // compute ledger
    uint256 computedLedgerHash = sha512Half(
        HashPrefix::ledgerMaster,
        std::uint32_t(lgr[jss::index].asUInt()),
        *coins,
        phash,
        computedTxRoot,
        acroot,
        std::uint32_t(lgr[jss::pclose].asUInt()),
        std::uint32_t(lgr[jss::close].asUInt()),
        std::uint8_t(lgr[jss::cres].asUInt()),
        std::uint8_t(lgr[jss::flags].asUInt()));

    //
    // validation section
    //

    std::map<std::string /* nodepub */, std::string /* public key */>
        validators;
    std::map<std::string /* master pubkey */, std::string /* nodepub */>
        validatorsMaster;

    uint64_t totalValidatorCount{0};

    // parse the validator list
    for (auto const& val : list[jss::validators])
    {
        totalValidatorCount++;

        if (!val.isObject() || !val.isMember(jss::validation_public_key) ||
            !val[jss::validation_public_key].isString() ||
            !val.isMember(jss::manifest) || !val[jss::manifest].isString())
        {
            JLOG(ctx.j.warn()) << "Import: unl blob contained invalid "
                                  "validator entry, skipping "
                               << tx.getTransactionID();
            continue;
        }

        std::optional<Blob> const ret =
            strUnHex(val[jss::validation_public_key].asString());

        if (!ret || !publicKeyType(makeSlice(*ret)))
        {
            JLOG(ctx.j.warn()) << "Import: unl blob contained an invalid "
                                  "validator key, skipping "
                               << val[jss::validation_public_key].asString()
                               << " " << tx.getTransactionID();
            continue;
        }

        auto const m =
            deserializeManifest(base64_decode(val[jss::manifest].asString()));

        if (!m)
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob contained an invalid manifest, skipping "
                << tx.getTransactionID();
            continue;
        }

        if (strHex(m->masterKey) != val[jss::validation_public_key])
        {
            JLOG(ctx.j.warn()) << "Import: unl blob list entry manifest master "
                                  "key did not match master key, skipping "
                               << tx.getTransactionID();
            continue;
        }

        if (!m->verify())
        {
            JLOG(ctx.j.warn()) << "Import: unl blob list entry manifest "
                                  "signature invalid, skipping "
                               << tx.getTransactionID();
            continue;
        }

        std::string const nodepub =
            toBase58(TokenType::NodePublic, m->signingKey);
        std::string const nodemaster =
            toBase58(TokenType::NodePublic, m->masterKey);
        validators[nodepub] = strHex(m->signingKey);
        validatorsMaster[nodemaster] = nodepub;
    }

    JLOG(ctx.j.trace()) << "totalValidatorCount: " << totalValidatorCount;

    uint64_t quorum = totalValidatorCount * 0.8;

    if (quorum == 0)
    {
        JLOG(ctx.j.warn()) << "Import: resetting quorum to 1. "
                           << tx.getTransactionID();
        quorum = 1;
    }

    // count how many validations this ledger hash has

    uint64_t validationCount{0};
    {
        auto const& data = (*xpop)[jss::validation][jss::data];
        std::set<std::string> used_key;
        for (const auto& key : data.getMemberNames())
        {
            auto nodepub = key;
            auto const datakey = nodepub;

            // if the specified node address (nodepub) is in the master address
            // => regular address list then make a note and replace it with the
            // regular address
            auto regular = validatorsMaster.find(nodepub);
            if (regular != validatorsMaster.end() &&
                validators.find(regular->second) != validators.end())
            {
                used_key.emplace(nodepub);
                nodepub = regular->second;
            }

            auto const signingKey = validators.find(nodepub);
            if (signingKey == validators.end())
            {
                JLOG(ctx.j.trace()) << "Import: validator nodepub " << nodepub
                                    << " did not appear in validator list but "
                                       "did appear in data section "
                                    << tx.getTransactionID();
                continue;
            }

            if (used_key.find(nodepub) != used_key.end())
            {
                JLOG(ctx.j.trace())
                    << "Import: validator nodepub " << nodepub
                    << " key appears more than once in data section "
                    << tx.getTransactionID();
                continue;
            }

            used_key.emplace(nodepub);

            // process the validation message
            try
            {
                std::unique_ptr<STValidation> val;
                auto const valBlob = strUnHex(data[datakey].asString());

                if (!valBlob)
                {
                    JLOG(ctx.j.warn())
                        << "Import: validation inside xpop was not valid hex "
                        << "nodepub: " << nodepub
                        << " txid: " << tx.getTransactionID();
                    continue;
                }

                SerialIter sit(makeSlice(*valBlob));
                val = std::make_unique<STValidation>(
                    std::ref(sit),
                    [](PublicKey const& pk) { return calcNodeID(pk); },
                    false);

                if (val->getLedgerHash() != computedLedgerHash)
                {
                    JLOG(ctx.j.warn()) << "Import: validation message was not "
                                          "for computed ledger hash "
                                       << computedLedgerHash << " it was for "
                                       << val->getLedgerHash();
                    continue;
                }

                if (!(strHex(val->getSignerPublic()) == signingKey->second))
                {
                    JLOG(ctx.j.warn())
                        << "Import: validation inside xpop was not signed with "
                           "a signing key we recognise "
                        << "despite being listed against a nodepub we "
                           "recognise. "
                        << "nodepub: " << nodepub
                        << ". txid: " << tx.getTransactionID();
                    continue;
                }

                // signature check is expensive hence done after checking
                // everything else
                if (!val->isValid())
                {
                    JLOG(ctx.j.warn()) << "Import: validation inside xpop was "
                                          "not correctly signed "
                                       << "nodepub: " << nodepub
                                       << " txid: " << tx.getTransactionID();
                    continue;
                }

                validationCount++;
            }
            catch (...)
            {
                JLOG(ctx.j.warn()) << "Import: validation inside xpop was not "
                                      "able to be parsed "
                                   << "nodepub: " << nodepub
                                   << " txid: " << tx.getTransactionID();
                continue;
            }
        }
    }

    JLOG(ctx.j.trace()) << "quorum: " << quorum
                        << " validation count: " << validationCount;

    // check if the validation count is adequate
    auto hasInsufficientQuorum =
        [&ctx](uint64_t quorum, uint64_t validationCount) {
            if (ctx.rules.enabled(fixXahauV1))
            {
                return quorum > validationCount;
            }
            else
            {
                return quorum >= validationCount;
            }
        };
    if (hasInsufficientQuorum(quorum, validationCount))
    {
        JLOG(ctx.j.warn()) << "Import: xpop did not contain an 80% quorum for "
                              "the txn it purports to prove. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    // Duplicate / Sanity
    if (!stpTrans->isFieldPresent(sfSequence) ||
        !stpTrans->isFieldPresent(sfFee) ||
        !isXRP(stpTrans->getFieldAmount(sfFee)))
    {
        JLOG(ctx.j.warn()) << "Import: xpop inner txn did not contain a "
                              "sequence number or fee. "
                           << tx.getTransactionID();
        return temMALFORMED;
    }

    if (stpTrans->getFieldAmount(sfFee) < beast::zero)
    {
        JLOG(ctx.j.warn()) << "Import: xpop contained negative fee. "
                           << tx.getTransactionID();
        return temBAD_FEE;
    }

    return preflight2(ctx);
}

TER
Import::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureImport))
        return temDISABLED;

    if (!ctx.tx.isFieldPresent(sfBlob))
        return tefINTERNAL;

    // parse blob as json
    auto const xpop = syntaxCheckXPOP(ctx.tx.getFieldVL(sfBlob), ctx.j);

    if (!xpop)
    {
        JLOG(ctx.j.warn())
            << "Import: during preclaim could not parse xpop, bailing.";
        return tefINTERNAL;
    }

    auto const [stpTrans, meta] = getInnerTxn(ctx.tx, ctx.j, &(*xpop));

    if (!stpTrans || !meta || !stpTrans->isFieldPresent(sfSequence))
    {
        JLOG(ctx.j.warn()) << "Import: during preclaim could not find "
                              "importSequence, bailing.";
        return tefINTERNAL;
    }

    auto const& sle = ctx.view.read(keylet::account(ctx.tx[sfAccount]));

    auto const tt = stpTrans->getTxnType();
    if ((tt == ttSIGNER_LIST_SET || tt == ttREGULAR_KEY_SET) &&
        ctx.view.rules().enabled(fixReduceImport) && sle)
    {
        // blackhole check
        do
        {
            // if master key is not set then it is not blackholed
            if (!(sle->getFlags() & lsfDisableMaster))
                break;

            // if a regular key is set then it must be acc 0, 1, or 2 otherwise
            // not blackholed
            if (sle->isFieldPresent(sfRegularKey))
            {
                AccountID rk = sle->getAccountID(sfRegularKey);
                static const AccountID ACCOUNT_ZERO(0);
                static const AccountID ACCOUNT_ONE(1);
                static const AccountID ACCOUNT_TWO(2);

                if (rk != ACCOUNT_ZERO && rk != ACCOUNT_ONE &&
                    rk != ACCOUNT_TWO)
                    break;
            }

            // if a signer list is set then it's not blackholed
            auto const signerListKeylet = keylet::signers(ctx.tx[sfAccount]);
            if (ctx.view.exists(signerListKeylet))
                break;

            // execution to here means it's blackholed
            JLOG(ctx.j.warn())
                << "Import: during preclaim target account is blackholed "
                << ctx.tx[sfAccount] << ", bailing.";
            return tefIMPORT_BLACKHOLED;
        } while (0);
    }

    if (sle && sle->isFieldPresent(sfImportSequence))
    {
        uint32_t sleImportSequence = sle->getFieldU32(sfImportSequence);

        // replay attempt
        if (sleImportSequence >= stpTrans->getFieldU32(sfSequence))
            return tefPAST_IMPORT_SEQ;
    }

    // when importing for the first time the fee must be zero
    if (!sle && ctx.tx.getFieldAmount(sfFee) != beast::zero)
        return temBAD_FEE;

    auto const vlInfo = getVLInfo(*xpop, ctx.j);

    if (!vlInfo)
    {
        JLOG(ctx.j.warn())
            << "Import: during preclaim could not parse vlInfo, bailing.";
        return tefINTERNAL;
    }

    auto const& sleVL = ctx.view.read(keylet::import_vlseq(vlInfo->second));
    if (sleVL && sleVL->getFieldU32(sfImportSequence) > vlInfo->first)
    {
        JLOG(ctx.j.warn())
            << "Import: import vl sequence already used, bailing.";
        return tefPAST_IMPORT_VL_SEQ;
    }

    // check master VL key
    std::string strPk =
        (*xpop)[jss::validation][jss::unl][jss::public_key].asString();

    if (auto const& found = ctx.app.config().IMPORT_VL_KEYS.find(strPk);
        found != ctx.app.config().IMPORT_VL_KEYS.end())
        return tesSUCCESS;

    auto pkHex = strUnHex(strPk);
    if (!pkHex)
        return tefINTERNAL;

    auto const pkType = publicKeyType(makeSlice(*pkHex));
    if (!pkType)
        return tefINTERNAL;

    PublicKey const pk(makeSlice(*pkHex));

    // check on ledger
    if (auto const unlRep = ctx.view.read(keylet::UNLReport()); unlRep)
    {
        auto const& vlKeys = unlRep->getFieldArray(sfImportVLKeys);
        for (auto const& k : vlKeys)
            if (PublicKey(k[sfPublicKey]) == pk)
                return tesSUCCESS;
    }

    JLOG(ctx.j.warn()) << "Import: import vl key not recognized, bailing.";
    return telIMPORT_VL_KEY_NOT_RECOGNISED;
}

void
Import::doSignerList(std::shared_ptr<SLE>& sle, STTx const& stpTrans)
{
    AccountID id = stpTrans.getAccountID(sfAccount);

    JLOG(ctx_.journal.trace()) << "Import: doSignerList acc: " << id;

    if (!stpTrans.isFieldPresent(sfSignerQuorum))
    {
        JLOG(ctx_.journal.warn())
            << "Import: acc " << id
            << " tried to import signerlist without sfSignerQuorum, skipping";
        return;
    }

    Sandbox sb(&view());

    uint32_t quorum = stpTrans.getFieldU32(sfSignerQuorum);

    if (quorum == 0)
    {
        // delete operation

        TER result =
            SetSignerList::removeFromLedger(ctx_.app, sb, id, ctx_.journal);
        if (isTesSuccess(result))
        {
            JLOG(ctx_.journal.warn())
                << "Import: successful destroy SignerListSet";
            sb.apply(ctx_.rawView());
        }
        else
        {
            JLOG(ctx_.journal.warn())
                << "Import: SetSignerList destroy failed with code " << result
                << " acc: " << id;
        }
        return;
    }

    if (!stpTrans.isFieldPresent(sfSignerEntries) ||
        stpTrans.getFieldArray(sfSignerEntries).empty())
    {
        JLOG(ctx_.journal.warn())
            << "Import: SetSignerList lacked populated array and quorum was "
               "non-zero. Ignoring. acc: "
            << id;
        return;
    }

    //
    // extract signer entires and sort them
    //

    std::vector<ripple::SignerEntries::SignerEntry> signers;
    auto const entries = stpTrans.getFieldArray(sfSignerEntries);
    signers.reserve(entries.size());
    for (auto const& e : entries)
    {
        if (!e.isFieldPresent(sfAccount) || !e.isFieldPresent(sfSignerWeight))
        {
            JLOG(ctx_.journal.warn())
                << "Import: SignerListSet entry lacked a required field "
                   "(Account/SignerWeight). "
                << "Skipping SignerListSet.";
            return;
        }

        std::optional<uint256> tag;
        if (e.isFieldPresent(sfWalletLocator) &&
            ctx_.view().rules().enabled(featureExpandedSignerList))
            tag = e.getFieldH256(sfWalletLocator);

        signers.emplace_back(
            e.getAccountID(sfAccount), e.getFieldU16(sfSignerWeight), tag);
    }
    std::sort(signers.begin(), signers.end());

    //
    // validate signer list
    //

    JLOG(ctx_.journal.warn()) << "Import: actioning SignerListSet "
                              << "quorum: " << quorum << " "
                              << "size: " << signers.size();

    if (SetSignerList::validateQuorumAndSignerEntries(
            quorum, signers, id, ctx_.journal, ctx_.view().rules()) !=
        tesSUCCESS)
    {
        JLOG(ctx_.journal.warn())
            << "Import: validation of signer entries failed acc: " << id
            << ". Skipping.";
        return;
    }

    //
    // install signerlist
    //

    // RH NOTE: this handles the ownercount
    TER result = SetSignerList::replaceSignersFromLedger(
        ctx_.app,
        sb,
        ctx_.journal,
        id,
        quorum,
        signers,
        sle->getFieldAmount(sfBalance).xrp());

    if (isTesSuccess(result))
    {
        JLOG(ctx_.journal.warn()) << "Import: successful set SignerListSet";
        sb.apply(ctx_.rawView());
    }
    else
    {
        JLOG(ctx_.journal.warn())
            << "Import: SetSignerList set failed with code " << result
            << " acc: " << id;
    }
    return;
}

void
Import::doRegularKey(std::shared_ptr<SLE>& sle, STTx const& stpTrans)
{
    AccountID id = stpTrans.getAccountID(sfAccount);

    JLOG(ctx_.journal.trace()) << "Import: doRegularKey acc: " << id;

    if (stpTrans.getFieldU16(sfTransactionType) != ttREGULAR_KEY_SET)
    {
        JLOG(ctx_.journal.warn())
            << "Import: doRegularKey called on non-regular key transaction.";
        return;
    }

    if (!stpTrans.isFieldPresent(sfRegularKey))
    {
        // delete op
        JLOG(ctx_.journal.trace()) << "Import: clearing SetRegularKey "
                                   << " acc: " << id;
        if (sle->isFieldPresent(sfRegularKey))
            sle->makeFieldAbsent(sfRegularKey);
        return;
    }

    AccountID rk = stpTrans.getAccountID(sfRegularKey);
    JLOG(ctx_.journal.trace())
        << "Import: actioning SetRegularKey " << rk << " acc: " << id;
    sle->setAccountID(sfRegularKey, rk);

    // always set this flag if they have done any regular keying
    sle->setFlag(lsfPasswordSpent);

    ctx_.view().update(sle);

    return;
}

TER
Import::doApply()
{
    if (!view().rules().enabled(featureImport))
        return temDISABLED;

    if (!ctx_.tx.isFieldPresent(sfBlob))
        return tefINTERNAL;

    //
    // Before starting decode and validate XPOP, update ImportVL seq
    //
    auto const xpop = syntaxCheckXPOP(ctx_.tx.getFieldVL(sfBlob), ctx_.journal);

    if (!xpop)
        return tefINTERNAL;

    auto const infoVL = getVLInfo(*xpop, ctx_.journal);

    if (!infoVL)
        return tefINTERNAL;

    auto const keyletVL = keylet::import_vlseq(infoVL->second);
    auto sleVL = view().peek(keyletVL);

    if (!sleVL)
    {
        // create VL import seq counter
        JLOG(ctx_.journal.trace())
            << "Import: create vl seq - insert import sequence + public key";
        sleVL = std::make_shared<SLE>(keyletVL);
        sleVL->setFieldU32(sfImportSequence, infoVL->first);
        sleVL->setFieldVL(sfPublicKey, infoVL->second.slice());
        view().insert(sleVL);
    }
    else
    {
        uint32_t current = sleVL->getFieldU32(sfImportSequence);

        if (current > infoVL->first)
        {
            // should never happen
            return tefINTERNAL;
        }
        else if (infoVL->first > current)
        {
            // perform an update because the sequence number is newer
            sleVL->setFieldU32(sfImportSequence, infoVL->first);
            view().update(sleVL);
        }
        else
        {
            // it's the same sequence number so leave it be
        }
    }

    auto const [stpTrans, meta] = getInnerTxn(ctx_.tx, ctx_.journal, &(*xpop));

    if (!stpTrans || !stpTrans->isFieldPresent(sfSequence) ||
        !stpTrans->isFieldPresent(sfFee) || !meta ||
        !meta->isFieldPresent(sfTransactionResult))
    {
        JLOG(ctx_.journal.warn())
            << "Import: during apply could not find one of: importSequence, "
               "meta, tx result or fee, bailing.";
        return tefINTERNAL;
    }

    //
    // Now deal with the account creation and crediting
    //

    STAmount burn = stpTrans->getFieldAmount(sfFee);

    if (!isXRP(burn) || burn < beast::zero)
    {
        JLOG(ctx_.journal.warn()) << "Import: inner fee was not XRP value.";
        return tefINTERNAL;
    }

    // ensure header is not going to overflow
    if (burn <= beast::zero ||
        burn.xrp() + view().info().drops < view().info().drops)
    {
        JLOG(ctx_.journal.warn()) << "Import: ledger header overflowed\n";
        return tecINTERNAL;
    }

    uint32_t importSequence = stpTrans->getFieldU32(sfSequence);
    auto const id = ctx_.tx[sfAccount];
    auto sle = view().peek(keylet::account(id));

    if (sle && sle->getFieldU32(sfImportSequence) >= importSequence)
    {
        // make double sure import seq hasn't passed
        JLOG(ctx_.journal.warn()) << "Import: ImportSequence passed";
        return tefINTERNAL;
    }

    // get xahau genesis start ledger, or just assume the current ledger is the
    // start seq if it's not set.
    uint32_t curLgrSeq = view().info().seq;
    uint32_t startLgrSeq = curLgrSeq;
    auto sleFees = view().peek(keylet::fees());
    if (sleFees && sleFees->isFieldPresent(sfXahauActivationLgrSeq))
        startLgrSeq = sleFees->getFieldU32(sfXahauActivationLgrSeq);

    uint32_t elapsed = curLgrSeq - startLgrSeq;

    bool const create = !sle;

    XRPAmount const bonusAmount = Import::computeStartingBonus(ctx_.view());
    STAmount startBal =
        create ? STAmount(bonusAmount) : STAmount(mSourceBalance);

    uint64_t creditDrops = burn.xrp().drops();

    if (view().rules().enabled(featureZeroB2M))
    {
        // B2M xrp is disabled by amendment
        creditDrops = 0;
    }
    else if (elapsed < 2'000'000)
    {
        // first 2MM ledgers
        // the ratio is 1:1
    }
    else if (elapsed < 30'000'000)
    {
        // there is a linear decline over 28MM ledgers
        double x = elapsed - 2000000.0;
        double y = 1.0 - x / 28000000.0;
        y = std::clamp(y, 0.0, 1.0);
        creditDrops *= y;
    }
    else
    {
        // thereafter
        // B2M xrp is disabled
        creditDrops = 0;
    }

    STAmount finalBal = startBal + STAmount(XRPAmount(creditDrops));

    if (finalBal < startBal)
    {
        JLOG(ctx_.journal.warn()) << "Import: overflow finalBal < startBal.";
        return tefINTERNAL;
    }

    if (create)
    {
        // Create the account.
        std::uint32_t const seqno{
            view().rules().enabled(featureXahauGenesis)
                ? view().info().parentCloseTime.time_since_epoch().count()
                : view().rules().enabled(featureDeletableAccounts)
                    ? view().seq()
                    : 1};

        sle = std::make_shared<SLE>(keylet::account(id));
        sle->setAccountID(sfAccount, id);

        sle->setFieldU32(sfSequence, seqno);
        sle->setFieldU32(sfOwnerCount, 0);
        if (sleFees && view().rules().enabled(featureXahauGenesis))
        {
            uint64_t accIdx = sleFees->isFieldPresent(sfAccountCount)
                ? sleFees->getFieldU64(sfAccountCount)
                : 0;
            sle->setFieldU64(sfAccountIndex, accIdx);
            sleFees->setFieldU64(sfAccountCount, accIdx + 1);
        }

        if (ctx_.tx.getSigningPubKey().empty() ||
            calcAccountID(PublicKey(makeSlice(ctx_.tx.getSigningPubKey()))) !=
                id)
        {
            // disable master unless the first Import is signed with master
            sle->setFieldU32(sfFlags, lsfDisableMaster);
            JLOG(ctx_.journal.warn())
                << "Import: acc " << id << " created with disabled master key.";
        }
    }

    sle->setFieldU32(sfImportSequence, importSequence);
    sle->setFieldAmount(sfBalance, finalBal);

    if (create)
    {
        view().insert(sle);
        if (sleFees)
            view().update(sleFees);
    }
    else
        view().update(sle);

    //
    // Handle any key imports, but only if a tes code
    // these functions update the sle on their own
    //
    if (isTesSuccess(meta->getFieldU8(sfTransactionResult)))
    {
        auto const tt = stpTrans->getTxnType();
        if (tt == ttSIGNER_LIST_SET)
            doSignerList(sle, *stpTrans);
        else if (tt == ttREGULAR_KEY_SET)
            doRegularKey(sle, *stpTrans);
    }

    // update the ledger header
    STAmount added = create ? finalBal : finalBal - startBal;
    ctx_.rawView().rawDestroyXRP(-added.xrp());

    return tesSUCCESS;
}

XRPAmount
Import::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    if (!view.exists(keylet::account(tx.getAccountID(sfAccount))) &&
        !tx.isFieldPresent(sfIssuer))
        return XRPAmount{0};

    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
