//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2019 Dev Null Productions

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
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/rpc/Context.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>

#include <algorithm>
#include <vector>

namespace ripple {
Json::Value
doManifest(RPC::JsonContext& context)
{
    auto& params = context.params;

    if (!params.isMember(jss::public_key))
        return RPC::missing_field_error(jss::public_key);

    auto const requested = params[jss::public_key].asString();

    Json::Value ret;
    ret[jss::requested] = requested;

    auto const pk = parseBase58<PublicKey>(TokenType::NodePublic, requested);
    if (!pk)
    {
        RPC::inject_error(rpcINVALID_PARAMS, ret);
        return ret;
    }

    auto identity = context.app.validators().resolveMonitoringSigner(*pk);
    if (identity.status == ValidatorIdentityStatus::unknown)
        identity = context.app.validators().lookupMonitoringIdentity(*pk);

    auto statusName = [](ValidatorIdentityStatus status) -> char const* {
        switch (status)
        {
            case ValidatorIdentityStatus::resolved:
                return "resolved";
            case ValidatorIdentityStatus::revoked:
                return "revoked";
            case ValidatorIdentityStatus::conflict:
                return "conflict";
            case ValidatorIdentityStatus::unstable:
                return "unstable";
            case ValidatorIdentityStatus::unknown:
                return "unknown";
        }
        return "unknown";
    };
    ret["monitoring_status"] = statusName(identity.status);
    if (identity.status == ValidatorIdentityStatus::unknown ||
        identity.status == ValidatorIdentityStatus::unstable)
        return ret;

    if (identity.manifest)
        ret[jss::manifest] = base64_encode(identity.manifest->serialized);
    Json::Value details;

    if (identity.master)
        details[jss::master_key] =
            toBase58(TokenType::NodePublic, *identity.master);
    if (identity.signing)
        details[jss::ephemeral_key] =
            toBase58(TokenType::NodePublic, *identity.signing);

    if (identity.manifest)
    {
        details[jss::seq] = identity.manifest->sequence;
        if (!identity.manifest->domain.empty())
            details[jss::domain] = identity.manifest->domain;
    }

    details["ordinary_state"] = identity.presentInOrdinaryState;

    auto appendPublishers = [&](char const* field,
                                hash_set<PublicKey> const& publishers) {
        if (publishers.empty())
            return;
        auto& array = details[field] = Json::arrayValue;
        std::vector<PublicKey> ordered{publishers.begin(), publishers.end()};
        std::sort(ordered.begin(), ordered.end());
        for (auto const& publisher : ordered)
            array.append(strHex(publisher));
    };
    appendPublishers(
        "candidate_identity_publishers", identity.candidateIdentityPublishers);
    appendPublishers(
        "candidate_manifest_publishers", identity.candidateManifestPublishers);

    if (!identity.conflictingMasters.empty())
    {
        auto& conflicts = details["conflicting_masters"] = Json::arrayValue;
        for (auto const& master : identity.conflictingMasters)
            conflicts.append(toBase58(TokenType::NodePublic, master));
    }

    ret[jss::details] = details;
    return ret;
}
}  // namespace ripple
