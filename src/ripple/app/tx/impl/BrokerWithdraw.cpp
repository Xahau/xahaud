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

#include <ripple/app/tx/impl/BrokerWithdraw.h>
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
BrokerWithdraw::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
BrokerWithdraw::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        // There are no flags (other than universal).
        JLOG(ctx.j.warn()) << "Malformed transaction: Invalid flags set.";
        return temINVALID_FLAG;
    }

    // sanity check amounts
    if (ctx.tx.isFieldPresent(sfAmounts))
    {
        if (ctx.tx.getFieldArray(sfAmounts).size() > 32)
        {
            JLOG(ctx.j.warn()) << "Malformed: AmountEntrys Exceed Limit `32`.";
            return temMALFORMED;
        }

        std::map<Currency, std::set<AccountID>> already;
        bool nativeAlready = false;

        STArray const& sEntries(ctx.tx.getFieldArray(sfAmounts));
        for (STObject const& sEntry : sEntries)
        {
            // Validate the AmountEntry.
            if (sEntry.getFName() != sfAmountEntry)
            {
                JLOG(ctx.j.warn()) << "Malformed: Expected AmountEntry.";
                return temMALFORMED;
            }

            STAmount const amount = sEntry.getFieldAmount(sfAmount);
            if (!isLegalNet(amount) || amount.signum() <= 0)
            {
                JLOG(ctx.j.warn()) << "Malformed transaction: bad amount: "
                                   << amount.getFullText();
                return temBAD_AMOUNT;
            }

            if (isBadCurrency(amount.getCurrency()))
            {
                JLOG(ctx.j.warn()) << "Malformed transaction: Bad currency.";
                return temBAD_CURRENCY;
            }

            if (isXRP(amount))
            {
                if (nativeAlready)
                {
                    JLOG(ctx.j.warn()) << "Malformed transaction: Native "
                                          "Currency appears more than once.";
                    return temMALFORMED;
                }

                nativeAlready = true;
                continue;
            }

            auto found = already.find(amount.getCurrency());
            if (found == already.end())
            {
                already.emplace(
                    amount.getCurrency(),
                    std::set<AccountID>{amount.getIssuer()});
                continue;
            }

            if (found->second.find(amount.getIssuer()) != found->second.end())
            {
                JLOG(ctx.j.warn()) << "Malformed transaction: Issued Currency "
                                      "appears more than once.";
                return temMALFORMED;
            }

            found->second.emplace(amount.getIssuer());
        }
    }
}

