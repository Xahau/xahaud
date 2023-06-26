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

#ifndef RIPPLE_PROTOCOL_IMPORT_H_INCLUDED
#define RIPPLE_PROTOCOL_IMPORT_H_INCLUDED

// #include <ripple/basics/Log.h>
#include <charconv>
#include <ripple/app/misc/Manifest.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/basics/base64.h>
#include <ripple/json/json_reader.h>

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
    if (depth > 64) {
        JLOG(j.warn()) << "XPOP.transaction.proof list should be less than 64 entries";
        return false;
    }

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
        if (depth == 0) // root is special case
        {
            if (!proof["hash"].isString() ||
                proof["hash"].asString().size() != 64 ||
                !proof["key"].isString() ||
                proof["key"].asString().size() != 64 ||
                !proof["children"].isObject())
            {
                JLOG(j.warn())
                    << "XPOP.transaction.proof tree node has wrong format (root)";
                return false;
            }

            return syntaxCheckProof(proof["children"], j, depth + 1);
        }

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
            if (!syntaxCheckProof(node["children"], j, depth + 1)) {
                JLOG(j.warn())
                    << "XPOP.transaction.proof bad children format";
                return false;
            }
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
            JLOG(j.warn()) << "XPOP failed to parse string json";
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

        if (xpop["ledger"]["coins"].isInt())
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

// <sequence, master key>
inline
std::optional<std::pair< uint32_t, PublicKey>>
getVLInfo(Json::Value const& xpop, beast::Journal const& j)
{  
    auto const data = base64_decode(xpop[jss::validation][jss::unl][jss::blob].asString());
    Json::Reader r;
    Json::Value list;
    if (!r.parse(data, list))
    {
        JLOG(j.warn()) << "Import: unl blob was not valid json (after base64 decoding)";
        return {};
    }
    auto const sequence = list[jss::sequence].asUInt();
    auto const m = deserializeManifest(base64_decode(xpop[jss::validation][jss::unl][jss::manifest].asString()));
    if (!m)
    {
        JLOG(j.warn()) << "Import: failed to deserialize manifest";
        return {};
    }
    return { {sequence, m->masterKey} };
}

}  // namespace ripple

#endif
