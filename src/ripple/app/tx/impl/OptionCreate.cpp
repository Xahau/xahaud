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

#include <ripple/app/ledger/OrderBookDB.h>
#include <ripple/app/tx/impl/OptionCreate.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/BrokerCore.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/Option.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

TxConsequences
OptionCreate::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
OptionCreate::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfOptionCreateMask)
    {
        JLOG(ctx.j.warn()) << "OptionCreate: Invalid flags set.";
        return temINVALID_FLAG;
    }

    auto const flags = ctx.tx.getFlags();
    std::optional<uint256> const swapID = ctx.tx[~sfInvoiceID];
    std::uint32_t const quantity = ctx.tx.getFieldU32(sfQualityIn);

    bool const isClose = (flags & tfPosition) != 0;

    if (isClose && !swapID)
    {
        JLOG(ctx.j.warn())
            << "OptionCreate: Cannot close option without swap ID.";
        return temMALFORMED;
    }

    if (quantity % 100)
    {
        JLOG(ctx.j.warn())
            << "OptionCreate: Quantity must be a multple of 100.";
        return temMALFORMED;
    }
}

std::optional<uint256>
sealOption(
    Sandbox& sb,
    STAmount strikePrice,
    std::uint32_t expiration,
    bool isPut,
    bool isSell)
{
    const uint256 uBookBaseOpp = getOptionBookBase(
        strikePrice.getIssuer(),
        strikePrice.getCurrency(),
        strikePrice.mantissa(),
        expiration);
    // std::cout << "BOOK BASE: " << uBookBaseOpp << "\n";
    const uint256 uBookEndOpp = getOptionQualityNext(uBookBaseOpp);
    // std::cout << "BOOK BASE END: " << uBookEndOpp << "\n";
    auto key = sb.succ(uBookBaseOpp, uBookEndOpp);
    if (!key)
    {
        return std::nullopt;
    }

    auto sleOfferDir = sb.read(keylet::page(key.value()));
    uint256 offerIndex;
    unsigned int bookEntry;

    if (!cdirFirst(sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex))
    {
        return std::nullopt;
    }

    // auto sleOffer = sb.read(keylet::unchecked(offerIndex));
    // STAmount premium = sleOffer->getFieldAmount(sfAmount);
    // auto const dir = to_string(sleOffer->getFieldH256(sfBookDirectory));
    // std::cout << "dir: " << dir << "\n";
    // auto const uTipIndex = sleOfferDir->key();
    // auto const optionQuality = getOptionQuality(uTipIndex);
    // std::cout << "optionQuality: " << optionQuality << "\n";
    // STAmount dirRate = STAmount(premium.issue(), optionQuality);
    // std::cout << "dirRate: " << dirRate << "\n";
    // BEAST_EXPECT(100 / rate == 110);

    do
    {
        auto sleItem = sb.read(keylet::child(offerIndex));
        if (!sleItem)
        {
            continue;
        }

        auto const flags = sleItem->getFlags();
        bool const _isPut = (flags & tfType) != 0;
        bool const _isSell = (flags & tfAction) != 0;

        // Skip Sealed Options
        if (sleItem->getFieldU32(sfQualityOut) == 0)
        {
            continue;
        }

        // looking for oposite option position.
        if (_isPut == isPut && _isSell != isSell)
        {
            uint256 const sealID = sleItem->getFieldH256(sfInvoiceID);
            if (sealID.isNonZero())
            {
                continue;
            }
            else
            {
                return offerIndex;
            }
        }
    } while (
        cdirNext(sb, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex));

    return std::nullopt;
}

