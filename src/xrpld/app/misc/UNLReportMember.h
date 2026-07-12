//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#ifndef RIPPLE_APP_MISC_UNLREPORTMEMBER_H_INCLUDED
#define RIPPLE_APP_MISC_UNLREPORTMEMBER_H_INCLUDED

#include <xrpld/app/misc/Manifest.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/STTx.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ripple {

inline constexpr std::size_t maxUNLReportMemberUpdatesPerRound = 8;

enum class UNLReportMemberBindingResolution : std::uint8_t {
    missing,
    malformed,
    resolved,
};

struct UNLReportMemberBinding
{
    explicit UNLReportMemberBinding(PublicKey const& master) : masterKey(master)
    {
    }

    PublicKey masterKey;
    std::optional<PublicKey> signingKey;
    std::uint32_t manifestSequence = 0;
    uint256 bindingID;
    Blob manifestBlob;
    std::uint32_t ledgerFlags = 0;
    UNLReportMemberBindingResolution resolution =
        UNLReportMemberBindingResolution::missing;
    bool duplicateSigningKey = false;

    bool
    revoked() const;

    bool
    frozen() const;

    bool
    usableSigningBinding() const;
};

struct UNLReportMemberBindingView
{
    uint256 parentLedgerHash;
    std::uint32_t parentLedgerSequence = 0;
    std::vector<UNLReportMemberBinding> members;

    UNLReportMemberBinding const*
    findMaster(PublicKey const& masterKey) const;
};

std::vector<PublicKey>
unlReportActiveMasters(ReadView const& parent);

/** Resolve active UNLReport masters against ledger-anchored manifests.

    The result is a canonical parent-ledger observation, not committee policy.
    Each active master receives one binding entry. Missing and malformed
    records remain visible so a later selector can fail closed only when a
    selected member lacks a usable binding.

    @return `std::nullopt` when UNLReportV2 or the active master universe is
            unavailable in the parent ledger.
*/
std::optional<UNLReportMemberBindingView>
buildUNLReportMemberBindingView(ReadView const& parent);

std::vector<STTx>
buildUNLReportMemberUpdates(
    ReadView const& parent,
    std::vector<Manifest> evidence,
    std::size_t limit = maxUNLReportMemberUpdatesPerRound);

}  // namespace ripple

#endif
