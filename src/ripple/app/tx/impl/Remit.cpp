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
#include <ripple/app/tx/impl/URIToken.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>

namespace ripple {

TxConsequences
Remit::makeTxConsequences(PreflightContext const& ctx)
{
    XRPAmount native = ([&ctx]() -> XRPAmount {
        if (!ctx.tx.isFieldPresent(sfAmounts))
            return beast::zero;

        STArray const& sEntries(ctx.tx.getFieldArray(sfAmounts));
        for (STObject const& sEntry : sEntries)
        {
            if (!sEntry.isFieldPresent(sfAmount))
                continue;

            STAmount const amount = sEntry.getFieldAmount(sfAmount);
            if (isXRP(amount))
                return amount.xrp();
        }
        return beast::zero;
    })();
    return TxConsequences{ctx.tx, native};
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

    AccountID const dstID = ctx.tx.getAccountID(sfDestination);
    AccountID const srcID = ctx.tx.getAccountID(sfAccount);

    if (dstID == srcID)
    {
        JLOG(ctx.j.warn()) << "Malformed transaction: Remit to self.";
        return temREDUNDANT;
    }

    if (ctx.rules.enabled(fix20250131))
    {
        if (!dstID || dstID == noAccount())
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction: Remit to invalid account.";
            return temMALFORMED;
        }
    }

    if (ctx.tx.isFieldPresent(sfInform))
    {
        AccountID const infID = ctx.tx.getAccountID(sfInform);
        if (infID == dstID || srcID == infID)
        {
            JLOG(ctx.j.warn()) << "Malformed transaction: sfInform is same as "
                                  "source or destination.";
            return temMALFORMED;
        }
    }

    if (ctx.tx.isFieldPresent(sfBlob) &&
        ctx.tx.getFieldVL(sfBlob).size() > (128 * 1024))
    {
        JLOG(ctx.j.warn()) << "Blob was more than 128kib "
                           << ctx.tx.getTransactionID();
        return temMALFORMED;
    }

    // sanity check amounts
    if (ctx.tx.isFieldPresent(sfAmounts))
    {
        if (ctx.tx.getFieldArray(sfAmounts).size() > 32)
        {
            JLOG(ctx.j.warn()) << "Malformed: AmountEntry count exceeds `32`.";
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

    // sanity check minturitoken
    if (ctx.tx.isFieldPresent(sfMintURIToken))
    {
        STObject const& mint = const_cast<ripple::STTx&>(ctx.tx)
                                   .getField(sfMintURIToken)
                                   .downcast<STObject>();

        for (auto const& mintElement : mint)
        {
            auto const& name = mintElement.getFName();
            if (name != sfURI && name != sfFlags && name != sfDigest)
            {
                JLOG(ctx.j.trace()) << "Malformed transaction: sfMintURIToken "
                                       "contains invalid field.";
                return temMALFORMED;
            }
        }
        if (!mint.isFieldPresent(sfURI))
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction: URI was not provided.";
            return temMALFORMED;
        }
        Blob const uri = mint.getFieldVL(sfURI);
        if (uri.size() < 1 || uri.size() > 256)
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction: URI was too short/long.";
            return temMALFORMED;
        }

        if (!URIToken::validateUTF8(uri))
        {
            JLOG(ctx.j.warn())
                << "Malformed transaction: Invalid UTF8 inside MintURIToken.";
            return temMALFORMED;
        }

        if (mint.isFieldPresent(sfFlags))
        {
            if (mint.getFieldU32(sfFlags) & tfURITokenMintMask)
                return temINVALID_FLAG;
        }
    }

