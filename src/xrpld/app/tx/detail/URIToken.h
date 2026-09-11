//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2014 Ripple Labs Inc.

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

#ifndef RIPPLE_TX_URITOKEN_H_INCLUDED
#define RIPPLE_TX_URITOKEN_H_INCLUDED

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AppLoader.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>

namespace ripple {

class URIToken : public Transactor
{
public:
    /** Validate a byte sequence as UTF-8.

        Two things differ across featurePWALoader. Before the amendment,
        U+FFFE and U+FFFF were rejected; they are well-formed UTF-8, and
        noncharacters are permitted in interchange, so the amendment accepts
        them. Because that changes which transactions preflight successfully,
        it rides an amendment rather than landing directly.

        @param permitNoncharacters Pass rules.enabled(featurePWALoader).
               Defaults to the post-amendment behaviour so that a caller who
               omits it is at worst wrong before activation, never after.
    */
    bool inline static validateUTF8(
        std::vector<uint8_t> const& u,
        bool permitNoncharacters = true)
    {
        if (!appLoader::isValidUTF8(u.data(), u.size()))
            return false;

        if (permitNoncharacters)
            return true;

        // Pre-amendment behaviour: reject U+FFFE and U+FFFF, which encode as
        // EF BF BE and EF BF BF. UTF-8 is self-synchronising, so in a
        // sequence already known to be well-formed these bytes can only be
        // those two code points.
        for (std::size_t i = 0; i + 2 < u.size(); ++i)
            if (u[i] == 0xEF && u[i + 1] == 0xBF &&
                (u[i + 2] == 0xBE || u[i + 2] == 0xBF))
                return false;

        return true;
    }

    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit URIToken(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

using URITokenMint = URIToken;
using URITokenBurn = URIToken;
using URITokenBuy = URIToken;
using URITokenCreateSellOffer = URIToken;
using URITokenCancelSellOffer = URIToken;

}  // namespace ripple

#endif
