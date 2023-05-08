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

#include <ripple/app/tx/impl/Import.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <iostream>
#include <json/json.h>
#include <algorithm>
#include <vector>

namespace ripple {


inline bool
isHex(std::string const& str)
{
    return str.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

inline bool
isBase58(std::string const& str)
{
    return str.find_first_not_of(
               "rpshnaf39wBUDNEGHJKLM4PQRST7VWXYZ2bcdeCg65jkm8oFqi1tuvAxyz") ==
        std::string::npos;
}

inline bool
isBase64(std::string const& str)
{
    return str.find_first_not_of(
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
               "/=") == std::string::npos;
}

inline std::optional<uint64_t>
parse_uint64(std::string const& str)
{
    uint64_t result;
    auto [ptr, ec] =
        std::from_chars(str.data(), str.data() + str.size(), result);

    if (ec == std::errc())
        return result;
    return {};
}

inline bool
syntaxCheckProof(Json::Value const& proof, beast::Journal& j, int depth = 0)
{
    if (depth > 64)
        return false;

    if (proof.isArray())
    {
        // List form
        if (proof.asArray().size() != 16)
        {
            JLOG(j.warn() << "XPOP.transaction.proof list should be exactly 16 entries";
            return false;
        }
        for (const auto& entry : proof)
        {
            if (entry.isString())
            {
                if (!isHex(entry.asString()) || entry.asString().size() != 64)
                {
                    JLOG(j.warn()) << "XPOP.transaction.proof list entry missing "
                                  "or wrong format "
                               << "(should be hex string with 64 characters)";
                    return false;
                }
            }
            else if (entry.isArray())
            {
                if (!syntaxCheckProof(entry, j, depth + 1))
                    return false;
            }
            else
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof list entry has wrong format";
                return false;
            }
        }
    }
    else if (proof.isObject())
    {
        // Tree form
        for (const auto& branch : proof.getMemberNames())
        {
            if (branch.size() != 1 || !isHex(branch))
            {
                JLOG(j.warn()) << "XPOP.transaction.proof child node was not 0-F "
                              "hex nibble";
                return false;
            }

            const auto& node = proof[branch];
            if (!node.isObject() || !node["hash"].isString() ||
                node["hash"].asString().size() != 64 ||
                !node["key"].isString() ||
                node["key"].asString().size() != 64 ||
                !node["children"].isObject())
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof tree node has wrong format";
                return false;
            }
            if (!syntaxCheckProof(node["children"], j, depth + 1))
                return false;
        }
    }
    else
    {
        JLOG(j.warn()) << "XPOP.transaction.proof has wrong format (should be "
                      "array or object)";
        return false;
    }

    return true;
}
// does not check signature etc
inline
std::opitonla<Json::Value>
syntaxCheckXPOP(Blob const& blob,  beast::Journal& j)
{
    if (blob.empty())
        return {};

    std::string strJson(blobJson.begin(), blobJson.end());
    if (strJson.empty())
        return {};

    try
    {
        Json::Value xpop;
        Json::Reader reader;

        if (!reader.parse(strJson, xpop))
        {
            JLOG(j.warn()) << "XPOP isn't a string";
            return {};
        }

        if (!xpop.isObject())
        {
            JLOG(j.warn()) << "XPOP is not a JSON object";
            return {};
        }

        if (!xpop["ledger"].isObject())
        {
            JLOG(j.warn()) << "XPOP.ledger is not a JSON object";
            return {};
        }

        if (!xpop["transaction"].isObject())
        {
            JLOG(j.warn()) << "XPOP.transaction is not a JSON object";
            return {};
        }

        if (!xpop["validation"].isObject())
        {
            JLOG(j.warn()) << "XPOP.validation is not a JSON object";
            return {};
        }

        if (!xpop["ledger"]["acroot"].isString() ||
            xpop["ledger"]["acroot"].asString().size() != 64 ||
            !isHex(xpop["ledger"]["acroot"].asString()))
        {
            JLOG(j.warn()) << "XPOP.ledger.acroot missing or wrong format (should "
                          "be hex string)";
            return {};
        }

        if (!xpop["ledger"]["txroot"].isString() ||
            xpop["ledger"]["txroot"].asString().size() != 64 ||
            !isHex(xpop["ledger"]["txroot"].asString()))
        {
            JLOG(j.warn()) << "XPOP.ledger.txroot missing or wrong format "
                          "(should be hex string)";
            return {};
        }

        if (!xpop["ledger"]["phash"].isString() ||
            xpop["ledger"]["phash"].asString().size() != 64 ||
            !isHex(xpop["ledger"]["phash"].asString()))
        {
            JLOG(j.warn()) << "XPOP.ledger.phash missing or wrong format "
                          "(should be hex string)";
            return {};
        }

        if (!xpop["ledger"]["close"].isInt())
        {
            JLOG(j.warn()) << "XPOP.ledger.close missing or wrong format "
                          "(should be int)";
            return {};
        }

        if (!xpop["ledger"]["coins"].isInt())
        {
            // pass
        }
        else if (xpop["ledger"]["coins"].isString())
        {
            if (!parse_uint64(xpop["ledger"]["coins"]))
            {
                JLOG(j.warn()) << "XPOP.ledger.coins missing or wrong format "
                              "(should be int or string)";
                return {};
            }
        }
        else
        {
            JLOG(j.warn()) << "XPOP.ledger.coins missing or wrong format "
                          "(should be int or string)";
            return {};
        }

        if (!xpop["ledger"]["cres"].isInt())
        {
            JLOG(j.warn()) << "XPOP.ledger.cres missing or wrong format "
                          "(should be int)";
            return {};
        }

        if (!xpop["ledger"]["flags"].isInt())
        {
            JLOG(j.warn()) << "XPOP.ledger.flags missing or wrong format "
                          "(should be int)";
            return {};
        }

        if (!xpop["ledger"]["pclose"].isInt())
        {
            JLOG(j.warn()) << "XPOP.ledger.pclose missing or wrong format "
                          "(should be int)";
            return {};
        }

        if (!xpop["transaction"]["blob"].isString() ||
            !isHex(xpop["transaction"]["blob"].asString()))
        {
            JLOG(j.warn()) << "XPOP.transaction.blob missing or wrong format "
                          "(should be hex string)";
            return {};
        }

        if (!xpop["transaction"]["meta"].isString() ||
            !isHex(xpop["transaction"]["meta"].asString()))
        {
            JLOG(j.warn()) << "XPOP.transaction.meta missing or wrong format "
                          "(should be hex string)";
            return {};
        }

        if (!syntxCheckProof(xpop["transaction"]["proof"], j))
        {
            JLOG(j.warn()) << "XPOP.transaction.proof failed syntax check "
                          "(tree/list form)";
            return {};
        }

        if (!xpop["validation"]["data"].isObject())
        {
            JLOG(j.warn()) << "XPOP.validation.data missing or wrong format "
                          "(should be JSON object)";
            return {};
        }

        if (!xpop["validation"]["unl"].isObject())
        {
            JLOG(j.warn()) << "XPOP.validation.unl missing or wrong format "
                          "(should be JSON object)";
            return {};
        }

        for (const auto& key : xpop["validation"]["data"].getMemberNames())
        {
            const auto& value = xpop["validation"]["data"][key];
            if (!isBase58(key) || !value.isString() || !isHex(value.asString()))
            {
                JLOG(j.warn()) << "XPOP.validation.data entry has wrong format "
                           << "(key should be base58 string and value "
                              "should be hex string)";
                return {};
            }
        }

        for (const auto& key : xpop["validation"]["unl"].getMemberNames())
        {
            const auto& value = xpop["validation"]["unl"][key];
            if (key == "public_key")
            {
                if (!value.isString() || !isPublicKey(value.asString()))
                {
                    JLOG(j.warn()) << "XPOP.validation.unl.public_key missing or "
                                  "wrong format (should be base58 string)";
                    return {};
                }
            }
            else if (key == "manifest")
            {
                if (!value.isString())
                {
                    JLOG(j.warn()) << "XPOP.validation.unl.manifest missing or "
                                  "wrong format (should be string)";
                    return {};
                }
            }
            else if (key == "blob")
            {
                if (!value.isString() || !isBase64(value.asString()))
                {
                    JLOG(j.warn()) << "XPOP.validation.unl.blob missing or wrong "
                                  "format (should be base64 string)";
                    return {};
                }
            }
            else if (key == "signature")
            {
                if (!value.isString() || !isHex(value.asString()))
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl.signature missing or wrong "
                           "format (should be hex string)";
                    return {};
                }
            }
            else if (key == "version")
            {
                if (!value.isInt())
                {
                    JLOG(j.warn()) << "XPOP.validation.unl.version missing or "
                                  "wrong format (should be int)";
                    return {};
                }
            }
            else
            {
                if (!value.isObject() && !value.isString())
                {
                    JLOG(j.warn())
                        << "XPOP.validation.unl entry has wrong format";
                    return {};
                }
            }
        }

        return xpop;
    }
    catch (...)
    {
        JLOG(j.warn()) << "An exception occurred during XPOP validation";
    }

