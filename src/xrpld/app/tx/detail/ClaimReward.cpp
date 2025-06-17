//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/tx/impl/ClaimReward.h>
#include <ripple/basics/Log.h>
#include <ripple/core/Config.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/st.h>

namespace ripple {

TxConsequences
ClaimReward::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
ClaimReward::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureBalanceRewards))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    // can have flag 1 set to opt-out of rewards
    auto const invalidFlags = ctx.rules.enabled(fixRewardClaimFlags)
        ? (ctx.tx.getFlags() & tfClaimRewardMask)
        : (ctx.tx.isFieldPresent(sfFlags) &&
           ctx.tx.getFieldU32(sfFlags) > tfOptOut);
    if (invalidFlags)
        return temINVALID_FLAG;

    if (ctx.tx.isFieldPresent(sfIssuer) &&
        ctx.tx.getAccountID(sfIssuer) == ctx.tx.getAccountID(sfAccount))
    {
        JLOG(ctx.j.warn())
            << "ClaimReward: Issuer cannot be the source account.";
        return temMALFORMED;
    }

    return preflight2(ctx);
}

TER
ClaimReward::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureBalanceRewards))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    std::optional<uint32_t> flags = ctx.tx[~sfFlags];
    std::optional<AccountID const> issuer = ctx.tx[~sfIssuer];

    bool isOptOut = flags && *flags == tfOptOut;
    if ((issuer && isOptOut) || (!issuer && !isOptOut))
        return temMALFORMED;

    if (issuer && !ctx.view.exists(keylet::account(*issuer)))
        return tecNO_ISSUER;

    return tesSUCCESS;
}

TER
ClaimReward::doApply()
{
    auto const sle = view().peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    std::optional<uint32_t> flags = ctx_.tx[~sfFlags];

    bool isOptOut = flags && *flags == tfOptOut;
    if (isOptOut)
    {
        if (sle->isFieldPresent(sfRewardLgrFirst))
            sle->makeFieldAbsent(sfRewardLgrFirst);
        if (sle->isFieldPresent(sfRewardLgrLast))
            sle->makeFieldAbsent(sfRewardLgrLast);
        if (sle->isFieldPresent(sfRewardAccumulator))
            sle->makeFieldAbsent(sfRewardAccumulator);
        if (sle->isFieldPresent(sfRewardTime))
            sle->makeFieldAbsent(sfRewardTime);
    }
    else
    {
        // all actual rewards are handled by the hook on the sfIssuer
        // the tt just resets the counters
        uint32_t lgrCur = view().seq();
        sle->setFieldU32(sfRewardLgrFirst, lgrCur);
        sle->setFieldU32(sfRewardLgrLast, lgrCur);
        sle->setFieldU64(sfRewardAccumulator, 0ULL);
        sle->setFieldU32(
            sfRewardTime,
            std::chrono::duration_cast<std::chrono::seconds>(
                ctx_.app.getLedgerMaster()
                    .getValidatedLedger()
                    ->info()
                    .parentCloseTime.time_since_epoch())
                .count());
    }

    view().update(sle);
    return tesSUCCESS;
}

}  // namespace ripple
