//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <xrpl/protocol/JsonTx.h>

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STBase.h>
#include <xrpl/protocol/STBlob.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <vector>

#include <set>
#include <string>

namespace ripple {
namespace jsonTx {

namespace {

/** Serialize `obj` in canonical form, skipping the named fields.
    Returns the concatenated field-by-field binary encoding -- the same
    bytes that would appear in the normal binary tx minus the excluded
    fields. Field order is the usual fieldCode-sorted canonical order.
*/
Blob
canonicalSerialization(
    STObject const& obj,
    std::set<std::string> const& skip)
{
    std::vector<STBase const*> fields;
    for (auto const& entry : obj)
    {
        if (entry.getSType() == STI_NOTPRESENT)
            continue;
        if (skip.find(entry.getFName().getName()) != skip.end())
            continue;
        fields.push_back(&entry);
    }
    std::sort(
        fields.begin(), fields.end(), [](STBase const* a, STBase const* b) {
            return a->getFName().fieldCode < b->getFName().fieldCode;
        });

    Serializer s;
    for (STBase const* f : fields)
    {
        f->addFieldID(s);
        f->add(s);
        auto const sType = f->getSType();
        if (sType == STI_ARRAY || sType == STI_OBJECT)
            s.addFieldID(sType, 1);
    }
    return s.getData();
}

}  // namespace

bool
hasBody(STObject const& obj) noexcept
{
    try
    {
        return obj.isFieldPresent(sfJsonTxBody) &&
            !obj.getFieldVL(sfJsonTxBody).empty();
    }
    catch (...)
    {
        return false;
    }
}

Slice
body(STObject const& obj)
{
    if (!obj.isFieldPresent(sfJsonTxBody))
        return Slice{};
    // peekAtField gives us a view into the STObject's owned storage;
    // STBlob::value() returns a Slice over that storage directly.
    auto const& field = obj.peekAtField(sfJsonTxBody);
    return static_cast<STBlob const&>(field).value();
}

uint256
bodyHash(STObject const& obj)
{
    auto const s = body(obj);
    if (s.empty())
        return uint256{};
    return sha512Half(s);
}

Expected<void, std::string>
checkSignature(STTx const& stx)
{
    if (!hasBody(stx))
        return Unexpected<std::string>("JsonTxBody is missing or empty.");

    if (!stx.isFieldPresent(sfSigningPubKey))
        return Unexpected<std::string>("SigningPubKey is missing.");

    Blob const spk = stx.getFieldVL(sfSigningPubKey);
    if (!publicKeyType(makeSlice(spk)))
        return Unexpected<std::string>("SigningPubKey is not a valid key.");

    if (!stx.isFieldPresent(sfTxnSignature))
        return Unexpected<std::string>("TxnSignature is missing.");

    Blob const sig = stx.getFieldVL(sfTxnSignature);
    if (sig.empty())
        return Unexpected<std::string>("TxnSignature is empty.");

    if (!verify(PublicKey(makeSlice(spk)), body(stx), makeSlice(sig)))
        return Unexpected<std::string>(
            "Signature over JsonTxBody failed verification.");

    return {};
}

Expected<void, std::string>
checkStructuralEquivalence(STTx const& stx)
{
    if (!hasBody(stx))
        return Unexpected<std::string>("JsonTxBody is missing or empty.");

    auto const bodySlice = body(stx);
    std::string const bodyStr(
        reinterpret_cast<char const*>(bodySlice.data()), bodySlice.size());

    Json::Value parsed;
    Json::Reader reader;
    if (!reader.parse(bodyStr, parsed) || !parsed.isObject())
        return Unexpected<std::string>(
            "JsonTxBody is not a valid JSON object.");

    STParsedJSONObject parsedObj("JsonTxBody", parsed);
    if (!parsedObj.object)
        return Unexpected<std::string>(
            "JsonTxBody does not parse into a valid STObject: " +
            (parsedObj.error.isMember(jss::error_message)
                 ? parsedObj.error[jss::error_message].asString()
                 : std::string("unknown parse error")));

    // The json-tx wrapper fields (TxnSignature, JsonTxBody) must be
    // excluded from both sides: TxnSignature covers the body bytes
    // (not the binary), and JsonTxBody is the body itself.
    std::set<std::string> const skip{
        sfTxnSignature.getName(), sfJsonTxBody.getName()};

    if (canonicalSerialization(stx, skip) !=
        canonicalSerialization(*parsedObj.object, skip))
        return Unexpected<std::string>(
            "JsonTxBody content does not match the structural fields "
            "of the transaction.");

    return {};
}

}  // namespace jsonTx
}  // namespace ripple
