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
#include <algorithm>
#include <vector>
#include <charconv>
#include <ripple/json/json_value.h>
#include <ripple/json/json_reader.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/STTx.h>
#include <ripple/basics/base64.h>
#include <ripple/app/misc/Manifest.h>

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
syntaxCheckProof(Json::Value const& proof, beast::Journal const& j, int depth = 0)
{
    if (depth > 64)
        return false;

    if (proof.isArray())
    {
        // List form
        if (proof.size() != 16)
        {
            JLOG(j.warn()) << "XPOP.transaction.proof list should be exactly 16 entries";
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
std::optional<Json::Value>
syntaxCheckXPOP(Blob const& blob,  beast::Journal const& j)
{
    if (blob.empty())
        return {};

    std::string strJson(blob.begin(), blob.end());
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
            if (!parse_uint64(xpop["ledger"]["coins"].asString()))
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

        if (!syntaxCheckProof(xpop["transaction"]["proof"], j))
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
                if (!value.isString() || !isHex(value.asString()))
                {
                    JLOG(j.warn()) << "XPOP.validation.unl.public_key missing or "
                                  "wrong format (should be hex string)";
                    return {};
                }

                auto pk = strUnHex(value.asString());

                if (!publicKeyType(makeSlice(*pk)))
                {
                    JLOG(j.warn()) << "XPOP.validation.unl.public_key invalid key type.";
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
    if (!ctx.rules.enabled(featureImport))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;

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

    // check if we recognise the vl key
    auto const& vlKeys = ctx.app.config().IMPORT_VL_KEYS;
    auto const found = std::ranges::find(vlKeys, (*xpop)[jss::validation][jss::unl][jss::public_key].asString());
    if (found == vlKeys.end())
        return telIMPORT_VL_KEY_NOT_RECOGNISED;

    // extract the transaction for which this xpop is a proof
    auto rawTx = strUnHex((*xpop)[jss::transaction][jss::blob].asString());

    if (!rawTx)
    {
        JLOG(ctx.j.warn())
            << "Import: failed to deserialize tx blob inside xpop (invalid hex) "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    std::unique_ptr<STTx const> stpTrans;
    try
    {
        stpTrans = std::make_unique<STTx const>(SerialIter { rawTx->data(), rawTx->size() });
    }
    catch (std::exception& e)
    {
        JLOG(ctx.j.warn())
            << "Import: failed to deserialize tx blob inside xpop "
            << tx.getTransactionID();
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

    // ensure the inner txn is an accountset
    if (stpTrans->getTxnType() != ttACCOUNT_SET)
    {
        JLOG(ctx.j.warn())
            << "Import: inner txn must be an AccountSet transaction. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // if the user is trying to use burn to mint (as opposed to just settings import) check that the amount
    // is compatible with the burn amount
    if (amt && *amt > stpTrans->getFieldAmount(sfFee))
    {
        JLOG(ctx.j.warn())
            << "Import: inner txn fee must be higher than outer txn amount field. "
            << tx.getTransactionID();
        return temMALFORMED;
    }

    // check if the inner transaction is signed using the same keying as the outer txn
    {
        auto outer = tx.getSigningPubKey();
        auto inner = stpTrans->getSigningPubKey();

        if (outer.empty() && inner.empty())
        {
            // check signer list
            bool const outerHasSigners = tx.isFieldPresent(sfSigners);
            bool const innerHasSigners = stpTrans->isFieldPresent(sfSigners);

            if (!(outerHasSigners && innerHasSigners) ||
                tx.getFieldArray(sfSigners) != stpTrans->getFieldArray(sfSigners))
            {
                JLOG(ctx.j.warn())
                    << "Import: outer and inner txns were (multi) signed with different keys. "
                    << tx.getTransactionID();
                return temMALFORMED;
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

    if (strHex(m->masterKey) != *found)
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

    auto const tx_meta = strUnHex((*xpop)[jss::transaction][jss::meta].asString());
    
    if (!tx_meta)
    {
        JLOG(ctx.j.warn())
            << "Import: tx meta not valid hex "
            << tx.getTransactionID();
        return temMALFORMED;
    }
    
    auto const tx_hash = sha512Half(HashPrefix::transactionID, *rawTx);



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
    })((*xpop)[jss::transaction][jss::proof], strHex(computed_tx_hash_and_meta)))
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
    auto phash = strUnHex(lgr[jss::phash].asString());
    auto acroot = strUnHex(lgr[jss::acroot].asString());

    if (!coins || !phash || !acroot)
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
        *phash,
        computedTxRoot,
        *acroot,
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

    uint64_t quorum = totalValidatorCount * 0.8;

    if (quorum == 0)
        quorum = 1;


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

            if (used_key.find(nodepub) == used_key.end())
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
                    continue;
                
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


    // check if the validation count is adequate
    if (quorum > validationCount)
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

    return preflight2(ctx);
}

// RH TODO: manifest serials should be kept on chain

TER
Import::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureImport))
        return temDISABLED;

/*    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    if (ctx.tx.isFieldPresent(sfDestination))
    {
        if (!ctx.view.exists(keylet::account(ctx.tx[sfDestination])))
            return tecNO_TARGET;
    }
*/
    return tesSUCCESS;
}

TER
Import::doApply()
{
    return tesSUCCESS;
}

XRPAmount
Import::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return XRPAmount { 0 } ;
}

}  // namespace ripple
