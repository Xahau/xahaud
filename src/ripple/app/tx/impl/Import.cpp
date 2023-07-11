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

#include <ripple/protocol/Import.h>
#include <ripple/app/tx/impl/Import.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <charconv>
#include <ripple/json/json_value.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STTx.h>
#include <ripple/basics/base64.h>
#include <ripple/app/misc/Manifest.h>
#include <ripple/app/tx/impl/SetSignerList.h>

namespace ripple {

TxConsequences
Import::makeTxConsequences(PreflightContext const& ctx)
{
    auto calculate = [](PreflightContext const& ctx) -> XRPAmount
    {
        auto const [inner, meta] = getInnerTxn(ctx.tx, ctx.j);
        if (!inner || !inner->isFieldPresent(sfFee))
            return beast::zero;

        if (!meta)
            return beast::zero;

        auto const result = meta->getFieldU8(sfTransactionResult);
        if (result != tesSUCCESS && !(result >= tecCLAIM && result <= tecLAST_POSSIBLE_ENTRY))
            return beast::zero;

        STAmount const innerFee = inner->getFieldAmount(sfFee);
        if (!isXRP(innerFee))
            return beast::zero;

        return innerFee.xrp();
    };

    return TxConsequences{ctx.tx, calculate(ctx)};
}


std::pair<
    std::unique_ptr<STTx const>,            // txn
    std::unique_ptr<STObject const>>        // meta
Import::getInnerTxn(STTx const& outer, beast::Journal const& j,Json::Value const* xpop)
{
    // parse blob as json

    std::optional<Json::Value> xpop_storage;

    if (!xpop)
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
        JLOG(j.warn())
            << "Import: failed to deserialize tx blob inside xpop (invalid hex) "
            << outer.getTransactionID();
        return {};
    }

    if (!meta)
    {
        JLOG(j.warn())
            << "Import: failed to deserialize tx meta inside xpop (invalid hex) "
            << outer.getTransactionID();
        return {};
    }

    try
    {
        return {
            std::make_unique<STTx const>(SerialIter { rawTx->data(), rawTx->size() }),
            std::make_unique<STObject const>(SerialIter(meta->data(), meta->size()), sfMetadata)
        };
    }
    catch (std::exception& e)
    {
        JLOG(j.warn())
            << "Import: failed to deserialize tx blob/meta inside xpop ("
            << e.what()
            << ") outer txid: "
            << outer.getTransactionID();
        return {};
    }
}

