//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#include <xrpld/app/misc/UNLReportMember.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/STArray.h>

#include <algorithm>

namespace ripple {

std::vector<PublicKey>
unlReportActiveMasters(ReadView const& parent)
{
    std::vector<PublicKey> result;
    auto const report = parent.read(keylet::UNLReport());
    if (!report || !report->isFieldPresent(sfActiveValidators))
        return result;

    for (auto const& entry : report->getFieldArray(sfActiveValidators))
    {
        auto const key = entry.getFieldVL(sfPublicKey);
        if (publicKeyType(makeSlice(key)))
            result.emplace_back(makeSlice(key));
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<STTx>
buildUNLReportMemberUpdates(
    ReadView const& parent,
    std::vector<Manifest> evidence,
    std::size_t limit)
{
    if (!parent.rules().enabled(featureUNLReportV2) || limit == 0)
        return {};

    evidence.erase(
        std::remove_if(
            evidence.begin(),
            evidence.end(),
            [](Manifest const& manifest) { return !manifest.verify(); }),
        evidence.end());

    auto const less = [](Manifest const& lhs, Manifest const& rhs) {
        if (lhs.masterKey != rhs.masterKey)
            return lhs.masterKey < rhs.masterKey;
        if (lhs.sequence != rhs.sequence)
            return lhs.sequence > rhs.sequence;
        auto const lhsBinding = lhs.bindingID();
        auto const rhsBinding = rhs.bindingID();
        if (lhsBinding != rhsBinding)
            return lhsBinding < rhsBinding;
        return lhs.serialized < rhs.serialized;
    };
    std::sort(evidence.begin(), evidence.end(), less);
    evidence.erase(
        std::unique(
            evidence.begin(),
            evidence.end(),
            [](Manifest const& lhs, Manifest const& rhs) {
                return lhs.masterKey == rhs.masterKey &&
                    lhs.sequence == rhs.sequence &&
                    lhs.bindingID() == rhs.bindingID();
            }),
        evidence.end());

    std::vector<Manifest> highestEvidence;
    highestEvidence.reserve(evidence.size());
    std::optional<PublicKey> currentMaster;
    std::uint32_t currentSequence = 0;
    for (auto& manifest : evidence)
    {
        if (!currentMaster || *currentMaster != manifest.masterKey)
        {
            currentMaster = manifest.masterKey;
            currentSequence = manifest.sequence;
        }
        if (manifest.sequence == currentSequence)
            highestEvidence.emplace_back(std::move(manifest));
    }
    evidence = std::move(highestEvidence);

    auto const activeMasters = unlReportActiveMasters(parent);
    std::vector<STTx> result;
    result.reserve(std::min(limit, evidence.size()));

    for (auto const& manifest : evidence)
    {
        auto const sle =
            parent.read(keylet::UNLReportMember(manifest.masterKey));
        bool const active = std::binary_search(
            activeMasters.begin(), activeMasters.end(), manifest.masterKey);

        bool eligible = false;
        if (!sle)
        {
            eligible = active && !manifest.revoked();
        }
        else
        {
            auto const storedSequence = sle->getFieldU32(sfSequence);
            auto const storedFlags = sle->getFlags();
            if (!Manifest::revoked(storedSequence) &&
                manifest.sequence > storedSequence)
            {
                eligible = active || manifest.revoked();
            }
            else if (
                active && manifest.sequence == storedSequence &&
                manifest.bindingID() != sle->getFieldH256(sfDigest) &&
                (!(storedFlags & lsfUNLReportMemberEquivocationFreeze) ||
                 manifest.bindingID() < sle->getFieldH256(sfDigest)))
            {
                eligible = true;
            }
        }

        if (!eligible)
            continue;

        result.emplace_back(ttUNL_REPORT_MEMBER, [&](auto& obj) {
            obj.setAccountID(sfAccount, AccountID{});
            obj.setFieldU32(sfSequence, 0);
            obj.setFieldAmount(sfFee, STAmount{});
            obj.setFieldVL(sfBlob, makeSlice(manifest.serialized));
        });

        if (result.size() == limit)
            break;
    }

    return result;
}

}  // namespace ripple
