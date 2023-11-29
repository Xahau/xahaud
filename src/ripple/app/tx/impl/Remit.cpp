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

#include <ripple/app/tx/impl/Remit.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/app/tx/impl/URIToken.h>

namespace ripple {

TxConsequences
Remit::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
Remit::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        // There are no flags (other than universal).
        JLOG(ctx.j.warn()) << "Malformed transaction: Invalid flags set.";
        return temINVALID_FLAG;
    }
    if (ctx.tx[sfAccount] == ctx.tx[sfDestination])
    {
        // They wrote a check to themselves.
        JLOG(ctx.j.warn()) << "Malformed transaction: Remit to self.";
        return temREDUNDANT;
    }

    // sanity check amounts
    if (ctx.tx.isFieldPresent(sfAmounts))
    {
        std::map<Currency, std::set<AccountID>> already;
        bool nativeAlready = false;

        STArray const& sEntries(obj.getFieldArray(sfAmounts));
        for (STObject const& sEntry : sEntries)
        {
            // Validate the AmountEntry.
            if (sEntry.getFName() != sfAmountEntry)
            {
                JLOG(ctx.j.warn())
                    << "Malformed " << annotation << ": Expected AmountEntry.";
                return temMALFORMED;
            }

            STAmount const amt = sEntry.getFieldAmount(sfAmount);
            if (!isLegalNet(amt) || amt.signum() <= 0)
            {
                JLOG(ctx.j.warn()) << "Malformed transaction: bad amount: "
                                   << amt.getFullText();
                return temBAD_AMOUNT;
            }

            if (isBadCurrency(amt.getCurrency()))
            {
                JLOG(ctx.j.warn()) << "Malformed transaction: Bad currency.";
                return temBAD_CURRENCY;
            }

            if (isXRP(amt))
            {
                if (nativeAlready)
                {
                    JLOG(ctx.j.warn()) << "Malformed transaction: Native Currency appears more than once.";
                    return temMALFORMED;
                }

                nativeAlready = true;
                continue;
            }


            auto& found = already.find(amt.getCurrency());
            if (found == already.end())
            {
                already.emplace(amt.getCurrency(), {amt.getIssuer()});
                continue;
            }

            if (found->second.find(amt.getIssuer()))
            {
                JLOG(ctx.j.warn()) << "Malformed transaction: Issued Currency appears more than once.";
                return temMALFORMED;
            }

            found->second.emplace(amt.getIssuer());
        }
    }

    // sanity check minturitoken
    if (ctx.tx.isFieldPresent(sfMintURIToken))
    {
        STObject const& mint = const_cast<ripple::STTx&>(tx)
                                      .getField(sfMintURIToken)
                                      .downcast<STObject>();
        if (!URToken::validateUTF(mint.getFieldVL(sfURI)))
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction: Invalid UTF8 inside MintURIToken.";
            return temMALFORMED;
        }
    }

    // check uritokenids for duplicates
    if (ctx.tx.isFieldPresent(sfURITokenIDs))
    {
        STVector256 ids = ctx.tx.getFieldV256(sfURITokenIDs);
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction: Duplicate URITokenID.";
            return temMALFORMED;
        }
    }

    return preflight2(ctx);
}

