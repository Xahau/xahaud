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
#include <xrpl/protocol/STTx.h>

#include <cstddef>
#include <vector>

namespace ripple {

inline constexpr std::size_t maxUNLReportMemberUpdatesPerRound = 8;

std::vector<PublicKey>
unlReportActiveMasters(ReadView const& parent);

std::vector<STTx>
buildUNLReportMemberUpdates(
    ReadView const& parent,
    std::vector<Manifest> evidence,
    std::size_t limit = maxUNLReportMemberUpdatesPerRound);

}  // namespace ripple

#endif