    return {};
}

NotTEC
Import::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;

    if (tx.getFieldVL(sfBlob).size() > (256 * 1024))
        return temMALFORMED;

    auto const forbiddenFields[] = {
        sfFlags,
        sfSourceTag,
        sfPreviousTxnID,
        sfLastLedgerSequence,
        sfAccountTxnID,
        sfMemos,
        sfTxnSignature,
        sfSigners,
        sfEmitDetails,
        sfFirstLedgerSequence,
        sfHookParameters};

    for (auto const& f : forbiddenFields)
        if (tx.isFieldPresent(f))
            return temMALFORMED;

    // parse blob as json
    auto const xpop =
        syntaxCheckXPOPBlob(tx.getFieldVL(sfBlob), ctx.j);

    if (!xpop)
        return temMALFORMED;

    // check if we recognise the vl key
    auto const& vlKeys = app_.config().IMPORT_VL_KEYS;
    auto const found = std::ranges::find(vlkeys, xpop[jss::validation][jss::unl][jss::public_key].asString());
    if (found == vlKeys.end())
        return telIMPORT_VL_KEY_NOT_RECOGNISED;

    // check it was used to sign over the manifest
    auto const m =
        deserializeManifest(base64_decode(xpop[jss::validation][jss::unl][jss::manifest]));

    if (!m)
    {
        JLOG(ctx.j.warn())
            << "Import: failed to deserialize manifest on txid "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    if (to_string(m->masterkey) != *found)
    {
        JLOG(ctx.j.warn())
            << "Import: manifest master key did not match top level master key in unl section of xpop "
            << tx.getTransactionID();
        return temMALOFRMED;
    }

    if (!m->valid())
    {
        JLOG(ctx.j.warn())
            << "Import: manifest signature invalid "
            << tx.getTransactionID();
        return temMALOFRMED;
    }

    // manifest signing (ephemeral) key
    auto const signingKey = m->signingKey;

    // decode blob
    auto const data = base64_decode(xpop[jss::validation][jss::unl][jss::blob]);
    auto const sig = strUnHex(xpop[jss::validation][jss::unl][jss::signature]);
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

    if (!(list.isMember(jss::sequence) && list[jss::sequence].isInt() &&
        list.isMember(jss::expiration) && list[jss::expiration].isInt() &&
        (!list.isMember(jss::effective) || list[jss::effective].isInt()) &&
        list.isMember(jss::validators) && list[jss::validators].isArray()))
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob json (after base64 decoding) lacked required fields and/or types "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    auto const sequence = list[jss::sequence].asUInt();
    auto const validFrom = TimeKeeper::time_point{TimeKeeper::duration{
        list.isMember(jss::effective) ? list[jss::effective].asUInt() : 0}};
    auto const validUntil = TimeKeeper::time_point{
        TimeKeeper::duration{list[jss::expiration].asUInt()}};
    auto const now = timeKeeper_.now();
    if (validUntil <= validFrom)
    {
        JLOG(ctx.j.warn())
            << "Import: unl blob validUnil <= validFrom "
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
            deserializeManifest(base64_decode(val[jss::manifest]));

        if (!m)
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob contained an invalid manifest, skipping "
                << tx.getTransactionID();
            continue;
        }

        if (to_string(m->masterkey) != val[jss::validation_public_key])
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob list entry manifest master key did not match master key, skipping "
                << tx.getTransactionID();
            continue;
        }

        if (!m->valid())
        {
            JLOG(ctx.j.warn())
                << "Import: unl blob list entry manifest signature invalid, skipping "
                << tx.getTransactionID();
            continue;
        }

        std::string const nodepub = toBase58(TokenType::NodePublic, m->signingKey);
        std::string const nodemaster = toBase58(TokenType::NodePublic, m->masterKey);
        validators[nodepub] = m->signingKey;
        validatorsMaster[nodemaster] = nodepub;
    }

    uint64_t quorum = totalValidatorCount * 0.8;

    if (quorum == 0)
        quorum = 1;

    auto const tx_blob = strUnHex(xpop[jss::transaction][jss::blob].asString());
    auto const tx_meta = strUnHex(xpop[jss::transaction][jss::meta].asString());
    auto const tx_hash = sha512Half(HashPrefix::transactionID, tx_blob);

    Serializer s(txn->getDataLength() + metaData->getDataLength() + 16);
    s.addVL(tx_blob);
    s.addVL(tx_meta);
    s.addBitString(tx_hash);

    uint256 const computed_tx_hash_and_meta =
        sha512Half(HashPrefix::txNode, s.slice());


    // check if the proof is inside the proof tree/list
    if (!([](Json::Value const& proof, std::string hash) -> bool 
    {
        auto const proofContains = 
        [](Json::Value const* proof, std::string hash, int depth = 0, auto proofContains) -> bool
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
    })(xpop[jss::transaction][jss::proof], strHex(computed_tx_hash_and_meta)))
    {
        JLOG(ctx.j.warn())
           << "Import: xpop proof did not contain the specified txn "
           << tx.getTransactionID();
        return temMALFORMED;
    }


    // compute the merkel root over the proof
    uint256 const computedTxRoot = 
    ([](Json::Value const& proof) -> uint256
    {
        auto hashProof = 
        [](Json::Value const& proof, int depth = 0, auto const& hashProof) -> uint256
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
    })(xpop[jss::transaction][jss::proof]);


    // compute ledger


    // check if the data section contains a quorum

    // RH UPTO: check if the validation::data section contains a quorum
    //

    //
    // finally in preclaim check the sfImportSequence field on the account
    // if it is less than the Account Sequence in the xpop then mint and
    // update sfImportSequence

    return preflight2(ctx);
}

