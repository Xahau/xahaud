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

#include <ripple/app/tx/impl/BrokerCreate.h>
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
BrokerCreate::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
BrokerCreate::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfUniversalMask)
    {
        // There are no flags (other than universal).
        JLOG(ctx.j.warn()) << "Malformed transaction: Invalid flags set.";
        return temINVALID_FLAG;
    }
}

TER
BrokerCreate::doApply()
{
    if (!view().rules().enabled(featureOptions))
        return temDISABLED;

    Sandbox sb(&ctx_.view());

    beast::Journal const& j = ctx_.journal;

    auto const srcAccID = ctx_.tx[sfAccount];

    auto sleSrcAcc = sb.peek(keylet::account(srcAccID));
    if (!sleSrcAcc)
        return terNO_ACCOUNT;

    XRPAmount const accountReserve{sb.fees().accountReserve(0)};
    XRPAmount const objectReserve{sb.fees().accountReserve(1) - accountReserve};

    // amount of native tokens we will transfer to cover reserves for the
    // tls/acc/uritokens created, and native tokens listed in amounts
    XRPAmount nativeAmount{0};

    // AccountID const dstAccID{ctx_.tx[sfDestination]};
    auto const brokerKeylet = keylet::broker();
    auto sleBrokerAcc = sb.peek(brokerKeylet);
    auto const flags = !sleBrokerAcc ? 0 : sleBrokerAcc->getFlags();

    std::cout << "flags: " << flags << "\n";

    // Check if the destination has disallowed incoming
    if (sb.rules().enabled(featureDisallowIncoming) &&
        (flags & lsfDisallowIncomingRemit))
        return tecNO_PERMISSION;

    // the destination may require a dest tag
    // if ((flags & lsfRequireDestTag) &&
    //     !ctx_.tx.isFieldPresent(sfDestinationTag))
    // {
    //     JLOG(j.warn())
    //         << "BrokerCreate: DestinationTag required for this destination.";
    //     return tecDST_TAG_NEEDED;
    // }

    auto const brokerAccID = [&]() -> Expected<AccountID, TER> {
        std::uint16_t constexpr maxAccountAttempts = 256;
        for (auto p = 0; p < maxAccountAttempts; ++p)
        {
            auto const brokerAccount =
                brokerAccountID(p, sb.info().parentHash, brokerKeylet.key);
            if (!sb.read(keylet::account(brokerAccount)))
                return brokerAccount;
        }
        return Unexpected(tecDUPLICATE);
    }();

    std::cout << "brokerAccID: " << *brokerAccID << "\n";

    if (!brokerAccID)
    {
        JLOG(j_.error()) << "Broker: Broker already exists.";
        return brokerAccID.error();
    }

    // if the destination doesn't exist, create it.
    bool const createDst = !sleBrokerAcc;
    if (createDst)
    {
        // sender will pay the reserve
        nativeAmount += accountReserve;

        // Create the account.
        std::uint32_t const seqno{
            sb.rules().enabled(featureXahauGenesis)
                ? sb.info().parentCloseTime.time_since_epoch().count()
                : sb.rules().enabled(featureDeletableAccounts) ? sb.seq() : 1};

        sleBrokerAcc = std::make_shared<SLE>(keylet::account(*brokerAccID));
        sleBrokerAcc->setAccountID(sfAccount, *brokerAccID);

        sleBrokerAcc->setFieldU32(sfSequence, seqno);
        sleBrokerAcc->setFieldU32(sfOwnerCount, 0);

        if (sb.exists(keylet::fees()) &&
            sb.rules().enabled(featureXahauGenesis))
        {
            auto sleFees = sb.peek(keylet::fees());
            uint64_t accIdx = sleFees->isFieldPresent(sfAccountCount)
                ? sleFees->getFieldU64(sfAccountCount)
                : 0;
            sleBrokerAcc->setFieldU64(sfAccountIndex, accIdx);
            sleFees->setFieldU64(sfAccountCount, accIdx + 1);
            sb.update(sleFees);
        }

        // we'll fix this up at the end
        sleBrokerAcc->setFieldAmount(sfBalance, STAmount{XRPAmount{0}});
        sb.insert(sleBrokerAcc);
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
        JLOG(j.warn()) << "BrokerCreate: sender " << srcAccID
                       << " lacks reserves to cover send.";
        return tecINSUFFICIENT_RESERVE;
    }

    // this isn't actually an error but we will print a warning
    // this can occur if the destination was already below reserve level at the
    // time assets were sent
    if (!hasSufficientReserve(sleBrokerAcc))
    {
        JLOG(j.warn()) << "BrokerCreate: destination has insufficient reserves.";
    }

    // // apply
    sb.update(sleSrcAcc);
    sb.update(sleBrokerAcc);
    sb.apply(ctx_.rawView());
    
    return tesSUCCESS;
}

XRPAmount
BrokerCreate::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
