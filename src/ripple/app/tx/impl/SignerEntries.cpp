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

#include <ripple/app/tx/impl/SignerEntries.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Rules.h>
#include <ripple/protocol/STArray.h>
#include <ripple/protocol/STObject.h>
#include <ripple/protocol/STTx.h>
#include <cstdint>
#include <optional>

namespace ripple {

Expected<std::vector<SignerEntries::SignerEntry>, NotTEC>
SignerEntries::deserialize(
    STObject const& obj,
    beast::Journal journal,
    std::string const& annotation)
{
    std::pair<std::vector<SignerEntry>, NotTEC> s;

    if (!obj.isFieldPresent(sfSignerEntries))
    {
        JLOG(journal.trace())
            << "Malformed " << annotation << ": Need signer entry array.";
        return Unexpected(temMALFORMED);
    }

    std::vector<SignerEntry> accountVec;
    accountVec.reserve(STTx::maxMultiSigners());

    STArray const& sEntries(obj.getFieldArray(sfSignerEntries));
    for (STObject const& sEntry : sEntries)
    {
        // Validate the SignerEntry.
        if (sEntry.getFName() != sfSignerEntry)
        {
            JLOG(journal.trace())
                << "Malformed " << annotation << ": Expected SignerEntry.";
            return Unexpected(temMALFORMED);
        }

        // Extract SignerEntry fields.
        AccountID const account = sEntry.getAccountID(sfAccount);
        std::uint16_t const weight = sEntry.getFieldU16(sfSignerWeight);
        std::optional<uint256> const tag = sEntry.at(~sfWalletLocator);

        accountVec.emplace_back(account, weight, tag);
    }
    return accountVec;
}

std::tuple<
    NotTEC,
    std::uint32_t,
    std::vector<SignerEntries::SignerEntry>,
    SignerEntries::Operation>
SignerEntries::determineOperation(
    STTx const& tx,
    ApplyFlags flags,
    beast::Journal j)
{
    // Check the quorum.  A non-zero quorum means we're creating or replacing
    // the list.  A zero quorum means we're destroying the list.
    auto const quorum = tx[sfSignerQuorum];
    std::vector<SignerEntries::SignerEntry> sign;
    Operation op = unknown;

    bool const hasSignerEntries(tx.isFieldPresent(sfSignerEntries));
    if (quorum && hasSignerEntries)
    {
        auto signers = SignerEntries::deserialize(tx, j, "transaction");

        if (!signers)
            return std::make_tuple(signers.error(), quorum, sign, op);

        std::sort(signers->begin(), signers->end());

        // Save deserialized list for later.
        sign = std::move(*signers);
        op = set;
    }
    else if ((quorum == 0) && !hasSignerEntries)
    {
        op = destroy;
    }

    return std::make_tuple(tesSUCCESS, quorum, sign, op);
}

// NotTEC
// SignerEntries::validateOperation(
//     NotTEC const& ter,
//     std::uint32_t quorum,
//     std::vector<SignerEntries::SignerEntry> signers,
//     SignerEntries::Operation op,
//     STTx const& tx,
//     beast::Journal j,
//     Rules const& rules)
// {

//     if (!isTesSuccess(ter))
//         return ter;

//     if (op == unknown)
//     {
//         // Neither a set nor a destroy.  Malformed.
//         JLOG(j.trace())
//             << "Malformed transaction: Invalid signer set list format.";
//         return temMALFORMED;
//     }

//     if (op == set)
//     {
//         // Validate our settings.
//         auto const account = tx.getAccountID(sfAccount);
//         NotTEC const ter = validateQuorumAndSignerEntries(
//             quorum,
//             signers,
//             account,
//             j,
//             rules);
//         if (!isTesSuccess(ter))
//         {
//             return ter;
//         }
//     }

//     return tesSUCCESS;
// }

}  // namespace ripple
