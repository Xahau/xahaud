//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2017 Ripple Labs Inc.

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

#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/tx/impl/URIToken.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/Quality.h>
#include <ripple/protocol/STAccount.h>
#include <ripple/protocol/TER.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

NotTEC
URIToken::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureURIToken))
        return temDISABLED;

    NotTEC const ret{preflight1(ctx)};
    if (!isTesSuccess(ret))
        return ret;

    uint32_t flags = ctx.tx.getFlags();
    auto const tt = ctx.tx.getTxnType();

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
            if (tt == ttURITOKEN_BUY)
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

    // fix amendment to return temMALFORMED if sfDestination field is present
    // and sfAmount field is not present
    if (ctx.rules.enabled(fixXahauV1))
    {
        if (ctx.tx.isFieldPresent(sfDestination) &&
            !ctx.tx.isFieldPresent(sfAmount))
        {
            return temMALFORMED;
        }
    }

    // the validation for the URI field is also the same regardless of the txn
    // type
    if (ctx.tx.isFieldPresent(sfURI))
    {
        auto const uri = ctx.tx.getFieldVL(sfURI);

        if (uri.size() < 1 || uri.size() > 256)
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction. URI must be at least 1 "
                   "character and no more than 256 characters.";
            return temMALFORMED;
        }

        if (!validateUTF8(uri))
        {
            JLOG(ctx.j.warn()) << "Malformed transaction. URI must be a "
                                  "valid utf-8 string.";
            return temMALFORMED;
        }
    }

    switch (tt)
    {
        case ttURITOKEN_MINT: {
            if (flags & tfURITokenMintMask)
                return temINVALID_FLAG;
            break;
        }

        case ttURITOKEN_CANCEL_SELL_OFFER:
        case ttURITOKEN_BURN:
        case ttURITOKEN_BUY:
        case ttURITOKEN_CREATE_SELL_OFFER: {
            if (flags & tfURITokenNonMintMask)
                return temINVALID_FLAG;

            break;
        }

        default:
            return tefINTERNAL;
    }

    // specifying self as a destination is always an error
    if (ctx.tx.isFieldPresent(sfDestination) &&
        ctx.tx.getAccountID(sfAccount) == ctx.tx.getAccountID(sfDestination))
        return temREDUNDANT;

    return preflight2(ctx);
}