NotTEC
Import::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureImport))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;

    if (tx.getFieldAmount(sfFee) != beast::zero)
    {
        if (tx.isFieldPresent(sfIssuer))
        {
            // pass. Import against a Hook does pay a fee
        }
        else
        {
            JLOG(ctx.j.warn())
                << "Import: sfFee must be 0 "
                << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    if (tx.getFieldVL(sfBlob).size() > (512 * 1024))
    {
        JLOG(ctx.j.warn())
            << "Import: blob was more than 512kib "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const amt = tx[~sfAmount];

    if (amt && !isXRP(*amt))
    {
        JLOG(ctx.j.warn())
            << "Import: sfAmount field must be in drops. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // parse blob as json
    auto const xpop = 
        syntaxCheckXPOP(tx.getFieldVL(sfBlob), ctx.j);

    if (!xpop)
        return temMALFORMED;

    // we will check if we recognise the vl key in preclaim because it may be from on-ledger object
    std::optional<PublicKey> masterVLKey;
    {
        std::string strPk = (*xpop)[jss::validation][jss::unl][jss::public_key].asString();
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
            JLOG(ctx.j.warn())
                << "Import: validation.unl.public_key was not a recognised public key type.";
            return temMALFORMED;
        }

        masterVLKey = PublicKey(makeSlice(*pkHex));
    }

    auto const [stpTrans, meta] = getInnerTxn(tx, ctx.j, &(*xpop));

    if (!stpTrans)
        return temMALFORMED;

    if (stpTrans->isFieldPresent(sfTicketSequence))
    {
        JLOG(ctx.j.warn())
            << "Import: cannot use TicketSequence XPOP.";
        return temMALFORMED;
    }

    // check if txn is emitted or a psuedo
    if (isPseudoTx(*stpTrans) || stpTrans->isFieldPresent(sfEmitDetails))
    {
        JLOG(ctx.j.warn())
            << "Import: attempted to import xpop containing an emitted or pseudo txn. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // ensure that the txn was tesSUCCESS / tec
    if (!meta->isFieldPresent(sfTransactionResult))
    {
        JLOG(ctx.j.warn())
            << "Import: inner txn lacked transaction result... "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    else
    {
        uint8_t innerResult = meta->getFieldU8(sfTransactionResult);

        if (innerResult == tesSUCCESS)
        {
            // pass
        }
        else if (innerResult >= tecCLAIM && innerResult <= tecLAST_POSSIBLE_ENTRY)
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
        JLOG(ctx.j.warn())
            << "Import: import and txn inside xpop must be signed by the same account "
            << tx.getTransactionID();
        return temMALFORMED;
    }


    // ensure inner txn is for networkid = 0 (network id must therefore be missing)
    if (stpTrans->isFieldPresent(sfNetworkID))
    {
        JLOG(ctx.j.warn())
            << "Import: attempted to import xpop containing a txn with a sfNetworkID field. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // ensure inner txn is destined for the network we're on, this is according to OperationLimit field
    if (!stpTrans->isFieldPresent(sfOperationLimit))
    {
        JLOG(ctx.j.warn())
            << "Import: OperationLimit missing from inner xpop txn. outer txid: "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (stpTrans->getFieldU32(sfOperationLimit) != ctx.app.config().NETWORK_ID)
    {
        JLOG(ctx.j.warn())
            << "Import: Wrong network ID for OperationLimit in inner txn. outer txid: "
            << tx.getTransactionID();
        return telWRONG_NETWORK;
    }

/*
    // ensure the inner txn is an accountset or signerlistset
    auto const tt = stpTrans->getTxnType();

    if (tt != ttACCOUNT_SET && tt != ttSIGNER_LIST_SET && tt != ttREGULAR_KEY_SET)
    {
        JLOG(ctx.j.warn())
            << "Import: inner txn must be an AccountSet, SetRegularKey or SignerListSet transaction. "
            << tx.getTransactionID();
        return temMALFORMED;
    }
*/

    // check if the inner transaction is signed using the same keying as the outer txn
    {
        auto outer = tx.getSigningPubKey();
        auto inner = stpTrans->getSigningPubKey();

        if (outer.empty() && inner.empty())
        {
            // check signer list
            bool const outerHasSigners = tx.isFieldPresent(sfSigners);
            bool const innerHasSigners = stpTrans->isFieldPresent(sfSigners);

            if (!(outerHasSigners && innerHasSigners))
            {
                auto const& outerSigners = tx.getFieldArray(sfSigners);
                auto const& innerSigners = stpTrans->getFieldArray(sfSigners);

                bool ok = outerSigners.size() == innerSigners.size();
                for (uint64_t i = 0; ok && i < outerSigners.size(); ++i)
                {
                    if (outerSigners[i].getAccountID(sfAccount) != innerSigners[i].getAccountID(sfAccount) ||
                        outerSigners[i].getFieldVL(sfSigningPubKey) != innerSigners[i].getFieldVL(sfSigningPubKey))
                        ok = false;
                }

                if (!ok)
                {
                    JLOG(ctx.j.warn())
                        << "Import: outer and inner txns were (multi) signed with different keys. "
                        << tx.getTransactionID();
                    return temMALFORMED;
                }

            }

        }
        else if (outer != inner)
        {
            JLOG(ctx.j.warn())
                << "Import: outer and inner txns were signed with different keys. "
                << tx.getTransactionID();
            return temMALFORMED;
        }
    }

    // check inner txns signature
    // we do this with a custom ruleset which should be kept up to date with network 0's signing rules
    const std::unordered_set<uint256, beast::uhash<>> rulesFeatures {featureExpandedSignerList};
    if (!stpTrans->checkSign(STTx::RequireFullyCanonicalSig::yes, Rules(rulesFeatures)))
    {
        JLOG(ctx.j.warn())
            << "Import: inner txn signature verify failed "
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
    auto const m =
        deserializeManifest(base64_decode((*xpop)[jss::validation][jss::unl][jss::manifest].asString()));

    if (!m)
    {
        JLOG(ctx.j.warn())
            << "Import: failed to deserialize manifest on txid "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // we will check the master key matches a known one in preclaim, because the import vl key might be 
    // from the on-ledger object
    if (m->masterKey != masterVLKey)
    {
        JLOG(ctx.j.warn())
            << "Import: manifest master key did not match top level master key in unl section of xpop "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (!m->verify())
    {
        JLOG(ctx.j.warn())
            << "Import: manifest signature invalid "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    

    // manifest signing (ephemeral) key
    auto const signingKey = m->signingKey;

    // decode blob
    auto const data = base64_decode((*xpop)[jss::validation][jss::unl][jss::blob].asString());
    auto const sig = strUnHex((*xpop)[jss::validation][jss::unl][jss::signature].asString());
    if (!sig ||
        !ripple::verify(
            signingKey,
            makeSlice(data),
            makeSlice(*sig)))
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob not signed correctly "
            << tx.getTransactionID();
        return temMALFORMED;
    }

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
        JLOG(ctx.j.warn())
            << "Import: unl blob json (after base64 decoding) lacked required field (sequence) and/or types "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    if (!list.isMember(jss::expiration) || !list[jss::expiration].isInt())
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob json (after base64 decoding) lacked required field (expiration) and/or types "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    if (list.isMember(jss::effective) && !list[jss::effective].isInt())
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob json (after base64 decoding) lacked required field (effective) and/or types "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    if (!list.isMember(jss::validators) || !list[jss::validators].isArray())
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob json (after base64 decoding) lacked required field (validators) and/or types "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const sequence = list[jss::sequence].asUInt();
    auto const validFrom = TimeKeeper::time_point{TimeKeeper::duration{
        list.isMember(jss::effective) ? list[jss::effective].asUInt() : 0}};
    auto const validUntil = TimeKeeper::time_point{
        TimeKeeper::duration{list[jss::expiration].asUInt()}};
    auto const now = ctx.app.timeKeeper().now();
    if (validUntil <= validFrom)
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob validUnil <= validFrom "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (validUntil <= now)
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob expired "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (validFrom > now)
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob not yet valid "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const tx_hash = stpTrans->getTransactionID();//sha512Half(HashPrefix::transactionID, *rawTx);

    JLOG(ctx.j.trace()) << "tx_hash (computed): " << tx_hash;

    auto rawTx = strUnHex((*xpop)[jss::transaction][jss::blob].asString());
    auto const tx_meta = strUnHex((*xpop)[jss::transaction][jss::meta].asString());
    Serializer s(rawTx->size() + tx_meta->size() + 40);
    s.addVL(*rawTx);
    s.addVL(*tx_meta);
    s.addBitString(tx_hash);

    uint256 const computed_tx_hash_and_meta =
        sha512Half(HashPrefix::txNode, s.slice());


    // check if the proof is inside the proof tree/list
    if (!([](Json::Value const& proof, std::string hash) -> bool
    {
        auto const proofContains =
        [](Json::Value const* proof, std::string hash, int depth = 0, auto proofContains = nullptr) -> bool
        {
            if (depth > 32)
                return false;

            if (!proof->isObject() && !proof->isArray())
                return false;

            if (proof->isMember(jss::children))
                proof = &((*proof)[jss::children]);

            for (int x = 0; x < 16; ++x)
            {
                Json::Value const* entry =
                    proof->isObject()
                        ? &((*proof)[std::string(1, "0123456789ABCDEF"[x])])
                        : &((*proof)[x]);

                if (entry->isNull())
                    continue;

                if (entry->isString() && entry->asString() == hash ||
                    entry->isObject() && entry->isMember(jss::hash) && (*entry)[jss::hash] == hash ||
                    proofContains(entry, hash, depth + 1, proofContains))
                    return true;
            }

            return false;
        };

        return proofContains(&proof, hash, 0, proofContains);
    })((*xpop)[jss::transaction][jss::proof], strHex(computed_tx_hash_and_meta)))
    {
        JLOG(ctx.j.warn())
           << "Import: xpop proof did not contain the specified txn hash "
           << strHex(computed_tx_hash_and_meta)
           << " submitted in Import TXN "
           << tx.getTransactionID();
        return temMALFORMED;
    }


    // compute the merkel root over the proof
    uint256 const computedTxRoot =
    ([](Json::Value const& proof) -> uint256
    {
        auto hashProof =
        [](Json::Value const& proof, int depth = 0, auto const& hashProof = nullptr) -> uint256
        {
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
                        hash.parseHex(entry.asString());
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
                    std::string const nibble (1, "0123456789ABCDEF"[x]);
                    if (!proof[jss::children].isMember(nibble))
                        hash_append(h, nullhash);
                    else if (proof[jss::children][nibble][jss::children].size() == 0u)
                    {
                        uint256 hash;
                        hash.parseHex(proof[jss::children][nibble][jss::hash].asString());
                        hash_append(h, hash);
                    }
                    else
                        hash_append(h, hashProof(proof[jss::children][nibble], depth + 1, hashProof));
                }
            }
            return static_cast<uint256>(h);
        };
        return hashProof(proof, 0, hashProof);
    })((*xpop)[jss::transaction][jss::proof]);

    auto const& lgr = (*xpop)[jss::ledger];
    if (strHex(computedTxRoot) != lgr[jss::txroot])
    {
        JLOG(ctx.j.warn())
            << "Import: computed txroot does not match xpop txroot, invalid xpop. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    auto coins = parse_uint64(lgr[jss::coins].asString());
    uint256 phash, acroot;

    // Duplicate / Sanity - syntaxCheckXPOP
    if (!coins ||
        !phash.parseHex(lgr[jss::phash].asString()) ||
        !acroot.parseHex(lgr[jss::acroot].asString()))
    {
        JLOG(ctx.j.warn())
            << "Import: error parsing coins | phash | acroot in the ledger section of XPOP. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // compute ledger
    uint256 computedLedgerHash =
    sha512Half(
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

    std::map<std::string /* nodepub */, std::string /* public key */> validators;
    std::map<std::string /* master pubkey */ , std::string /* nodepub */> validatorsMaster;

    uint64_t totalValidatorCount { 0 };

    // parse the validator list
    for (auto const& val : list[jss::validators])
    {
        totalValidatorCount++;

        if (!val.isObject() ||
            !val.isMember(jss::validation_public_key) ||
            !val[jss::validation_public_key].isString() ||
            !val.isMember(jss::manifest) ||
            !val[jss::manifest].isString())
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob contained invalid validator entry, skipping "
                << tx.getTransactionID();
            continue;
        }

        std::optional<Blob> const ret =
            strUnHex(val[jss::validation_public_key].asString());

        if (!ret || !publicKeyType(makeSlice(*ret)))
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob contained an invalid validator key, skipping "
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
            JLOG(ctx.j.warn())
                << "Import: unl blob list entry manifest master key did not match master key, skipping "
                << tx.getTransactionID();
            continue;
        }

        if (!m->verify())
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob list entry manifest signature invalid, skipping "
                << tx.getTransactionID();
            continue;
        }

        std::string const nodepub = toBase58(TokenType::NodePublic, m->signingKey);
        std::string const nodemaster = toBase58(TokenType::NodePublic, m->masterKey);
        validators[nodepub] = strHex(m->signingKey);
        validatorsMaster[nodemaster] = nodepub;
    }

    JLOG(ctx.j.trace())
        << "totalValidatorCount: " << totalValidatorCount;

    uint64_t quorum = totalValidatorCount * 0.8;

    if (quorum == 0)
    {
        JLOG(ctx.j.warn())
            << "Import: resetting quorum to 1. "
            << tx.getTransactionID();
        quorum = 1;
    }

    // count how many validations this ledger hash has

    uint64_t validationCount { 0 };
    {
        auto const& data = (*xpop)[jss::validation][jss::data];
        std::set<std::string> used_key;

        for (const auto& key : data.getMemberNames())
        {
            auto nodepub = key;
            auto const datakey = nodepub;

            // if the specified node address (nodepub) is in the master address => regular address list
            // then make a note and replace it with the regular address
            auto regular = validatorsMaster.find(nodepub);
            if (regular != validatorsMaster.end() && validators.find(regular->second) != validators.end())
            {
                used_key.emplace(nodepub);
                nodepub = regular->second;
            }

            auto const signingKey = validators.find(nodepub);
            if (signingKey == validators.end())
            {
                JLOG(ctx.j.trace())
                    << "Import: validator nodepub " << nodepub
                    << " did not appear in validator list but did appear in data section "
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
                        << "nodepub: " << nodepub << " txid: "
                        << tx.getTransactionID();
                    continue;
                }

                SerialIter sit(makeSlice(*valBlob));
                val = std::make_unique<STValidation>(
                    std::ref(sit),
                    [](PublicKey const& pk) {
                        return calcNodeID(pk);
                    },
                    false);

                if (val->getLedgerHash() != computedLedgerHash)
                {
                    JLOG(ctx.j.warn())
                        << "Import: validation message was not for computed ledger hash "
                        << computedLedgerHash
                        << " it was for "
                        << val->getLedgerHash();
                    continue;
                }

                if (!(strHex(val->getSignerPublic()) == signingKey->second))
                {
                    JLOG(ctx.j.warn())
                        << "Import: validation inside xpop was not signed with a signing key we recognise "
                        << "despite being listed against a nodepub we recognise. "
                        << "nodepub: " << nodepub << ". txid: " << tx.getTransactionID();
                    continue;
                }

                // signature check is expensive hence done after checking everything else
                if (!val->isValid())
                {
                    JLOG(ctx.j.warn())
                        << "Import: validation inside xpop was not correctly signed "
                        << "nodepub: " << nodepub << " txid: "
                        << tx.getTransactionID();
                    continue;
                }

                validationCount++;

            }
            catch (...)
            {
                JLOG(ctx.j.warn())
                    << "Import: validation inside xpop was not able to be parsed "
                    << "nodepub: " << nodepub << " txid: "
                    << tx.getTransactionID();
                continue;
            }
        }
    }

    JLOG(ctx.j.trace())
        << "quorum: " << quorum
        << " validation count: " << validationCount;

    // check if the validation count is adequate
    if (quorum >= validationCount)
    {
        JLOG(ctx.j.warn())
            << "Import: xpop did not contain an 80% quorum for the txn it purports to prove. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    /*
     * RH TODO: put this in preclaim, place sequence on an unowned object
    else if (sequence < listCollection.current.sequence)
    {
        return ListDisposition::stale;
    }
    else if (sequence == listCollection.current.sequence)
        return ListDisposition::same_sequence;
    */
    //
    // finally in preclaim check the sfImportSequence field on the account
    // if it is less than the Account Sequence in the xpop then mint and
    // update sfImportSequence

    // Duplicate / Sanity
    if (!stpTrans->isFieldPresent(sfSequence) ||
        !stpTrans->isFieldPresent(sfFee) ||
        !isXRP(stpTrans->getFieldAmount(sfFee)))
    {
        JLOG(ctx.j.warn())
            << "Import: xpop inner txn did not contain a sequence number or fee. "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    return preflight2(ctx);
}

// RH TODO: manifest serials should be kept on chain

TER
Import::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureImport))
        return temDISABLED;

    // parse blob as json
    auto const xpop =
        syntaxCheckXPOP(ctx.tx.getFieldVL(sfBlob), ctx.j);
    
    if (!xpop)
    {
        JLOG(ctx.j.warn())
            << "Import: during preclaim could not parse xpop, bailing.";
        return tefINTERNAL;
    }

    auto const [stpTrans, meta] = getInnerTxn(ctx.tx, ctx.j, &(*xpop));

    if (!stpTrans || !stpTrans->isFieldPresent(sfSequence))
    {
        JLOG(ctx.j.warn())
            << "Import: during preclaim could not find importSequence, bailing.";
        return tefINTERNAL;
    }

    auto const& sle = ctx.view.read(keylet::account(ctx.tx[sfAccount]));
    if (sle && sle->isFieldPresent(sfImportSequence))
    {
        uint32_t sleImportSequence = sle->getFieldU32(sfImportSequence);

        // replay attempt
        if (sleImportSequence >= stpTrans->getFieldU32(sfSequence))
            return tefPAST_IMPORT_SEQ;
    }

    auto const vlInfo = getVLInfo(*xpop, ctx.j);

    if (!vlInfo)
        return tefINTERNAL;

    auto const& sleVL = ctx.view.read(keylet::import_vlseq(vlInfo->second));
    
    if (sleVL && sleVL->getFieldU32(sfImportSequence) > vlInfo->first)
        return tefPAST_IMPORT_VL_SEQ;

    // check master VL key
    std::string strPk = (*xpop)[jss::validation][jss::unl][jss::public_key].asString();

    if (auto const& found = ctx.app.config().IMPORT_VL_KEYS.find(strPk);
        found != ctx.app.config().IMPORT_VL_KEYS.end())
        return tesSUCCESS;

    // not found in our local VL keys
    auto pkHex = strUnHex(strPk);
    if (!pkHex)
        return tefINTERNAL;
    
    auto const pkType = publicKeyType(makeSlice(*pkHex));
    if (!pkType)
        return tefINTERNAL;

    PublicKey const pk (makeSlice(*pkHex));    

    // check on ledger
    if (auto const unlRep = ctx.view.read(keylet::UNLReport()); unlRep)
    {
        auto const& vlKeys = unlRep->getFieldArray(sfImportVLKeys);
        for (auto const& k: vlKeys)
            if (PublicKey(k[sfPublicKey]) == pk)
                return tesSUCCESS;
    }

    return telIMPORT_VL_KEY_NOT_RECOGNISED;
}

TER
Import::doApply()
{
    if (!view().rules().enabled(featureImport))
        return temDISABLED;

    if (!ctx_.tx.isFieldPresent(sfBlob))
        return tefINTERNAL;

    auto const xpop =
        syntaxCheckXPOP(ctx_.tx.getFieldVL(sfBlob), ctx_.journal);

    if (!xpop)
        return tefINTERNAL;

    auto const [stpTrans, meta] = getInnerTxn(ctx_.tx, ctx_.journal, &(*xpop));

    if (!stpTrans || !stpTrans->isFieldPresent(sfSequence) || !stpTrans->isFieldPresent(sfFee))
    {
        JLOG(ctx_.journal.warn())
            << "Import: during apply could not find importSequence or fee, bailing.";
        return tefINTERNAL;
    }

    // why aren't transactors stateful, why does this need to be recomputed each time...
    STAmount burn = stpTrans->getFieldAmount(sfFee);

    if (!isXRP(burn) || burn < beast::zero)
    {
        JLOG(ctx_.journal.warn())
            << "Import: inner fee was not XRP value.";
        return tefINTERNAL;
    }

    uint32_t importSequence = stpTrans->getFieldU32(sfSequence);

    auto const id = ctx_.tx[sfAccount];

    auto const k = keylet::account(id);
    auto sle = view().peek(k);

    std::optional<
    std::vector<ripple::SignerEntries::SignerEntry>> setSignerEntries;
    uint32_t setSignerQuorum { 0 };
    std::optional<AccountID> setRegularKey;
    auto const signingKey = ctx_.tx.getSigningPubKey();
    bool const signedWithMaster = !signingKey.empty() && calcAccountID(PublicKey(makeSlice(signingKey))) == id;

    auto const tt = stpTrans->getTxnType();

    // rekeying is only allowed on a tesSUCCESS, but minting is allowed on any tes or tec code.
    if (meta->getFieldU8(sfTransactionResult) == tesSUCCESS)
    {
        if (tt == ttSIGNER_LIST_SET)
        {
            if (stpTrans->isFieldPresent(sfSignerQuorum) &&
                stpTrans->isFieldPresent(sfSignerEntries))
            {
                auto const entries = stpTrans->getFieldArray(sfSignerEntries);

                if (entries.empty())
                {
                    JLOG(ctx_.journal.warn()) << "Import: SignerListSet entires empty, skipping.";
                }
                else
                {
                    JLOG(ctx_.journal.trace()) << "Import: SingerListSet";
                    // key import: signer list
                    setSignerEntries.emplace();
                    setSignerEntries->reserve(entries.size());
                    for (auto const& e: entries)
                    {
                        if (!e.isFieldPresent(sfAccount) || !e.isFieldPresent(sfSignerWeight))
                        {
                            JLOG(ctx_.journal.warn())
                                << "Import: SignerListSet entry lacked a required field (Account/SignerWeight). "
                                << "Skipping SignerListSet.";
                            setSignerEntries = std::nullopt;
                            break;
                        }

                        std::optional<uint256> tag;
                        if (e.isFieldPresent(sfWalletLocator))
                            tag = e.getFieldH256(sfWalletLocator);

                        setSignerEntries->emplace_back(
                            e.getAccountID(sfAccount),
                            e.getFieldU16(sfSignerWeight),
                            tag);
                    }
                    setSignerQuorum = stpTrans->getFieldU32(sfSignerQuorum);
                }
            }
            else
            {
                JLOG(ctx_.journal.warn())
                    << "Import: SingerListSet lacked either populated SignerEntries or SignerQuorum, ignoring.";
            }

        }
        else if (tt == ttREGULAR_KEY_SET)
        {
            JLOG(ctx_.journal.warn()) << "SetRegularKey";
            // key import: regular key
            setRegularKey = stpTrans->getAccountID(sfRegularKey);
        }
    }

    bool const create = !sle;

    // compute the amount they receive first because the amount is maybe needed for computing setsignerlist later
    STAmount startBal = create ? STAmount(INITIAL_IMPORT_XRP) : sle->getFieldAmount(sfBalance);
    STAmount finalBal = startBal + burn;

    // this should never happen
    if (finalBal < startBal)
    {
        JLOG(ctx_.journal.warn())
            << "Import: logic error finalBal < startBal.";
        return tefINTERNAL;
    }

    if (create)
    {
        // Create the account.
        JLOG(ctx_.journal.warn()) << "create - create account";
        std::uint32_t const seqno{
            view().rules().enabled(featureDeletableAccounts) ? view().seq()
                                                             : 1};
        sle = std::make_shared<SLE>(k);
        sle->setAccountID(sfAccount, id);

        sle->setFieldU32(sfImportSequence, importSequence);
        sle->setFieldU32(sfSequence, seqno);
        sle->setFieldU32(sfOwnerCount, 0);

        sle->setFieldAmount(sfBalance, finalBal);

    }
    else if (sle->getFieldU32(sfImportSequence) >= importSequence)
    {
        // make double sure import seq hasn't passed
        JLOG(ctx_.journal.warn())
            << "Import: ImportSequence passed";
        return tefINTERNAL;
    }

    if (setRegularKey)
    {
        JLOG(ctx_.journal.warn()) << "Import: actioning SetRegularKey " << *setRegularKey << " acc: " << id;
        sle->setAccountID(sfRegularKey, *setRegularKey);
    }

    if (create)
    {
        JLOG(ctx_.journal.trace()) << "Import: creating account " << id;
        if (!signedWithMaster)
        {
            // disable master if the account is created using non-master key
            JLOG(ctx_.journal.warn()) << "Import: keying of " << id << " is unclear - disable master";
            sle->setAccountID(sfRegularKey, noAccount());
            sle->setFieldU32(sfFlags, lsfDisableMaster);
        }
        view().insert(sle);
    }
    else
    {
        // account already exists
        JLOG(ctx_.journal.trace()) << "Import: updating existing account " << id;
        sle->setFieldU32(sfImportSequence, importSequence);
        sle->setFieldAmount(sfBalance, finalBal);

        view().update(sle);
    }

    // todo: manifest sequence needs to be recorded to prevent certain types of replay attack
    auto const infoVL = getVLInfo(*xpop, ctx_.journal);

    if (!infoVL)
        return tefINTERNAL;

    auto const keyletVL = keylet::import_vlseq(infoVL->second);
    auto sleVL = view().peek(keyletVL);
    
    if (!sleVL)
    {
        // create VL import seq counter
        JLOG(ctx_.journal.trace()) << "Import: create vl seq - insert import sequence + public key";
        sleVL = std::make_shared<SLE>(keyletVL);
        sleVL->setFieldU32(sfImportSequence, infoVL->first);
        sleVL->setFieldVL(sfPublicKey, infoVL->second.slice());
        view().insert(sleVL);
    }
    else
    {
        JLOG(ctx_.journal.trace()) << "Import: update vl";
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

    /// this logic is executed last after the account might have already been created    
    if (setSignerEntries)
    {
        JLOG(ctx_.journal.trace())
            << "Import: actioning SignerListSet "
            << "quorum: " << setSignerQuorum << " "
            << "size: " << setSignerEntries->size();
    
        Sandbox sb(&view());
        TER result = 
        SetSignerList::replaceSignersFromLedger(
            ctx_.app,
            sb,
            ctx_.journal,
            id,
            setSignerQuorum,
            *setSignerEntries,
            finalBal.xrp());

        if (result == tesSUCCESS)
        {
            JLOG(ctx_.journal.trace())
                << "Import: successful SignerListSet";
            sb.apply(ctx_.rawView());
        }
        else
        {
            JLOG(ctx_.journal.warn())
                << "Import: SetSignerList failed with code "
                << result << " acc: " << id;
        }
    } 


    return tesSUCCESS;
}

XRPAmount
Import::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return 
        !view.exists(keylet::account(tx.getAccountID(sfAccount))) &&
        !tx.isFieldPresent(sfIssuer) 
            ? XRPAmount { 0 }
            : Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