    // sanity check uritokenids
    if (ctx.tx.isFieldPresent(sfURITokenIDs))
    {
        STVector256 ids = ctx.tx.getFieldV256(sfURITokenIDs);
        if (ids.size() < 1 || ids.size() > 32)
        {
            JLOG(ctx.j.warn()) << "Malformed transaction: URITokenIDs Invalid.";
            return temMALFORMED;
        }

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
Remit::doApply()
{
    Sandbox sb(&ctx_.view());

    if (!sb.rules().enabled(featureRemit))
        return temDISABLED;

    beast::Journal const& j = ctx_.journal;

    auto const srcAccID = ctx_.tx[sfAccount];

    auto sleSrcAcc = sb.peek(keylet::account(srcAccID));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    if (ctx_.tx.isFieldPresent(sfInform))
    {
        auto const informAcc = ctx_.tx.getAccountID(sfInform);
        if (!sb.exists(keylet::account(informAcc)))
        {
            JLOG(j.warn()) << "Remit: sfInform account does not exist.";
            return tecNO_TARGET;
        }
    }

    XRPAmount const accountReserve{sb.fees().accountReserve(0)};
    XRPAmount const objectReserve{sb.fees().accountReserve(1) - accountReserve};

    // sanity check
    if (accountReserve < beast::zero || objectReserve < beast::zero ||
        objectReserve > sb.fees().accountReserve(1))
    {
        JLOG(j.warn())
            << "Remit: account or object reserve calculation not sane.";
        return tecINTERNAL;
    }

    // amount of native tokens we will transfer to cover reserves for the
    // tls/acc/uritokens created, and native tokens listed in amounts
    XRPAmount nativeRemit{0};

    AccountID const dstAccID{ctx_.tx[sfDestination]};
    auto sleDstAcc = sb.peek(keylet::account(dstAccID));
    auto const flags = !sleDstAcc ? 0 : sleDstAcc->getFlags();

    // Check if the destination has disallowed incoming
    if (sb.rules().enabled(featureDisallowIncoming) &&
        (flags & lsfDisallowIncomingRemit))
        return tecNO_PERMISSION;

    // Check if the destination account requires deposit authorization.
    bool const depositAuth{sb.rules().enabled(featureDepositAuth)};
    if (depositAuth && sleDstAcc && (flags & lsfDepositAuth))
    {
        if (!sb.exists(keylet::depositPreauth(dstAccID, srcAccID)))
            return tecNO_PERMISSION;
    }

    // the destination may require a dest tag
    if ((flags & lsfRequireDestTag) &&
        !ctx_.tx.isFieldPresent(sfDestinationTag))
    {
        JLOG(j.warn())
            << "Remit: DestinationTag required for this destination.";
        return tecDST_TAG_NEEDED;
    }

    // if the destination doesn't exist, create it.
    bool const createDst = !sleDstAcc;
    if (createDst)
    {
        // sender will pay the reserve
        if (nativeRemit + accountReserve < nativeRemit)
            return tecINTERNAL;

        nativeRemit += accountReserve;

        // Create the account.
        std::uint32_t const seqno{
            sb.rules().enabled(featureXahauGenesis)
                ? sb.info().parentCloseTime.time_since_epoch().count()
                : sb.rules().enabled(featureDeletableAccounts) ? sb.seq() : 1};

        sleDstAcc = std::make_shared<SLE>(keylet::account(dstAccID));
        sleDstAcc->setAccountID(sfAccount, dstAccID);

        sleDstAcc->setFieldU32(sfSequence, seqno);
        sleDstAcc->setFieldU32(sfOwnerCount, 0);

        if (sb.exists(keylet::fees()) &&
            sb.rules().enabled(featureXahauGenesis))
        {
            auto sleFees = sb.peek(keylet::fees());
            uint64_t accIdx = sleFees->isFieldPresent(sfAccountCount)
                ? sleFees->getFieldU64(sfAccountCount)
                : 0;
            sleDstAcc->setFieldU64(sfAccountIndex, accIdx);
            sleFees->setFieldU64(sfAccountCount, accIdx + 1);
            sb.update(sleFees);
        }

        // we'll fix this up at the end
        sleDstAcc->setFieldAmount(sfBalance, STAmount{XRPAmount{0}});
        sb.insert(sleDstAcc);
    }

    // if theres a minted uritoken the sender pays for that
    if (ctx_.tx.isFieldPresent(sfMintURIToken))
    {
        if (nativeRemit + objectReserve < nativeRemit)
            return tecINTERNAL;

        nativeRemit += objectReserve;
        STObject const& mint = const_cast<ripple::STTx&>(ctx_.tx)
                                   .getField(sfMintURIToken)
                                   .downcast<STObject>();

        Blob const& mintURI = mint.getFieldVL(sfURI);

        std::optional<uint256> mintDigest;
        if (mint.isFieldPresent(sfDigest))
            mintDigest = mint.getFieldH256(sfDigest);

        Keylet kl = keylet::uritoken(srcAccID, mintURI);

        // check that it doesn't already exist
        if (sb.exists(kl))
        {
            JLOG(j.trace()) << "Remit: tried to creat duplicate URIToken. Tx: "
                            << ctx_.tx.getTransactionID();
            return tecDUPLICATE;
        }

        auto sleMint = std::make_shared<SLE>(kl);

        sleMint->setAccountID(sfOwner, dstAccID);
        sleMint->setAccountID(sfIssuer, srcAccID);

        sleMint->setFieldVL(sfURI, mintURI);

        if (mint.isFieldPresent(sfDigest))
            sleMint->setFieldH256(sfDigest, mint.getFieldH256(sfDigest));

        sleMint->setFieldU32(
            sfFlags,
            mint.isFieldPresent(sfFlags) ? mint.getFieldU32(sfFlags) : 0);

        auto const page = sb.dirInsert(
            keylet::ownerDir(dstAccID), kl, describeOwnerDir(dstAccID));

        JLOG(j_.trace()) << "Adding URIToken to owner directory "
                         << to_string(kl.key) << ": "
                         << (page ? "success" : "failure");

        if (!page)
            return tecDIR_FULL;

        sleMint->setFieldU64(sfOwnerNode, *page);
        sb.insert(sleMint);

        // ensure there is a deletion blocker against the issuer now
        sleSrcAcc->setFieldU32(
            sfFlags, sleSrcAcc->getFlags() | lsfURITokenIssuer);

        adjustOwnerCount(sb, sleDstAcc, 1, j);
    }

    // iterate uritokens
    if (ctx_.tx.isFieldPresent(sfURITokenIDs))
    {
        STVector256 ids = ctx_.tx.getFieldV256(sfURITokenIDs);
        for (uint256 const klRaw : ids)
        {
            Keylet kl = keylet::unchecked(klRaw);
            auto sleU = sb.peek(kl);

            // does it exist
            if (!sleU)
            {
                JLOG(j.warn()) << "Remit: one or more uritokens did not exist "
                                  "on the source account.";
                return tecNO_ENTRY;
            }

            // is it a uritoken?
            if (sleU->getFieldU16(sfLedgerEntryType) != ltURI_TOKEN)
            {
                JLOG(j.warn()) << "Remit: one or more supplied URITokenIDs was "
                                  "not actually a uritoken.";
                return tecNO_ENTRY;
            }

            // is it our uritoken?
            if (sleU->getAccountID(sfOwner) != srcAccID)
            {
                JLOG(j.warn()) << "Remit: one or more supplied URITokenIDs was "
                                  "not owned by sender.";
                return tecNO_PERMISSION;
            }

            // erase current sale offers, if any
            if (sleU->isFieldPresent(sfAmount))
                sleU->makeFieldAbsent(sfAmount);
            if (sleU->isFieldPresent(sfDestination))
                sleU->makeFieldAbsent(sfDestination);

            // pay the reserve
            if (nativeRemit + objectReserve < nativeRemit)
                return tecINTERNAL;

            nativeRemit += objectReserve;

            // remove from sender dir
            {
                auto const page = (*sleU)[sfOwnerNode];
                if (!sb.dirRemove(
                        keylet::ownerDir(srcAccID), page, kl.key, true))
                {
                    JLOG(j.fatal())
                        << "Could not remove URIToken from owner directory";
                    return tefBAD_LEDGER;
                }

                adjustOwnerCount(sb, sleSrcAcc, -1, j);
            }

            // add to dest dir
            {
                auto const page = sb.dirInsert(
                    keylet::ownerDir(dstAccID), kl, describeOwnerDir(dstAccID));

                JLOG(j_.trace()) << "Adding URIToken to owner directory "
                                 << to_string(kl.key) << ": "
                                 << (page ? "success" : "failure");

                if (!page)
                    return tecDIR_FULL;

                sleU->setFieldU64(sfOwnerNode, *page);

                adjustOwnerCount(sb, sleDstAcc, 1, j);
            }

            // change the owner
            sleU->setAccountID(sfOwner, dstAccID);

            sb.update(sleU);
        }
    }

    // iterate trustlines
    if (ctx_.tx.isFieldPresent(sfAmounts))
    {
        // process trustline remits
        STArray const& sEntries(ctx_.tx.getFieldArray(sfAmounts));
        for (STObject const& sEntry : sEntries)
        {
            STAmount const amount = sEntry.getFieldAmount(sfAmount);
            if (isXRP(amount))
            {
                // since we have to pay for all the created objects including
                // possibly the account itself this is paid right at the end,
                // and only if there is balance enough to cover.

                // check for overflow
                if (nativeRemit + amount.xrp() < nativeRemit)
                    return tecINTERNAL;

                nativeRemit += amount.xrp();
                continue;
            }

            AccountID const issuerAccID = amount.getIssuer();

            // check permissions
            if (issuerAccID == srcAccID || issuerAccID == dstAccID)
            {
                // no permission check needed when the issuer sends out or a
                // subscriber sends back RH TODO: move this condition into
                // trustTransferAllowed, guarded by an amendment
            }
            else if (TER canXfer = trustTransferAllowed(
                         sb,
                         std::vector<AccountID>{srcAccID, dstAccID},
                         amount.issue(),
                         j);
                     !isTesSuccess(canXfer))
                return canXfer;

            // compute the amount the source will need to send
            // in remit the sender pays all transfer fees, so that
            // the destination can always be assured they got the exact amount
            // specified. therefore we need to compute the amount + transfer fee
            auto const srcAmt =
                issuerAccID != srcAccID && issuerAccID != dstAccID
                ? multiply(amount, transferRate(sb, issuerAccID))
                : amount;

            // sanity check this calculation
            if (srcAmt < amount || srcAmt > amount + amount)
            {
                JLOG(j.warn()) << "Remit: srcAmt calculation not sane.";
                return tecINTERNAL;
            }

            STAmount availableFunds{
                accountFunds(sb, srcAccID, srcAmt, fhZERO_IF_FROZEN, j)};

            if (availableFunds < srcAmt)
                return tecUNFUNDED_PAYMENT;

            // if the target trustline doesn't exist we need to create it and
            // pay its reserve
            if (!sb.exists(
                    keylet::line(dstAccID, issuerAccID, amount.getCurrency())))
            {
                if (nativeRemit + objectReserve < nativeRemit)
                    return tecINTERNAL;

                nativeRemit += objectReserve;
            }

            // action the transfer
            if (TER result =
                    accountSend(sb, srcAccID, dstAccID, amount, j, true);
                !isTesSuccess(result))
                return result;
        }
    }

    if (nativeRemit < beast::zero)
        return tecINTERNAL;

    if (nativeRemit > beast::zero)
    {
        // ensure the account can cover the native remit
        if (mSourceBalance < nativeRemit)
            return tecUNFUNDED_PAYMENT;

        // subtract the balance from the sender
        {
            STAmount bal = mSourceBalance;
            bal -= nativeRemit;
            if (bal < beast::zero || bal > mSourceBalance)
                return tecINTERNAL;
            sleSrcAcc->setFieldAmount(sfBalance, bal);
        }

        // add the balance to the destination
        {
            STAmount bal = sleDstAcc->getFieldAmount(sfBalance);
            STAmount prior = bal;
            bal += nativeRemit;
            if (bal < beast::zero || bal < prior)
                return tecINTERNAL;
            sleDstAcc->setFieldAmount(sfBalance, bal);
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
        JLOG(j.warn()) << "Remit: sender " << srcAccID
                       << " lacks reserves to cover send.";
        return tecINSUFFICIENT_RESERVE;
    }

    // this isn't actually an error but we will print a warning
    // this can occur if the destination was already below reserve level at the
    // time assets were sent
    if (!hasSufficientReserve(sleDstAcc))
    {
        JLOG(j.warn()) << "Remit: destination has insufficient reserves.";
    }

    // apply
    sb.update(sleSrcAcc);
    sb.update(sleDstAcc);
    sb.apply(ctx_.rawView());

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
