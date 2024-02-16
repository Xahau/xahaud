//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <ripple/app/tx/impl/OptionExecute.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/BrokerCore.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

TxConsequences
OptionExecute::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
OptionExecute::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfOptionExpireMask)
    {
        // There are no flags (other than universal).
        JLOG(ctx.j.warn()) << "Malformed transaction: Invalid flags set.";
        return temINVALID_FLAG;
    }
}


TER
OptionExecute::doApply()
{
    if (!view().rules().enabled(featureOptions))
        return temDISABLED;

    Sandbox sb(&ctx_.view());

    beast::Journal const& j = ctx_.journal;

    AccountID const srcAccID = ctx_.tx.getAccountID(sfAccount);
    uint256 const optionID = ctx_.tx.getFieldH256(sfOfferID);
    uint256 const offerID = ctx_.tx.getFieldH256(sfInvoiceID);
    auto const flags = ctx_.tx.getFlags();

    auto sleSrcAcc = sb.peek(keylet::account(srcAccID));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    auto optionOfferKeylet = ripple::keylet::unchecked(offerID);
    auto sleOptionOffer = sb.peek(optionOfferKeylet);
    if (!sleOptionOffer)
        return tecNO_TARGET;
    
    AccountID const ownrAccID = sleOptionOffer->getAccountID(sfOwner);
    auto const sealedID = sleOptionOffer->getFieldH256(sfInvoiceID);
    auto oppOfferKeylet = ripple::keylet::unchecked(sealedID);
    auto sleSealedOffer = sb.peek(oppOfferKeylet);
    if (!sleSealedOffer)
        return tecNO_TARGET;

    AccountID const oppAccID = sleSealedOffer->getAccountID(sfOwner);
    auto sleOppAcc = sb.peek(keylet::account(oppAccID));
    if (!sleOppAcc)
        return terNO_ACCOUNT;

    auto const optionFlags = sleOptionOffer->getFlags();
    bool const isPut = (optionFlags & tfType) != 0;
    bool const isSell = (optionFlags & tfAction) != 0;

    auto sleOption = sb.peek(keylet::unchecked(optionID));
    if (!sleOption)
        return tecINTERNAL;

    STAmount const strikePrice = sleOption->getFieldAmount(sfAmount);
    STAmount const quantityShares = sleSealedOffer->getFieldAmount(sfAmount);
    std::uint32_t const quantity = sleOptionOffer->getFieldU32(sfQualityIn);
    STAmount const totalValue = STAmount(strikePrice.issue(), (strikePrice.mantissa() * quantity));

    if (flags & tfOptionExpire)
    {
        JLOG(j.warn()) << "OptionExecute: EXPIRE OPTION";
        sb.erase(sleSealedOffer);
        sb.erase(sleOptionOffer);
        sb.apply(ctx_.rawView());
        return tesSUCCESS;
    }

    switch (isPut)
    {
        case 0: {
            JLOG(j.warn()) << "OptionExecute: EXERCISE CALL";
            if (isXRP(quantityShares))
            {   
                if (mSourceBalance < totalValue.xrp())
                    return tecUNFUNDED_PAYMENT;

                STAmount hBalance = mSourceBalance;
                
                // subtract the total value from the buyer
                {
                    hBalance -= totalValue.xrp();
                    if (hBalance < beast::zero || hBalance > mSourceBalance)
                        return tecINTERNAL;
                }

                // add the total value to the writer
                {
                    STAmount wBalance = sleOppAcc->getFieldAmount(sfBalance);
                    STAmount prior = wBalance;
                    wBalance += totalValue.xrp();
                    if (wBalance < beast::zero || wBalance < prior)
                        return tecINTERNAL;
                    sleOppAcc->setFieldAmount(sfBalance, wBalance);
                }

                // add the shares to the buyer
                {
                    STAmount prior = hBalance;
                    hBalance += quantityShares.xrp();
                    if (hBalance < beast::zero || hBalance < prior)
                        return tecINTERNAL;
                }
                sleSrcAcc->setFieldAmount(sfBalance, hBalance);
            }
            break;
        }
        case 1: {
            JLOG(j.warn()) << "OptionExecute: EXERCISE PUT";
            if (isXRP(quantityShares))
            {       
                // add the total value to the holder
                {
                    STAmount hBalance = mSourceBalance;
                    hBalance += totalValue.xrp();
                    if (hBalance < beast::zero || hBalance < mSourceBalance)
                        return tecINTERNAL;
                    sleSrcAcc->setFieldAmount(sfBalance, hBalance);
                }

                STAmount wBalance = sleOppAcc->getFieldAmount(sfBalance);
                if (wBalance < totalValue.xrp())
                    return tecUNFUNDED_PAYMENT;
                // subtract the total value from the writer
                {
                    STAmount prior = wBalance;
                    wBalance -= totalValue.xrp();
                    if (wBalance < beast::zero || wBalance > prior)
                        return tecINTERNAL;
                }

                // add the shares to the writer
                {
                    STAmount prior = wBalance;
                    wBalance += quantityShares.xrp();
                    if (wBalance < beast::zero || wBalance < prior)
                        return tecINTERNAL;
                }
                sleOppAcc->setFieldAmount(sfBalance, wBalance);
            }
            break;
        }
        default:
            return tecINTERNAL;
    }

    // apply
    sb.update(sleOppAcc);
    sb.update(sleSrcAcc);
    sb.erase(sleSealedOffer);
    sb.erase(sleOptionOffer);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

XRPAmount
OptionExecute::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