TER
BrokerWithdraw::doApply()
{
    if (!view().rules().enabled(featureOptions))
        return temDISABLED;

    Sandbox sb(&ctx_.view());

    beast::Journal const& j = ctx_.journal;

    auto const srcAccID = ctx_.tx[sfAccount];
    auto const brokerAccID = ctx_.tx[sfDestination];

    auto sleSrcAcc = sb.peek(keylet::account(srcAccID));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    XRPAmount const accountReserve{sb.fees().accountReserve(0)};
    XRPAmount const objectReserve{sb.fees().accountReserve(1) - accountReserve};

    XRPAmount nativeAmount{0};

    auto const brokerKeylet = keylet::account(brokerAccID);
    auto sleBrokerAcc = sb.peek(brokerKeylet);
    if (!sleBrokerAcc)
        return terNO_ACCOUNT;
    
    auto const flags = !sleBrokerAcc ? 0 : sleBrokerAcc->getFlags();
    auto brokerStateDirKeylet = ripple::keylet::brokerStateDir(brokerAccID, srcAccID);

    // iterate trustlines
    if (ctx_.tx.isFieldPresent(sfAmounts))
    {
        // process trustline remits
        STArray const& sEntries(ctx_.tx.getFieldArray(sfAmounts));
        for (STObject const& sEntry : sEntries)
        {
            STAmount const amount = sEntry.getFieldAmount(sfAmount);

            auto brokerStateKeylet = ripple::keylet::brokerState(srcAccID, amount.getIssuer(), brokerStateDirKeylet.key);
            auto brokerState = sb.peek(brokerStateKeylet);
            if (!brokerState)
            {
                JLOG(j.warn()) << "BrokerWithdraw: keylet doesnt exist.";
                return tecINTERNAL;
            }

            STAmount stateAmount = brokerState->getFieldAmount(sfAmount);
            if (amount > stateAmount)
            {
                JLOG(j.warn()) << "BrokerWithdraw: insufficient funds.";
                return tecINTERNAL;
            }

            bool deleteState = amount == stateAmount;
            if (deleteState)
            {
                sb.dirRemove(brokerStateDirKeylet, sleBrokerAcc->getFieldU64(sfOwnerNode), brokerStateKeylet.key, true);
                adjustOwnerCount(sb, sleBrokerAcc, -1, j);
                sb.update(sleBrokerAcc);
                // remove state from ledger
                sb.erase(brokerState);
            }
            else
            {
                brokerState->setFieldAmount(sfAmount, stateAmount - amount);
                sb.update(brokerState);
            }
            
            if (isXRP(amount))
            {
                nativeAmount += amount.xrp();
                continue;
            }

            AccountID const issuerAccID = amount.getIssuer();

            // check permissions
            if (issuerAccID == srcAccID || issuerAccID == brokerAccID)
            {
                // no permission check needed when the issuer sends out or a
                // subscriber sends back RH TODO: move this condition into
                // trustTransferAllowed, guarded by an amendment
            }
            else if (TER canXfer = trustTransferAllowed(
                         sb,
                         std::vector<AccountID>{brokerAccID, srcAccID},
                         amount.issue(),
                         j);
                     canXfer != tesSUCCESS)
                return canXfer;

            // compute the amount the source will need to send
            // in remit the sender pays all transfer fees, so that
            // the destination can always be assured they got the exact amount
            // specified. therefore we need to compute the amount + transfer fee
            auto const srcAmt =
                issuerAccID != srcAccID && issuerAccID != brokerAccID
                ? multiply(amount, transferRate(sb, issuerAccID))
                : amount;

            STAmount availableFunds{
                accountFunds(sb, brokerAccID, srcAmt, fhZERO_IF_FROZEN, j)};

            if (availableFunds < srcAmt)
                return tecUNFUNDED_PAYMENT;

            // if the target trustline doesn't exist we need to create it and
            // pay its reserve
            if (issuerAccID != srcAccID &&
                !sb.exists(
                    keylet::line(srcAccID, issuerAccID, amount.getCurrency())))
            {
                JLOG(j.warn()) << "BrokerWithdraw: invalid trustline.";
                return tecINTERNAL;
            }

            // action the transfer
            if (TER result =
                    accountSend(sb, brokerAccID, srcAccID, amount, j, false);
                result != tesSUCCESS)
                return result;
        }
    }

    if (nativeAmount > beast::zero)
    {
        // ensure the account can cover the native amount
        if (mSourceBalance < nativeAmount)
            return tecUNFUNDED_PAYMENT;

        // add the balance from the sender
        {
            STAmount bal = mSourceBalance;
            bal += nativeAmount;
            if (bal < beast::zero || bal < mSourceBalance)
                return tecINTERNAL;
            sleSrcAcc->setFieldAmount(sfBalance, bal);
        }

        // subtract the balance to the destination
        {
            STAmount bal = sleBrokerAcc->getFieldAmount(sfBalance);
            STAmount prior = bal;
            bal -= nativeAmount;
            if (bal < beast::zero || bal > prior)
                return tecINTERNAL;
            sleBrokerAcc->setFieldAmount(sfBalance, bal);
        }
    }

    // // apply
    sb.update(sleSrcAcc);
    sb.update(sleBrokerAcc);
    sb.apply(ctx_.rawView());
    
    return tesSUCCESS;
}

XRPAmount
BrokerWithdraw::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