TER
Remit::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureRemit))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;


    // amount of native tokens we will transfer to cover reserves for the tls/acc/uritokens created, and
    // native tokens listed in amounts
    XRPAmount nativeRemit { 0 };

    XRPAmount const accountReserve { ctx.view.fees().accountReserve(0) };
    XRPAmount const objectReserve { ctx.view.fees().accountReserve(1) - accountReserve };
    AccountID const dstId{ctx.tx[sfDestination]};
    auto const sleDst = ctx.view.read(keylet::account(dstId));

    // the sender must pay for the destination acc if it doesn't exist
    if (!sleDst)
        nativeRemit += accountReserve;
    else
    {
        auto const flags = sleDst->getFlags();

        // Check if the destination has disallowed incoming
        if (ctx.view.rules().enabled(featureDisallowIncoming) &&
            (flags & lsfDisallowIncomingRemit))
            return tecNO_PERMISSION;
    }
    
    // the destination may require a dest tag
    if ((flags & lsfRequireDestTag) && !ctx.tx.isFieldPresent(sfDestinationTag))
    {
        JLOG(ctx.j.warn()) << "Remit: DestinationTag required for this destination.";
        return tecDST_TAG_NEEDED;
    }

    // if theres a minted uritoken the sender pays for that
    std::optional<Keylet> mintKL;
    std::optional<Blob> mintURI;
    std::optional<uint256> mintDigest;
    if (ctx.tx.isFieldPresent(sfMintURIToken))
    {
        nativeRemit += objectReserve;

        STObject const& mint = const_cast<ripple::STTx&>(tx)
                                      .getField(sfMintURIToken)
                                      .downcast<STObject>();
        mintURI = mint.getFieldVL(sfURI);

        if (mint.isFieldPresent(sfDigest))
            mintDigest = mint.getFieldH256(sfDigest);

        // check that it doesn't already exist
        mintKL = keylet::uritoken(id, *mintURI);
        
        if (ctx.view.exists(*mintKL))
        {
            JLOG(ctx.j.trace())
                << "Remit: tried to creat duplicate URIToken. Tx: "
                << ctx.tx.getTransactionID();
            return tecDUPLICATE;
        }
    }

    // iterate trustlines to see if these exist
    if (ctx.tx.isFieldPresent(sfAmounts))
    {
        STArray const& sEntries(obj.getFieldArray(sfAmounts));
        for (STObject const& sEntry : sEntries)
        {
            STAmount const amt = sEntry.getFieldAmount(sfAmount);
            if (isXRP(amt))
            {
                nativeRemit = amt.xrp();
                continue;
            }

            Keylet destLineKL = keylet::line(dstID, amt.getIssuer(), amt.getCurrency());
            if (!ctx.view.exists(destLineKL))
                nativeRemit += objectReserve;

            // if the sender is the issuer we'll just assume they're allowed to send it
            // the trustline might be frozen but since its their asset they can do what they want
            if (id == amt.getIssuer())
                continue;

            // check there's a source line and the amount is sufficient
            Keylet srcLineKL = keylet::line(id, amt.getIssuer(), amt.getCurrency());
            if (!ctx.view.exists(srcLineKL))
            {
                JLOG(ctx.j.trace())
                    << "Remit: sender lacked one or more trustlines for remitted currencies.";
                return tecUNFUNDED_PAYMENT;
            }

            // check permissions
            if (TER canXfer =
                trustTransferAllowed(
                    ctx.view,
                    {id, dstID},
                    ctx.j); canXfer != tesSUCCESS)
                return canXfer;

            //bool srcHigh = id > amt.getIssuer();

            auto const srcLine = ctx.view.read(srcLineKL);
            STAmount bal = srcLine.getFieldAmount(sfBalance);

            /*
               A = src acc
               B = amt.getIssuer()

               A > B    | bal > 0   | issuer is         | low acc | high acc
               ----------------------------             
               false    | false     | A                     A           B
               false    | true      | B                     A           B
               true     | false     | B                     B           A
               true     | true      | A                     B           A

               A cannot be the issuer, this would mean you specify someone who holds a trustline with you
               and has a balance issued by you, as the issuer on a transfer to someone else.

             */


            bool const srcHigh = id > amt.getIssuer();
            bool const balPos = bal >= beast::zero;

            if (srcHigh && balPos || !srcHigh && !balPos)
            {
                JLOG(ctx.j.warn())
                    << "Remit: specified issuer is not the issuer in the trustline between source and that account.";
                return tecUNFUNDED_PAYMENT;
            }

            if (bal < beast::zero)
                bal = -bal;

            if (bal.iou() < amt.iou())
            {
                JLOG(ctx.j.warn())
                    << "Remit: one or more currencies were not adequately covered by available funds in account.";
                return tecUNFUNDED_PAYMENT;
            }
        }
    }
    

    // iterate uritokens
    if (ctx.tx.isFieldPresent(sfURITokenIDs))
    {
        STVector256 ids = ctx.tx.getFieldV256(sfURITokenIDs);
        for (uint256 const utKL : ids)
        {
            auto ut = ctx.view.read(utKL);
            
            // does it exist
            if (!ut)
            {
                JLOG(ctx.j.warn())
                    << "Remit: one or more uritokens did not exist on the source account.";
                return tecUNFUNDED_PAYMENT;
            }

            // is it a uritoken?
            if (ut->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
            {
                JLOG(ctx.j.warn())
                    << "Remit: one or more supplied URITokenIDs was not actually a uritoken.";
                return tecNO_ENTRY;
            }

            // is it our uritoken?
            if (ut->getAccounntID(sfOwner) != id)
            {
                JLOG(ctx.j.warn())
                    << "Remit: one or more supplied URITokenIDs was not owned by sender.";
                return tecNO_PERMISSION;
            }
        }
    }

    // ensure the account can cover the native remit
   
    // RH UPTO 

    return tesSUCCESS;
}

TER
Remit::doApply()
{
    // RH TODO

    return tesSUCCESS;
}

XRPAmount
Remit::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount extraFee{0};

    if (tx.isFieldPresent(sfBlob))
        extraFee +=
            XRPAmount{static_cast<XRPAmount>(tx.getFieldVL(sfBlob).size())};

    return Transactor::calculateBaseFee(view, tx) + extraFee;
}

}  // namespace ripple