// RH TODO: manifest serials should be kept on chain

TER
Import::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureImport))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    if (ctx.tx.isFieldPresent(sfDestination))
    {
        if (!ctx.view.exists(keylet::account(ctx.tx[sfDestination])))
            return tecNO_TARGET;
    }

    return tesSUCCESS;
}

TER
Import::doApply()
{
    // everything happens in the hooks!
    return tesSUCCESS;
}

XRPAmount
Import::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return 0;
}

}  // namespace ripple
/*
    // xpop must be an object
    if (!xpop.isObject())
    {
        DEBUG_XPOP << "XPOP isn't a JSON object\n";
        return {};
    }


    // XLS41 format
    if (!xpop.isMember(jss::ledger) ||
        !xpop.isMember(jss::transaction) ||
        !xpop.isMember(jss::validation))
    {
        DEBUG_XPOP << "XPOP missing ledger/transaction/validation key\n";
        return {};
    }

    Json::Value const& ledger = xpop[jss::ledger];
    Json::Value const& transaction = xpop[jss::transaction];
    Json::Value const& validation = xpop[jss::validation];


    // ledger section
    if (!ledger.isMember(jss::acroot) ||
        !ledger[jss::acroot].isString() ||
        ledger[jss::acroot].asString().size() != 64)
    {
        DEBUG_XPOP << "XPOP.ledger.acroot missing or wrong format (should be hex
string)\n"; return {};
    }

    if (!ledger.isMember(jss::txroot) ||
        !ledger[jss::txroot].isString() ||
        ledger[jss::txroot].asString().size() != 64)
    {
        DEBUG_XPOP << "XPOP.ledger.txroot missing or wrong format (should be hex
string)\n"; return {};
    }

    if (!ledger.isMember(jss::close) || !ledger[jss::close].isNumeric())
    {
        DEBUG_XPOP << "XPOP.ledger.close missing or wrong format (should be
int)\n"; return {};
    }

    if (!ledger.isMember(jss::coins))
    {
        DEBUG_XPOP << "XPOP.leder.coins missing\n";
        return {};
    }

    if (ledger[jss::coins].isNumeric())
    {
       // pass
    }
    else if (ledger[jss::coins].isString())
    {
        // check if its an int
        if (!parse_uint64(ledger[jss::coins].asString()))
        {
            DEBUG_XPOP << "XPOP.ledger.coins present but isn't parsable to a
uint64\n"; return {};
        }
    }
    else
    {
        DEBUG_XPOP << "XPOP.ledger.coins wrong format (should be int or
string)\n"; return {};
    }

    || !(ledger[jss::coins].isNumeric() || ledger[jss::coins].isString()))


    // validate transaction section
    if (!transaction.isMember("blob") ||
        !transaction.isMember("meta") ||
        !transaction.isMember("proof"))
    {
        if (DEBUG_XPOP)
            std::cout << "XPOP.transaction missing blob/meta/proof key\n";
        return {};
    }


    // check validation section
    if (!validation.isMember("data") ||
        !validation.isMember("unl"))
    {
        if (DEBUG_XPOP)
            std::cout << "XPOP.validation missing data/unl key\n";
    }

}
*/
