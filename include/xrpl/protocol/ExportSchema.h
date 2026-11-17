//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#ifndef RIPPLE_PROTOCOL_EXPORTSCHEMA_H_INCLUDED
#define RIPPLE_PROTOCOL_EXPORTSCHEMA_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ripple {

class Rules;
class STObject;

/** What a hook may export (featureExport): the transactions both networks
    read the same way, and nothing else.

    The set is a hand-maintained table in ExportSchema.cpp: for each
    transaction type the other network has under the same code as this one,
    the fields it lists that this network defines identically, which of them
    it requires, the flags it accepts, the objects (Memos, Signers,
    SignerEntries) an export may nest, and what the type pays there on top
    of the ordinary fee (feeUnits). The table is consensus, so every change
    to it is gated behind an amendment, row by row; the first version ships
    with featureExport. bin/export-schema-check.py compares the table with a
    live rippled and prints what has drifted. */
struct ExportSchema
{
    /** Why `tx`, a transaction built for the other network (with or without
        Signers), may not be exported under `rules`; nullopt if it may */
    static std::optional<std::string>
    reject(STObject const& tx, Rules const& rules);

    static bool
    allowed(STObject const& tx, Rules const& rules)
    {
        return !reject(tx, rules).has_value();
    }

    /** The transaction types exportable under `rules`, by name */
    static std::vector<std::string_view>
    types(Rules const& rules);

    /** What `tx` pays on the other network on top of the ordinary fee, in
        that network's reference fees: the ordinary fee being one reference
        fee for the transaction and one more for each signature it carries,
        and the rest whatever the other network charges its type for (an
        EscrowFinish for the Fulfillment it must check, say). 0 for a
        transaction the schema knows of no extra for, or does not allow. */
    static std::uint64_t
    extraFeeUnits(STObject const& tx, Rules const& rules);

    /** The least `tx` can pay there when it carries `signers` signatures,
        in that network's reference fees: 1 + signers + extraFeeUnits. What
        xport_prepare() prices an export by, with every listed signer
        counted, and what a ttEXPORT's Fee is held to at the reference fee
        (see Export.h). */
    static std::uint64_t
    feeUnits(STObject const& tx, std::size_t signers, Rules const& rules);
};

}  // namespace ripple

#endif