TER
URIToken::preclaim(PreclaimContext const& ctx)
{
    bool const fixV1 = ctx.view.rules().enabled(fixXahauV1);

    std::shared_ptr<SLE const> sleU;
    uint32_t leFlags;
    std::optional<AccountID> issuer;
    std::optional<AccountID> owner;
    std::optional<STAmount> saleAmount;
    std::optional<AccountID> dest;
    std::shared_ptr<SLE const> sleOwner;

    if (ctx.tx.isFieldPresent(sfURITokenID))
    {
        sleU = ctx.view.read(
            Keylet{ltURI_TOKEN, ctx.tx.getFieldH256(sfURITokenID)});
        if (!sleU)
            return tecNO_ENTRY;

        leFlags = sleU ? sleU->getFieldU32(sfFlags) : 0;
        owner = sleU->getAccountID(sfOwner);
        issuer = sleU->getAccountID(sfIssuer);
        if (sleU->isFieldPresent(sfAmount))
            saleAmount = sleU->getFieldAmount(sfAmount);

        if (sleU->isFieldPresent(sfDestination))
            dest = sleU->getAccountID(sfDestination);

        sleOwner = ctx.view.read(keylet::account(*owner));
        if (!sleOwner)
        {
            JLOG(ctx.j.warn()) << "Malformed transaction: owner of URIToken is "
                                  "not in the ledger.";
            return tecNO_ENTRY;
        }
    }

    AccountID const acc = ctx.tx.getAccountID(sfAccount);
    uint16_t tt = ctx.tx.getFieldU16(sfTransactionType);

    auto const sle =
        ctx.view.read(keylet::account(ctx.tx.getAccountID(sfAccount)));
    if (!sle)
        return tefINTERNAL;

    switch (tt)
    {
        case ttURITOKEN_MINT: {
            // check if this token has already been minted.
            if (ctx.view.exists(
                    keylet::uritoken(acc, ctx.tx.getFieldVL(sfURI))))
                return tecDUPLICATE;

            return tesSUCCESS;
        }

        case ttURITOKEN_BURN: {
            if (leFlags == lsfBurnable && acc == *issuer)
            {
                // pass, the issuer can burn the URIToken if they minted it with
                // a burn flag
            }
            else if (acc == *owner)
            {
                // pass, the owner can always destroy their own URI token
            }
            else
                return tecNO_PERMISSION;

            return tesSUCCESS;
        }

        case ttURITOKEN_BUY: {
            if (acc == *owner)
                return tecCANT_ACCEPT_OWN_NFTOKEN_OFFER;

            // check if the seller has listed it at all
            if (!saleAmount)
                return tecNO_PERMISSION;

            // check if the seller has listed it for sale to a specific account
            if (dest && *dest != acc)
                return tecNO_PERMISSION;

            // check if the buyer is paying enough
            STAmount const purchaseAmount = ctx.tx[sfAmount];

            if (purchaseAmount.issue() != saleAmount->issue())
                return temBAD_CURRENCY;

            if (purchaseAmount < saleAmount)
                return tecINSUFFICIENT_PAYMENT;

            if (fixV1)
            {
                if (purchaseAmount.native() && saleAmount->native())
                {
                    // native transfer

                    STAmount needed{ctx.view.fees().accountReserve(
                        sle->getFieldU32(sfOwnerCount) + 1)};

                    STAmount const fee = ctx.tx.getFieldAmount(sfFee).xrp();

                    if (needed + fee < needed)
                        return tecINTERNAL;

                    needed += fee;

                    if (needed + purchaseAmount < needed)
                        return tecINTERNAL;

                    needed += purchaseAmount;

                    if (needed > sle->getFieldAmount(sfBalance))
                        return tecINSUFFICIENT_FUNDS;
                }
                else if (purchaseAmount.native() || saleAmount->native())
                {
                    // should not be able to happen
                    return tecINTERNAL;
                }
                else
                {
                    // iou transfer

                    STAmount availableFunds{accountFunds(
                        ctx.view,
                        acc,
                        purchaseAmount,
                        fhZERO_IF_FROZEN,
                        ctx.j)};

                    if (purchaseAmount > availableFunds)
                        return tecINSUFFICIENT_FUNDS;
                }
            }
            else
            {
                // old logic

                if (purchaseAmount.native() && saleAmount->native())
                {
                    // if it's an xrp sale/purchase then no trustline needed
                    if (purchaseAmount >
                        (sleOwner->getFieldAmount(sfBalance) - ctx.tx[sfFee]))
                        return tecINSUFFICIENT_FUNDS;
                }
                else
                {
                    // iou
                    STAmount availableFunds{accountFunds(
                        ctx.view,
                        acc,
                        purchaseAmount,
                        fhZERO_IF_FROZEN,
                        ctx.j)};

                    if (purchaseAmount > availableFunds)
                        return tecINSUFFICIENT_FUNDS;
                }
            }
            return tesSUCCESS;
        }

        case ttURITOKEN_CANCEL_SELL_OFFER: {
            if (acc != *owner)
                return tecNO_PERMISSION;

            return tesSUCCESS;
        }

        case ttURITOKEN_CREATE_SELL_OFFER: {
            if (acc != *owner)
                return tecNO_PERMISSION;

            STAmount txAmount = ctx.tx.getFieldAmount(sfAmount);
            if (!txAmount.native())
            {
                AccountID const iouIssuer = txAmount.getIssuer();
                if (!ctx.view.exists(keylet::account(iouIssuer)))
                    return tecNO_ISSUER;
            }
            return tesSUCCESS;
        }

        default: {
            JLOG(ctx.j.warn()) << "URIToken txid=" << ctx.tx.getTransactionID()
                               << " preclaim with tt = " << tt << "\n";
            return tecINTERNAL;
        }
    }
}

