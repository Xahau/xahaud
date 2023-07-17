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

#ifndef RIPPLE_TX_IMPORT_H_INCLUDED
#define RIPPLE_TX_IMPORT_H_INCLUDED

#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/core/Config.h>
#include <ripple/protocol/Indexes.h>
#include <memory>

namespace ripple {

class Import : public Transactor
{
public:
    // newly imported accounts get 2 XRP
    template <typename V>
    static
    XRPAmount computeStartingBonus(V const& v)
    {
        auto const& fees = v.read(keylet::fees());

        uint64_t b = 1'000'000;
        uint64_t i =   200'000;

        // new fee object format
        if (fees &&
            fees->isFieldPresent(sfReserveBaseDrops) && 
            fees->isFieldPresent(sfReserveIncrementDrops))
        {
            auto const base = fees->getFieldAmount(sfReserveBaseDrops);
            auto const incr = fees->getFieldAmount(sfReserveIncrementDrops);

            if (isXRP(base) && isXRP(incr))
            {
                b = base.xrp().drops();
                i = incr.xrp().drops();
            }
        }
    
        // old object format
        if (fees &&
            fees->isFieldPresent(sfReserveBase) &&
            fees->isFieldPresent(sfReserveIncrement))
        {
            b = fees->getFieldU32(sfReserveBase);
            i = fees->getFieldU32(sfReserveIncrement);
        }
            
        uint64_t x = b + i * 5U;
        if (x > i && x > b)
            return XRPAmount{x};

        // fallback in case of overflow
        return XRPAmount{2 * DROPS_PER_XRP};
    }
    
    static constexpr ConsequencesFactoryType ConsequencesFactory{Custom};

    static std::pair<
        std::unique_ptr<STTx const>,
        std::unique_ptr<STObject const>>
    getInnerTxn(STTx const& outer, beast::Journal const& j,Json::Value const* xpop = 0);

    explicit Import(ApplyContext& ctx) : Transactor(ctx)
    {
    }
    
    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    static TxConsequences
    makeTxConsequences(PreflightContext const& ctx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace ripple

#endif
