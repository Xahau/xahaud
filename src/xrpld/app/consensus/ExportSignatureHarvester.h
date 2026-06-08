//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/XRPLF/rippled
    Copyright 2026 Xahau

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_APP_CONSENSUS_EXPORTSIGNATUREHARVESTER_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_EXPORTSIGNATUREHARVESTER_H_INCLUDED

#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/UintTypes.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ripple {

using ExportTxnLookup = hash_map<uint256, std::shared_ptr<STTx const>>;

struct ExportSignatureHarvestInput
{
    PublicKey const& senderPK;
    uint256 const& proposalPrevLedger;
    std::vector<std::string> const& exportSignatures;
    std::optional<uint256> activeViewSourceLedgerHash;
    std::function<bool(PublicKey const&)> isActiveSigner;
    ExportTxnLookup const& exportTxns;
    LedgerIndex currentClosedSeq = 0;
    char const* source = "unknown";
    std::size_t maxEntries = 0;
};

bool
verifyExportSignatureAgainstTx(
    STTx const& exportTx,
    PublicKey const& validator,
    Slice sigSlice,
    uint256 const& txHash,
    beast::Journal j,
    char const* source);

std::size_t
harvestExportSignatures(
    ExportSignatureHarvestInput const& input,
    ExportSigCollector& collector,
    beast::Journal j);

}  // namespace ripple

#endif