TER
URIToken::doApply()
{
    auto j = ctx_.app.journal("View");

    Sandbox sb(&ctx_.view());

    bool const fixV1 = sb.rules().enabled(fixXahauV1);

    auto const sle = sb.peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    uint16_t tt = ctx_.tx.getFieldU16(sfTransactionType);

    if (tt == ttURITOKEN_MINT || tt == ttURITOKEN_BUY)
    {
        STAmount const reserve{
            sb.fees().accountReserve(sle->getFieldU32(sfOwnerCount) + 1)};

        STAmount const afterFee =
            mPriorBalance - ctx_.tx.getFieldAmount(sfFee).xrp();

        if (afterFee > mPriorBalance || afterFee < reserve)
            return tecINSUFFICIENT_RESERVE;
    }

    uint32_t flags = ctx_.tx.getFlags();

    std::shared_ptr<SLE> sleU;
    std::optional<AccountID> issuer;
    std::optional<AccountID> owner;
    std::optional<STAmount> saleAmount;
    std::optional<AccountID> dest;
    std::optional<Keylet> kl;
    std::shared_ptr<SLE> sleOwner;

    if (tt != ttURITOKEN_MINT)
    {
        kl = Keylet{ltURI_TOKEN, ctx_.tx.getFieldH256(sfURITokenID)};
        sleU = sb.peek(*kl);

        if (!sleU)
            return tecNO_ENTRY;

        if (sleU->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
            return tecNO_ENTRY;

        owner = (*sleU)[sfOwner];
        issuer = (*sleU)[sfIssuer];
        saleAmount = (*sleU)[~sfAmount];
        dest = (*sleU)[~sfDestination];

        if (*owner == account_)
            sleOwner = sle;
        else
            sleOwner = sb.peek(keylet::account(*owner));

        if (!sleOwner)
        {
            JLOG(j.warn()) << "Malformed transaction: owner of URIToken is not "
                              "in the ledger.";
            return tecNO_ENTRY;
        }
    }

    switch (tt)
    {
        case ttURITOKEN_MINT: {
            kl = keylet::uritoken(account_, ctx_.tx.getFieldVL(sfURI));
            if (sb.exists(*kl))
                return tecDUPLICATE;

            sleU = std::make_shared<SLE>(*kl);

            dest = ctx_.tx[~sfDestination];
            saleAmount = ctx_.tx[~sfAmount];

            sleU->setAccountID(sfOwner, account_);
            sleU->setAccountID(sfIssuer, account_);

            if (dest && !saleAmount)
                return tefINTERNAL;

            if (dest)
                sleU->setAccountID(sfDestination, *dest);

            if (saleAmount)
                sleU->setFieldAmount(sfAmount, *saleAmount);

            sleU->setFieldVL(sfURI, ctx_.tx.getFieldVL(sfURI));

            if (ctx_.tx.isFieldPresent(sfDigest))
                sleU->setFieldH256(sfDigest, ctx_.tx.getFieldH256(sfDigest));

            if (flags & tfBurnable)
                sleU->setFlag(tfBurnable);

            auto const page = sb.dirInsert(
                keylet::ownerDir(account_), *kl, describeOwnerDir(account_));

            JLOG(j_.trace())
                << "Adding URIToken to owner directory " << to_string(kl->key)
                << ": " << (page ? "success" : "failure");

            if (!page)
                return tecDIR_FULL;

            sleU->setFieldU64(sfOwnerNode, *page);
            sb.insert(sleU);

            // ensure there is a deletion blocker against the issuer now
            sle->setFieldU32(sfFlags, sle->getFlags() | lsfURITokenIssuer);

            adjustOwnerCount(sb, sle, 1, j);

            sb.update(sle);
            sb.apply(ctx_.rawView());
            return tesSUCCESS;
        }

        case ttURITOKEN_CANCEL_SELL_OFFER: {
            if (sleU->isFieldPresent(sfAmount))
                sleU->makeFieldAbsent(sfAmount);

            if (sleU->isFieldPresent(sfDestination))
                sleU->makeFieldAbsent(sfDestination);

            sb.update(sleU);
            sb.apply(ctx_.rawView());
            return tesSUCCESS;
        }

        case ttURITOKEN_BUY: {
            STAmount const purchaseAmount = ctx_.tx.getFieldAmount(sfAmount);

            // check if the seller has listed it at all
            if (!saleAmount)
                return tecNO_PERMISSION;

            // check if the seller has listed it for sale to a specific account
            if (dest && *dest != account_)
                return tecNO_PERMISSION;

            if (purchaseAmount.issue() != saleAmount->issue())
                return temBAD_CURRENCY;

            if (fixV1)
            {
                // this is the reworked version of the buy routine

                if (purchaseAmount < saleAmount)
                    return tecINSUFFICIENT_PAYMENT;

                // if it's an xrp sale/purchase then no trustline needed
                if (purchaseAmount.native())
                {
                    STAmount needed{sb.fees().accountReserve(
                        sle->getFieldU32(sfOwnerCount) + 1)};

                    STAmount const fee = ctx_.tx.getFieldAmount(sfFee).xrp();

                    if (needed + fee < needed)
                        return tecINTERNAL;

                    needed += fee;

                    if (needed + purchaseAmount < needed)
                        return tecINTERNAL;

                    needed += purchaseAmount;

                    if (needed > mPriorBalance)
                        return tecINSUFFICIENT_FUNDS;
                }
                else
                {
                    // IOU sale
                    if (TER result = trustTransferAllowed(
                            sb, {account_, *owner}, purchaseAmount.issue(), j);
                        !isTesSuccess(result))
                    {
                        JLOG(j.trace())
                            << "URIToken::doApply trustTransferAllowed result="
                            << result;

                        return result;
                    }

                    if (STAmount availableFunds{accountFunds(
                            sb, account_, purchaseAmount, fhZERO_IF_FROZEN, j)};
                        purchaseAmount > availableFunds)
                        return tecINSUFFICIENT_FUNDS;
                }

                // execute the funds transfer, we'll check reserves last
                if (TER result = accountSend(
                        sb,
                        account_,
                        *owner,
                        purchaseAmount,
                        j,
                        WaiveTransferFee::No,
                        false);
                    !isTesSuccess(result))
                    return result;

                // add token to new owner dir
                auto const newPage = sb.dirInsert(
                    keylet::ownerDir(account_),
                    *kl,
                    describeOwnerDir(account_));

                JLOG(j_.trace()) << "Adding URIToken to owner directory "
                                 << to_string(kl->key) << ": "
                                 << (newPage ? "success" : "failure");

                if (!newPage)
                    return tecDIR_FULL;

                // remove from current owner directory
                if (!sb.dirRemove(
                        keylet::ownerDir(*owner),
                        sleU->getFieldU64(sfOwnerNode),
                        kl->key,
                        true))
                {
                    JLOG(j.fatal())
                        << "Could not remove URIToken from owner directory";

                    return tefBAD_LEDGER;
                }

                // adjust owner counts
                adjustOwnerCount(sb, sleOwner, -1, j);
                adjustOwnerCount(sb, sle, 1, j);

                // clean the offer off the object
                sleU->makeFieldAbsent(sfAmount);
                if (sleU->isFieldPresent(sfDestination))
                    sleU->makeFieldAbsent(sfDestination);

                // set the new owner of the object
                sleU->setAccountID(sfOwner, account_);

                // tell the ledger where to find it
                sleU->setFieldU64(sfOwnerNode, *newPage);

                // check each side has sufficient balance remaining to cover the
                // updated ownercounts
                auto hasSufficientReserve =
                    [&](std::shared_ptr<SLE> const& sle) -> bool {
                    std::uint32_t const uOwnerCount =
                        sle->getFieldU32(sfOwnerCount);
                    return sle->getFieldAmount(sfBalance) >=
                        sb.fees().accountReserve(uOwnerCount);
                };

                if (!hasSufficientReserve(sle))
                {
                    JLOG(j.trace()) << "URIToken: buyer " << account_
                                    << " has insufficient reserve to buy";
                    return tecINSUFFICIENT_RESERVE;
                }

                // This should only happen if the owner burned their reserves
                // below the needed amount via another transactor. If this
                // happens they should top up their account before selling!
                if (!hasSufficientReserve(sleOwner))
                {
                    JLOG(j.warn())
                        << "URIToken: seller " << *owner
                        << " has insufficient reserve to allow purchase!";
                    return tecINSUF_RESERVE_SELLER;
                }

                sb.update(sle);
                sb.update(sleU);
                sb.update(sleOwner);
                sb.apply(ctx_.rawView());
                return tesSUCCESS;
            }

            // old logic
            {
                STAmount const purchaseAmount =
                    ctx_.tx.getFieldAmount(sfAmount);

                bool const sellerLow = purchaseAmount.getIssuer() > *owner;
                bool const buyerLow = purchaseAmount.getIssuer() > account_;
                bool sellerIssuer = purchaseAmount.getIssuer() == *owner;
                bool buyerIssuer = purchaseAmount.getIssuer() == account_;

                // check if the seller has listed it at all
                if (!saleAmount)
                    return tecNO_PERMISSION;

                // check if the seller has listed it for sale to a specific
                // account
                if (dest && *dest != account_)
                    return tecNO_PERMISSION;

                if (purchaseAmount.issue() != saleAmount->issue())
                    return temBAD_CURRENCY;

                std::optional<STAmount> initBuyerBal;
                std::optional<STAmount> initSellerBal;
                std::optional<STAmount> finBuyerBal;
                std::optional<STAmount> finSellerBal;
                std::optional<STAmount> dstAmt;
                std::optional<Keylet> tlSeller;
                std::shared_ptr<SLE> sleDstLine;
                std::shared_ptr<SLE> sleSrcLine;

                // if it's an xrp sale/purchase then no trustline needed
                if (purchaseAmount.native())
                {
                    if (purchaseAmount < saleAmount)
                        return tecINSUFFICIENT_PAYMENT;

                    if (purchaseAmount >
                        ((*sleOwner)[sfBalance] - ctx_.tx[sfFee]))
                        return tecINSUFFICIENT_FUNDS;

                    dstAmt = purchaseAmount;

                    initSellerBal = (*sleOwner)[sfBalance];
                    initBuyerBal = (*sle)[sfBalance];

                    finSellerBal = *initSellerBal + purchaseAmount;
                    finBuyerBal = *initBuyerBal - purchaseAmount;
                }
                else
                {
                    // IOU sale
                    STAmount availableFunds{accountFunds(
                        sb, account_, purchaseAmount, fhZERO_IF_FROZEN, j)};

                    // check for any possible bars to a buy transaction
                    // between these accounts for this asset

                    if (buyerIssuer)
                    {
                        // pass: issuer does not create own trustline
                    }
                    else
                    {
                        TER result = trustTransferAllowed(
                            sb, {account_, *owner}, purchaseAmount.issue(), j);
                        JLOG(j.trace())
                            << "URIToken::doApply trustTransferAllowed result="
                            << result;

                        if (!isTesSuccess(result))
                            return result;
                    }

                    if (purchaseAmount > availableFunds)
                        return tecINSUFFICIENT_FUNDS;

                    // check if the seller has a line
                    tlSeller = keylet::line(
                        *owner,
                        purchaseAmount.getIssuer(),
                        purchaseAmount.getCurrency());
                    Keylet tlBuyer = keylet::line(
                        account_,
                        purchaseAmount.getIssuer(),
                        purchaseAmount.getCurrency());

                    sleDstLine = sb.peek(*tlSeller);
                    sleSrcLine = sb.peek(tlBuyer);

                    if (sellerIssuer)
                    {
                        // pass: issuer does not create own trustline
                    }
                    else if (!sleDstLine)
                    {
                        // they do not, so we can create one if they have
                        // sufficient reserve

                        if (std::uint32_t const ownerCount = {sleOwner->at(
                                sfOwnerCount)};
                            (*sleOwner)[sfBalance] <
                            sb.fees().accountReserve(ownerCount + 1))
                        {
                            JLOG(j_.trace())
                                << "Trust line does not exist. "
                                   "Insufficent reserve to create line.";

                            return tecNO_LINE_INSUF_RESERVE;
                        }
                    }
                    if (buyerIssuer)
                    {
                        // pass: issuer does not adjust own trustline
                        initBuyerBal = purchaseAmount.zeroed();
                        finBuyerBal = purchaseAmount.zeroed();
                    }
                    else
                    {
                        // remove from buyer
                        initBuyerBal = buyerLow ? ((*sleSrcLine)[sfBalance])
                                                : -((*sleSrcLine)[sfBalance]);
                        finBuyerBal = *initBuyerBal - purchaseAmount;
                    }

                    dstAmt = purchaseAmount;
                    static Rate const parityRate(QUALITY_ONE);
                    auto xferRate = transferRate(sb, saleAmount->getIssuer());
                    if (!sellerIssuer && !buyerIssuer && xferRate != parityRate)
                    {
                        dstAmt = multiplyRound(
                            purchaseAmount,
                            xferRate,
                            purchaseAmount.issue(),
                            true);
                    }

                    initSellerBal = !sleDstLine
                        ? purchaseAmount.zeroed()
                        : sellerLow ? ((*sleDstLine)[sfBalance])
                                    : -((*sleDstLine)[sfBalance]);

                    finSellerBal = *initSellerBal + *dstAmt;
                }

                // sanity check balance mutations (xrp or iou, both are checked
                // the same way now)
                if (*finSellerBal < *initSellerBal)
                {
                    JLOG(j.warn())
                        << "URIToken txid=" << ctx_.tx.getTransactionID() << " "
                        << "finSellerBal < initSellerBal";
                    return tecINTERNAL;
                }

                if (*finBuyerBal > *initBuyerBal)
                {
                    JLOG(j.warn())
                        << "URIToken txid=" << ctx_.tx.getTransactionID() << " "
                        << "finBuyerBal > initBuyerBal";
                    return tecINTERNAL;
                }

                if (*finBuyerBal < beast::zero)
                {
                    JLOG(j.warn())
                        << "URIToken txid=" << ctx_.tx.getTransactionID() << " "
                        << "finBuyerBal < 0";
                    return tecINTERNAL;
                }

                if (*finSellerBal < beast::zero)
                {
                    JLOG(j.warn())
                        << "URIToken txid=" << ctx_.tx.getTransactionID() << " "
                        << "finSellerBal < 0";
                    return tecINTERNAL;
                }

                // to this point no ledger changes have been made
                // make them in a sensible order such that failure doesn't
                // require cleanup

                // add to new owner's directory first, this can fail if they
                // have too many objects
                auto const newPage = sb.dirInsert(
                    keylet::ownerDir(account_),
                    *kl,
                    describeOwnerDir(account_));

                JLOG(j_.trace()) << "Adding URIToken to owner directory "
                                 << to_string(kl->key) << ": "
                                 << (newPage ? "success" : "failure");

                if (!newPage)
                {
                    // nothing has happened at all and there is nothing to clean
                    // up we can just leave with DIR_FULL
                    return tecDIR_FULL;
                }

                // Next create destination trustline where applicable. This
                // could fail for a variety of reasons. If it does fail we need
                // to remove the dir entry we just added to the buyer before we
                // leave.
                bool lineCreated = false;
                if (!isXRP(purchaseAmount) && !sleDstLine && !sellerIssuer)
                {
                    // clang-format off
                    if (TER const ter = trustCreate(
                            sb,                         // payment sandbox
                            sellerLow,                      // is dest low?
                            purchaseAmount.getIssuer(),     // source
                            *owner,                         // destination
                            tlSeller->key,                  // ledger index
                            sleOwner,                       // Account to add to
                            false,                          // authorize account
                            (sleOwner->getFlags() & lsfDefaultRipple) == 0,
                            false,                          // freeze trust line
                            *dstAmt,                        // initial balance zero
                            Issue(
                                purchaseAmount.getCurrency(), 
                                *owner),                    // limit of zero
                            0,                              // quality in
                            0,                              // quality out
                            j);                             // journal
                        !isTesSuccess(ter))
                    {
                        // remove the newly inserted directory entry before we leave
                        //
                        if (!sb.dirRemove(keylet::ownerDir(account_), *newPage, kl->key, true))
                        {
                            JLOG(j.fatal())
                                << "Could not remove URIToken from owner directory";

                            return tefBAD_LEDGER;
                        }

                        // leave
                        return ter;
                    }
                    // clang-format on

                    // add their trustline to their ownercount
                    lineCreated = true;
                }

                // execution to here means we added the URIToken to the buyer's
                // directory and we definitely have a way to send the funds to
                // the seller.

                // remove from current owner directory
                if (!sb.dirRemove(
                        keylet::ownerDir(*owner),
                        sleU->getFieldU64(sfOwnerNode),
                        kl->key,
                        true))
                {
                    JLOG(j.fatal())
                        << "Could not remove URIToken from owner directory";

                    // remove the newly inserted directory entry before we leave
                    if (!sb.dirRemove(
                            keylet::ownerDir(account_),
                            *newPage,
                            kl->key,
                            true))
                    {
                        JLOG(j.fatal()) << "Could not remove URIToken from "
                                           "owner directory (2)";
                    }

                    // clean up any trustline we might have made
                    if (lineCreated)
                    {
                        auto line = sb.peek(*tlSeller);
                        if (line)
                            sb.erase(line);
                    }

                    return tefBAD_LEDGER;
                }

                // above is all the things that could fail. we now have swapped
                // the ownership as far as the ownerdirs are concerned, and we
                // have a place to pay to and from.

                // if a trustline was created then the ownercount stays the same
                // on the seller +1 TL -1 URIToken
                if (!lineCreated && !isXRP(purchaseAmount))
                    adjustOwnerCount(sb, sleOwner, -1, j);

                // the buyer gets a new object
                adjustOwnerCount(sb, sle, 1, j);

                // clean the offer off the object
                sleU->makeFieldAbsent(sfAmount);
                if (sleU->isFieldPresent(sfDestination))
                    sleU->makeFieldAbsent(sfDestination);

                // set the new owner of the object
                sleU->setAccountID(sfOwner, account_);

                // tell the ledger where to find it
                sleU->setFieldU64(sfOwnerNode, *newPage);

                // update the buyer's balance
                if (isXRP(purchaseAmount))
                {
                    // the sale is for xrp, so set the balance
                    sle->setFieldAmount(sfBalance, *finBuyerBal);
                }
                else if (sleSrcLine)
                {
                    // update the buyer's line to reflect the reduction of the
                    // purchase price
                    sleSrcLine->setFieldAmount(
                        sfBalance, buyerLow ? *finBuyerBal : -(*finBuyerBal));
                }
                else if (buyerIssuer)
                {
                    // pass: buyer is issuer, no update required.
                }
                else
                    return tecINTERNAL;

                // update the seller's balance
                if (isXRP(purchaseAmount))
                {
                    // the sale is for xrp, so set the balance
                    sleOwner->setFieldAmount(sfBalance, *finSellerBal);
                }
                else if (sleDstLine)
                {
                    // the line already existed on the seller side so update it
                    sleDstLine->setFieldAmount(
                        sfBalance,
                        sellerLow ? *finSellerBal : -(*finSellerBal));
                }
                else if (lineCreated)
                {
                    // pass, the TL already has this balance set on it at
                    // creation
                }
                else if (sellerIssuer)
                {
                    // pass: seller is issuer, no update required.
                }
                else
                    return tecINTERNAL;

                if (sleSrcLine)
                    sb.update(sleSrcLine);
                if (sleDstLine)
                    sb.update(sleDstLine);

                sb.update(sle);
                sb.update(sleU);
                sb.update(sleOwner);
                sb.apply(ctx_.rawView());
                return tesSUCCESS;
            }
        }

        case ttURITOKEN_BURN: {
            if (sleU->getAccountID(sfOwner) == account_)
            {
                // pass, owner may always delete own object
            }
            else if (
                sleU->getAccountID(sfIssuer) == account_ &&
                (sleU->getFlags() & lsfBurnable))
            {
                // pass, issuer may burn if the lsfBurnable flag was set during
                // minting
            }
            else
                return tecNO_PERMISSION;

            // execution to here means there is permission to burn

            auto const page = (*sleU)[sfOwnerNode];
            if (!sb.dirRemove(keylet::ownerDir(*owner), page, kl->key, true))
            {
                JLOG(j.fatal())
                    << "Could not remove URIToken from owner directory";
                return tefBAD_LEDGER;
            }

            sb.erase(sleU);

            auto& sleAcc = fixV1 ? sleOwner : sle;

            adjustOwnerCount(sb, sleAcc, -1, j);
            sb.update(sleAcc);
            sb.apply(ctx_.rawView());
            return tesSUCCESS;
        }

        case ttURITOKEN_CREATE_SELL_OFFER: {
            if (account_ != *owner)
                return tecNO_PERMISSION;

            auto const txDest = ctx_.tx[~sfDestination];

            // update destination where applicable
            if (txDest)
                sleU->setAccountID(sfDestination, *txDest);
            else if (dest)
                sleU->makeFieldAbsent(sfDestination);

            sleU->setFieldAmount(sfAmount, ctx_.tx[sfAmount]);

            sb.update(sleU);
            sb.apply(ctx_.rawView());
            return tesSUCCESS;
        }

        default:
            return tecINTERNAL;
    }
}

}  // namespace ripple
