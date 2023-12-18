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

#include <ripple/app/tx/impl/Cadastre.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/app/tx/impl/URIToken.h>

namespace ripple {

TxConsequences
Cadastre::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

/*
    add(jss::Cadastre,
        ltCADASTRE,
        {
            {sfOwner,                soeREQUIRED},
            {sfOwnerNode,            soeREQUIRED},
            {sfAssociation,          soeOPTIONAL},
            {sfAssociationNode,      soeOPTIONAL},
            {sfLocationX,            soeREQUIRED},
            {sfLocationY,            soeREQUIRED},
            {sfUniverse,             soeREQUIRED},
            {sfDisplayURI,           soeOPTIONAL},
            {sfBroadcastURI,         soeOPTIONAL},
            {sfCadastreCount,        soeOPTIONAL},  // for 0x8000,0x8000 tile only
        },
        commonFields);

    ttCADASTRE_MINT                 = 0x005D,    // HookOn = 93
    ttCADASTRE_BURN                 = 0x015D,
    ttCADASTRE_CREATE_SELL_OFFER    = 0x025D,
    ttCADASTRE_CANCEL_SELL_OFFER    = 0x035D,
    ttCADASTRE_BUY                  = 0x045D,
    ttCADASTRE_SET                  = 0x055D,
*/

NotTEC
Cadastre::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;

    // the validation for amount is the same regardless of which txn is appears
    // on
    if (ctx.tx.isFieldPresent(sfAmount))
    {
        auto amt = ctx.tx.getFieldAmount(sfAmount);

        if (!isLegalNet(amt) || amt.signum() < 0)
        {
            JLOG(ctx.j.warn()) << "Malformed transaction. Negative or "
                                  "invalid amount/currency specified.";
            return temBAD_AMOUNT;
        }

        if (isBadCurrency(amt.getCurrency()))
        {
            JLOG(ctx.j.warn()) << "Malformed transaction. Bad currency.";
            return temBAD_CURRENCY;
        }

        if (amt == beast::zero && !ctx.tx.isFieldPresent(sfDestination))
        {
            if (tt == ttCADASTRE_BUY)
            {
                // buy operation does not specify a destination, and can have a
                // zero amount pass
            }
            else
            {
                JLOG(ctx.j.warn()) << "Malformed transaction. "
                                   << "If no sell-to destination is specified "
                                      "then a non-zero price must be set.";
                return temMALFORMED;
            }
        }
    }

    if (ctx.tx.isFieldPresent(sfDestination) &&
        !ctx.tx.isFieldPresent(sfAmount))
        return temMALFORMED;


    auto checkURI = [&ctx](SField const& f) -> TER
    {
        if (ctx.tx.isFieldPresent(f))
        {
            auto const& vl = ctx.tx.getFieldVL(f);
            if (vl.size() < 1 || vl.size() > 256)
            {
                JLOG(ctx.j.warn())
                    << "Cadastre: Malformed transaction, "
                    << f.getName()
                    << " was invalid size (too big/small)";
                return temMALFORMED;   
            }

            if (!URIToken::validateUTF8(ctx.tx.getFieldVL(sfDisplayURI)))
            {
                JLOG(ctx.j.warn())
                    << "Cadastre: Malformed transaction, "
                    << f.getName()
                    << " was not valid utf-8";
                return temMALFORMED;   
            }
        }
        return tesSUCCESS;
    };

    checkURI(sfDisplayURI);
    checkURI(sfBroadcastURI);

    return preflight2(ctx);
}

TER
Cadastre::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureHooks))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    if (ctx.tx.isFieldPresent(sfDestination))
    {
        if (!ctx.view.exists(keylet::account(ctx.tx[sfDestination])))
            return tecNO_TARGET;
    }

    auto const tt = tx.getTxnType();

    switch(tt)
    {
        case ttCADASTRE_MINT:
        case ttCADASTRE_SET:
        {
            

            break;
        }
        case ttCADASTRE_BURN:
        {
            break;
        }
        case ttCADASTRE_CREATE_SELL_OFFER:
        {
            break;
        }
        case ttCADASTRE_CANCEL_SELL_OFFER:
        {
            break;
        }
        case ttCADASTRE_BUY:
        {
            break;
        }
    }


    return tesSUCCESS;
}

TER
Cadastre::doApply()
{
    // everything happens in the hooks!
    return tesSUCCESS;
}

XRPAmount
Cadastre::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount extraFee{0};

    return Transactor::calculateBaseFee(view, tx) + extraFee;
}

}  // namespace ripple
