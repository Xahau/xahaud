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

#include <ripple/app/tx/impl/BrokerDeposit.h>
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
BrokerDeposit::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
BrokerDeposit::preflight(PreflightContext const& ctx)
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
BrokerDeposit::doApply()
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

    // amount of native tokens we will transfer to cover reserves for the
    // tls/acc/uritokens created, and native tokens listed in amounts
    XRPAmount nativeAmount{0};

    auto const brokerKeylet = keylet::account(brokerAccID);
    auto sleBrokerAcc = sb.peek(brokerKeylet);
    if (!sleBrokerAcc)
        return terNO_ACCOUNT;
    
    auto const flags = !sleBrokerAcc ? 0 : sleBrokerAcc->getFlags();

    // Check if the destination has disallowed incoming
    if (sb.rules().enabled(featureDisallowIncoming) &&
        (flags & lsfDisallowIncomingRemit))
        return tecNO_PERMISSION;

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
            bool createNew = !brokerState;

            std::uint32_t ownerCount{(*sleBrokerAcc)[sfOwnerCount]};

            if (createNew)
            {
                ++ownerCount;
                XRPAmount const newReserve{sb.fees().accountReserve(ownerCount)};
                nativeAmount += newReserve;

                adjustOwnerCount(sb, sleBrokerAcc, 1, j);
                
                sb.update(sleBrokerAcc);

                // create an entry
                brokerState = std::make_shared<SLE>(brokerStateKeylet);
            }

            if (createNew)
            {
                bool nsExists = !!sb.peek(brokerStateDirKeylet);
                auto const page = sb.dirInsert(brokerStateDirKeylet, brokerStateKeylet.key, describeOwnerDir(brokerAccID));
                if (!page)
                    return tecDIR_FULL;

                brokerState->setFieldU64(sfOwnerNode, *page);
                brokerState->setFieldAmount(sfAmount, amount);
                brokerState->setFieldAmount(sfLockedBalance, STAmount(amount.issue(), 0));

                // add new data to ledger
                sb.insert(brokerState);
            }
            else
            {
                STAmount stateAmount = brokerState->getFieldAmount(sfAmount);
                brokerState->setFieldAmount(sfAmount, amount + stateAmount);
                sb.update(brokerState);
            }
            
            if (isXRP(amount))
            {
                // since we have to pay for all the created objects including
                // possibly the account itself this is paid right at the end,
                // and only if there is balance enough to cover.
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
                         std::vector<AccountID>{srcAccID, brokerAccID},
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
                accountFunds(sb, srcAccID, srcAmt, fhZERO_IF_FROZEN, j)};

            if (availableFunds < srcAmt)
                return tecUNFUNDED_PAYMENT;

            // if the target trustline doesn't exist we need to create it and
            // pay its reserve
            if (issuerAccID != brokerAccID &&
                !sb.exists(
                    keylet::line(brokerAccID, issuerAccID, amount.getCurrency())))
                nativeAmount += objectReserve;

            // action the transfer
            if (TER result =
                    accountSend(sb, srcAccID, brokerAccID, amount, j, false);
                result != tesSUCCESS)
                return result;
        }
    }

    if (nativeAmount < beast::zero)
        return tecINTERNAL;

    if (nativeAmount > beast::zero)
    {
        // ensure the account can cover the native remit
        if (mSourceBalance < nativeAmount)
            return tecUNFUNDED_PAYMENT;

        // subtract the balance from the sender
        {
            STAmount bal = mSourceBalance;
            bal -= nativeAmount;
            if (bal < beast::zero || bal > mSourceBalance)
                return tecINTERNAL;
            sleSrcAcc->setFieldAmount(sfBalance, bal);
        }

        // add the balance to the destination
        {
            STAmount bal = sleBrokerAcc->getFieldAmount(sfBalance);
            STAmount prior = bal;
            bal += nativeAmount;
            if (bal < beast::zero || bal < prior)
                return tecINTERNAL;
            sleBrokerAcc->setFieldAmount(sfBalance, bal);
        }
    }

    auto hasSufficientReserve = [&](std::shared_ptr<SLE> const& sle) -> bool {
        std::uint32_t const uOwnerCount = sle->getFieldU32(sfOwnerCount);
        return sle->getFieldAmount(sfBalance) >=
            sb.fees().accountReserve(uOwnerCount);
    };

    // sanity check reserves
    if (!hasSufficientReserve(sleSrcAcc))
    {
        JLOG(j.warn()) << "BrokerDeposit: sender " << srcAccID
                       << " lacks reserves to cover send.";
        return tecINSUFFICIENT_RESERVE;
    }

    // this isn't actually an error but we will print a warning
    // this can occur if the destination was already below reserve level at the
    // time assets were sent
    if (!hasSufficientReserve(sleBrokerAcc))
    {
        JLOG(j.warn()) << "BrokerDeposit: destination has insufficient reserves.";
    }

    // // apply
    sb.update(sleSrcAcc);
    sb.update(sleBrokerAcc);
    sb.apply(ctx_.rawView());
    
    return tesSUCCESS;
}

XRPAmount
BrokerDeposit::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