TER
OptionCreate::doApply()
{
    if (!view().rules().enabled(featureOptions))
        return temDISABLED;

    Sandbox sb(&ctx_.view());

    beast::Journal const& j = ctx_.journal;

    AccountID const srcAccID = ctx_.tx.getAccountID(sfAccount);
    uint256 const optionID = ctx_.tx.getFieldH256(sfOfferID);
    auto const flags = ctx_.tx.getFlags();
    STAmount const premium = ctx_.tx.getFieldAmount(sfAmount);
    std::uint32_t const quantity = ctx_.tx.getFieldU32(sfQualityIn);
    std::optional<uint256> const swapID = ctx_.tx[~sfInvoiceID];

    STAmount const totalPremium =
        STAmount(premium.issue(), (premium.mantissa() * quantity));

    auto sleSrcAcc = sb.peek(keylet::account(srcAccID));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    auto sleOptionAcc = sb.peek(keylet::unchecked(optionID));
    if (!sleOptionAcc)
        return tecINTERNAL;

    STAmount const strikePrice = sleOptionAcc->getFieldAmount(sfAmount);
    std::uint32_t const expiration = sleOptionAcc->getFieldU32(sfExpiration);
    STAmount const quantityShares = STAmount(strikePrice.issue(), isXRP(premium) ? quantity * 1000000 : quantity);

    bool const isPut = (flags & tfType) != 0;
    bool const isSell = (flags & tfAction) != 0;
    bool const isClose = (flags & tfPosition) != 0;

    auto optionBookDirKeylet = keylet::optionBook(
        strikePrice.getIssuer(),
        strikePrice.getCurrency(),
        strikePrice.mantissa(),
        expiration);
    auto optionOfferKeylet =
        ripple::keylet::optionOffer(srcAccID, ctx_.tx.getSeqProxy().value());

    std::optional<uint256> const sealID =
        sealOption(sb, strikePrice, expiration, isPut, isSell);

    // Update Sealed Option
    AccountID oppAccID{AccountID{}};
    if (sealID)
    {
        JLOG(j.warn()) << "Updating Sealed Offer: sealID";
        auto sealedKeylet = ripple::keylet::unchecked(*sealID);
        auto sealedOption = sb.peek(sealedKeylet);
        sealedOption->setFieldH256(sfInvoiceID, optionOfferKeylet.key);
        uint32_t currSealed = sealedOption->getFieldU32(sfQualityOut);
        sealedOption->setFieldU32(sfQualityOut, currSealed - quantity);
        oppAccID = sealedOption->getAccountID(sfOwner);
        sb.update(sealedOption);
    }

    // Block No Market Buys
    if (!isSell && !sealID)
    {
        JLOG(j.warn()) << "OptionCreate: No open sell offers exist.";
        return tecNO_TARGET;
    }

    // Insert New Option (3 Times [issuer, party, counter-party])
    if (!isClose)
    {
        JLOG(j.warn()) << "Creating Option Offer: !isClose";
        // Add Option to Issuer
        if (!isXRP(premium))
        {
            // unimplemented
            return tecINTERNAL;
        }

        // Add Option to Self
        std::uint32_t ownerCount{(*sleSrcAcc)[sfOwnerCount]};
        ++ownerCount;
        XRPAmount const newReserve{sb.fees().accountReserve(ownerCount)};
        adjustOwnerCount(sb, sleSrcAcc, 1, j);

        auto optionOffer = std::make_shared<SLE>(optionOfferKeylet);
        auto const page = sb.dirInsert(
            keylet::ownerDir(account_),
            optionOfferKeylet,
            describeOwnerDir(account_));
        if (!page)
        {
            std::cout << "NO PAGE"
                      << "\n";
            return tecDIR_FULL;
        }

        optionOffer->setFlag(flags);
        optionOffer->setAccountID(sfOwner, srcAccID);
        optionOffer->setFieldU64(sfOwnerNode, *page);
        optionOffer->setFieldH256(sfOfferID, optionID);
        optionOffer->setFieldU32(sfQualityIn, quantity);
        optionOffer->setFieldU32(sfQualityOut, quantity);
        optionOffer->setFieldAmount(sfTakerPays, STAmount(0));  // Premium
        optionOffer->setFieldAmount(sfAmount, STAmount(0));     // Locked
        if (sealID)
        {
            JLOG(j.warn()) << "Updating Option Offer: sealID";
            optionOffer->setFieldH256(sfInvoiceID, *sealID);
            optionOffer->setFieldU32(sfQualityOut, 0);
        }
        if (isSell)
        {
            JLOG(j.warn()) << "Updating Option Offer: isSell";
            // Update the locked balance and premium
            optionOffer->setFieldAmount(sfTakerPays, premium);      // Premium
            optionOffer->setFieldAmount(sfAmount, quantityShares);  // Locked
        }

        Option const option{
            strikePrice.issue(), strikePrice.mantissa(), expiration};

        // std::cout << "BOOK BASE: " << strikePrice.mantissa() << "\n";
        // std::cout << "PREMIUM: " << premium.mantissa() << "\n";

        auto dir =
            keylet::optionQuality(optionBookDirKeylet, premium.mantissa());
        bool const bookExisted = static_cast<bool>(sb.peek(dir));
        auto const bookNode =
            sb.dirAppend(dir, optionOfferKeylet, [&](SLE::ref sle) {
                sle->setFieldH160(
                    sfTakerPaysIssuer, strikePrice.issue().account);
                sle->setFieldH160(
                    sfTakerPaysCurrency, strikePrice.issue().currency);
                sle->setFieldU64(sfBaseFee, strikePrice.mantissa());
                sle->setFieldU32(sfExpiration, expiration);
                sle->setFieldU64(sfExchangeRate, premium.mantissa());
            });

        if (!bookNode)
        {
            JLOG(j_.debug()) << "final result: failed to add offer to book";
            return tecDIR_FULL;
        }

        // if (!bookExisted)
        //     ctx_.app.getOrderBookDB().addOptionOrderBook(option);

        optionOffer->setFieldH256(sfBookDirectory, dir.key);
        optionOffer->setFieldU64(sfBookNode, *bookNode);
        sb.insert(optionOffer);
    }

    if (isClose)
    {
        JLOG(j.warn()) << "Updating Option Offer: isClose";
        auto optionOffer = sb.peek(optionOfferKeylet);
        uint256 const sealID = optionOffer->getFieldH256(sfInvoiceID);
        if (!sealID)
        {
            JLOG(j.warn())
                << "OptionCreate: Cannot close option that has not sealed.";
            return tecINTERNAL;
        }

        // Update Swap Option
        auto swapKeylet = ripple::keylet::unchecked(*swapID);
        auto swapOption = sb.peek(swapKeylet);
        if (!swapOption)
        {
            JLOG(j.warn()) << "OptionCreate: Swap Option does not exist.";
            return tecINTERNAL;
        }
        swapOption->setFieldH256(sfInvoiceID, optionOfferKeylet.key);
        sb.update(swapOption);

        // Update New Option
        optionOffer->setFieldH256(sfInvoiceID, *swapID);
        sb.update(optionOffer);

        // Erase Swap Sealed Option
        uint256 const swapSealedID = swapOption->getFieldH256(sfInvoiceID);
        if (!swapSealedID)
        {
            JLOG(j.warn()) << "OptionCreate: Swap Option is not sealed.";
            return tecINTERNAL;
        }
        auto swapSealedKeylet = ripple::keylet::unchecked(swapSealedID);
        auto swapSealedOption = sb.peek(swapSealedKeylet);
        if (!swapSealedOption)
        {
            JLOG(j.warn())
                << "OptionCreate: Swap Sealed Option does not exist.";
            return tecINTERNAL;
        }
        sb.erase(swapSealedOption);

        // Erase New Sealed Option
        auto sealedKeylet = ripple::keylet::unchecked(sealID);
        auto sealedOption = sb.peek(sealedKeylet);
        sb.erase(sealedOption);
    }

    // xfer the premium from the buyer to the seller
    if (!isSell && sealID)
    {
        JLOG(j.warn()) << "Updating Option Balances: !isSell && sealID";
        auto sleOppAcc = sb.peek(keylet::account(oppAccID));
        if (!sleOppAcc)
            return terNO_ACCOUNT;

        // Native
        if (isXRP(premium))
        {
            // ensure the account can cover the native
            if (mSourceBalance < totalPremium.xrp())
                return tecUNFUNDED_PAYMENT;

            // subtract the balance from the buyer
            {
                STAmount bal = mSourceBalance;
                bal -= totalPremium.xrp();
                if (bal < beast::zero || bal > mSourceBalance)
                    return tecINTERNAL;
                sleSrcAcc->setFieldAmount(sfBalance, bal);
            }

            // add the balance to the writer
            {
                STAmount bal = sleOppAcc->getFieldAmount(sfBalance);
                STAmount prior = bal;
                bal += totalPremium.xrp();
                if (bal < beast::zero || bal < prior)
                    return tecINTERNAL;
                sleOppAcc->setFieldAmount(sfBalance, bal);
            }
        }

        sb.update(sleOppAcc);
    }
    else
    {
        JLOG(j.warn()) << "Updating Option Balances: isSell";
        if (isXRP(quantityShares))
        {
            // subtract the balance from the writer
            if (mSourceBalance < quantityShares.xrp())
                return tecUNFUNDED_PAYMENT;
            {
                STAmount bal = mSourceBalance;
                bal -= quantityShares.xrp();
                if (bal < beast::zero || bal > mSourceBalance)
                    return tecINTERNAL;

                sleSrcAcc->setFieldAmount(sfBalance, bal);
            }
        }
    }

    // apply
    sb.update(sleSrcAcc);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

XRPAmount
OptionCreate::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple