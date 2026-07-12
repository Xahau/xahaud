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

bool
UNLReportMemberBinding::revoked() const
{
    return resolution == UNLReportMemberBindingResolution::resolved &&
        Manifest::revoked(manifestSequence);
}

bool
UNLReportMemberBinding::frozen() const
{
    // Unknown future flags also make a binding ineligible until understood.
    return resolution == UNLReportMemberBindingResolution::resolved &&
        ledgerFlags != 0;
}

bool
UNLReportMemberBinding::usableSigningBinding() const
{
    return resolution == UNLReportMemberBindingResolution::resolved &&
        signingKey && !revoked() && !frozen() && !duplicateSigningKey;
}

UNLReportMemberBinding const*
UNLReportMemberBindingView::findMaster(PublicKey const& masterKey) const
{
    auto const iter = std::lower_bound(
        members.begin(),
        members.end(),
        masterKey,
        [](UNLReportMemberBinding const& member, PublicKey const& key) {
            return member.masterKey < key;
        });
    if (iter == members.end() || iter->masterKey != masterKey)
        return nullptr;
    return &*iter;
}

std::vector<PublicKey>
unlReportActiveMasters(ReadView const& parent)
{
    // Publication tolerates malformed entries so valid evidence can continue
    // converging. The policy-facing binding view below instead fails closed.
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

std::optional<UNLReportMemberBindingView>
buildUNLReportMemberBindingView(ReadView const& parent)
{
    if (!parent.rules().enabled(featureUNLReportV2))
        return std::nullopt;

    auto const report = parent.read(keylet::UNLReport());
    if (!report || !report->isFieldPresent(sfActiveValidators))
        return std::nullopt;

    std::vector<PublicKey> activeMasters;
    for (auto const& entry : report->getFieldArray(sfActiveValidators))
    {
        if (!entry.isFieldPresent(sfPublicKey))
            return std::nullopt;
        auto const& bytes = entry.getFieldVL(sfPublicKey);
        if (!publicKeyType(makeSlice(bytes)))
            return std::nullopt;
        activeMasters.emplace_back(makeSlice(bytes));
    }
    if (activeMasters.empty())
        return std::nullopt;

    std::sort(activeMasters.begin(), activeMasters.end());
    activeMasters.erase(
        std::unique(activeMasters.begin(), activeMasters.end()),
        activeMasters.end());

    UNLReportMemberBindingView view{parent.info().hash, parent.seq(), {}};
    view.members.reserve(activeMasters.size());

    for (auto const& masterKey : activeMasters)
    {
        UNLReportMemberBinding binding{masterKey};
        auto const sle = parent.read(keylet::UNLReportMember(masterKey));
        if (!sle)
        {
            view.members.emplace_back(std::move(binding));
            continue;
        }

        binding.resolution = UNLReportMemberBindingResolution::malformed;
        binding.ledgerFlags = sle->getFlags();
        if (!sle->isFieldPresent(sfPublicKey) ||
            !sle->isFieldPresent(sfSequence) || !sle->isFieldPresent(sfBlob) ||
            !sle->isFieldPresent(sfDigest))
        {
            view.members.emplace_back(std::move(binding));
            continue;
        }

        auto const& storedMaster = sle->getFieldVL(sfPublicKey);
        auto const& storedBlob = sle->getFieldVL(sfBlob);
        if (!publicKeyType(makeSlice(storedMaster)) ||
            PublicKey{makeSlice(storedMaster)} != masterKey ||
            storedBlob.empty() ||
            storedBlob.size() > maxUNLReportMemberManifestSize)
        {
            view.members.emplace_back(std::move(binding));
            continue;
        }

        binding.manifestSequence = sle->getFieldU32(sfSequence);
        binding.bindingID = sle->getFieldH256(sfDigest);
        binding.manifestBlob = storedBlob;

        if (sle->isFieldPresent(sfSigningPubKey))
        {
            auto const& storedSigning = sle->getFieldVL(sfSigningPubKey);
            if (!publicKeyType(makeSlice(storedSigning)))
            {
                view.members.emplace_back(std::move(binding));
                continue;
            }
            binding.signingKey.emplace(makeSlice(storedSigning));
        }

        auto manifest = deserializeManifest(binding.manifestBlob);
        if (!manifest || !manifest->verify() ||
            manifest->masterKey != masterKey ||
            manifest->sequence != binding.manifestSequence ||
            manifest->bindingID() != binding.bindingID ||
            manifest->signingKey != binding.signingKey)
        {
            view.members.emplace_back(std::move(binding));
            continue;
        }

        bool const storedBytesMatch = makeSlice(manifest->serialized()) ==
            makeSlice(binding.manifestBlob);
        if (!storedBytesMatch || (manifest->revoked() && binding.signingKey) ||
            (!manifest->revoked() && !binding.signingKey))
        {
            view.members.emplace_back(std::move(binding));
            continue;
        }

        binding.resolution = UNLReportMemberBindingResolution::resolved;
        view.members.emplace_back(std::move(binding));
    }

    for (std::size_t i = 0; i < view.members.size(); ++i)
    {
        auto& lhs = view.members[i];
        if (lhs.resolution != UNLReportMemberBindingResolution::resolved ||
            !lhs.signingKey)
            continue;

        for (std::size_t j = i + 1; j < view.members.size(); ++j)
        {
            auto& rhs = view.members[j];
            if (rhs.resolution != UNLReportMemberBindingResolution::resolved ||
                !rhs.signingKey || *lhs.signingKey != *rhs.signingKey)
                continue;

            lhs.duplicateSigningKey = true;
            rhs.duplicateSigningKey = true;
        }
    }

    return view;
}

std::vector<STTx>
buildUNLReportMemberUpdates(
    ReadView const& parent,
    std::vector<Manifest> evidence,
    std::size_t limit)
{
    if (!parent.rules().enabled(featureUNLReportV2) || limit == 0)
        return {};

    auto const activeMasters = unlReportActiveMasters(parent);
    evidence.erase(
        std::remove_if(
            evidence.begin(),
            evidence.end(),
            [&](Manifest const& manifest) {
                bool const active = std::binary_search(
                    activeMasters.begin(),
                    activeMasters.end(),
                    manifest.masterKey);
                auto const sle =
                    parent.read(keylet::UNLReportMember(manifest.masterKey));
                if (!sle)
                    return !active;

                auto const storedSequence = sle->getFieldU32(sfSequence);
                if (Manifest::revoked(storedSequence))
                    return true;
                if (manifest.sequence > storedSequence)
                    return !active && !manifest.revoked();
                return !active || manifest.sequence != storedSequence;
            }),
        evidence.end());

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
        return lhs.serialized() < rhs.serialized();
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
            eligible = active;
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
            obj.setFieldVL(sfBlob, makeSlice(manifest.serialized()));
        });

        if (result.size() == limit)
            break;
    }

    return result;
}

}  // namespace ripple
