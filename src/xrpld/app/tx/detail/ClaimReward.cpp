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

#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/tx/detail/ClaimReward.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/st.h>

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

    if (ctx.tx.isFieldPresent(sfClaimCurrency))
    {
        // IOU RewardClaim
        if (!ctx.rules.enabled(featureIOURewardClaim))
            return temDISABLED;

        auto const claimAsset = ctx.tx[sfClaimCurrency];
        bool const isMPT = claimAsset.holds<MPTIssue>();

        if (isMPT)
        {
            JLOG(ctx.j.debug()) << "ClaimReward: MPT is not supported yet.";
            return temMALFORMED;
        }

        auto const claimIssue = claimAsset.get<Issue>();
        if (claimIssue.account == beast::zero || isXRP(claimIssue.currency))
            return temMALFORMED;

        if (claimIssue.account == ctx.tx.getAccountID(sfAccount))
            return temMALFORMED;
    }

    if (ctx.rules.enabled(featureXahauGenesis) &&
        ctx.rules.enabled(featureIOURewardClaim) &&
        ctx.tx.isFieldPresent(sfIssuer))
    {
        static auto const genesisAccountId = calcAccountID(
            generateKeyPair(
                KeyType::secp256k1, generateSeed("masterpassphrase"))
                .first);
        auto const issuer = ctx.tx.getAccountID(sfIssuer);
        if (ctx.tx.isFieldPresent(sfClaimCurrency))
        {
            if (issuer == genesisAccountId)
            {
                JLOG(ctx.j.debug()) << "ClaimReward (IOU): Issuer cannot "
                                       "be the Genesis account";
                return temBAD_ISSUER;
            }
        }
        else
        {
            if (issuer != genesisAccountId)
            {
                JLOG(ctx.j.debug()) << "ClaimReward (XAH): Issuer must be "
                                       "the Genesis account";
                return temBAD_ISSUER;
            }
        }
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

    if (issuer)
    {
        auto const sleIssuer = ctx.view.read(keylet::account(*issuer));
        if (!sleIssuer)
            return tecNO_ISSUER;

        if (sleIssuer->isFieldPresent(sfAMMID))
            return tecNO_PERMISSION;

        if (ctx.view.rules().enabled(featureIOURewardClaim))
        {
            auto const& sleHook = ctx.view.read(keylet::hook(*issuer));
            if (!sleHook || !sleHook->isFieldPresent(sfHooks) ||
                sleHook->getFieldArray(sfHooks).empty())
                return tecNO_TARGET;

            bool hasClaimRewardHook = false;
            auto const& hooks = sleHook->getFieldArray(sfHooks);
            for (auto const& hook : hooks)
            {
                if (!hook.isFieldPresent(sfHookHash))
                    return tefINTERNAL;  // LCOV_EXCL_LINE
                auto const& hash = hook.getFieldH256(sfHookHash);
                auto const& sleDef =
                    ctx.view.read(keylet::hookDefinition(hash));
                if (!sleDef)
                    return tefINTERNAL;  // LCOV_EXCL_LINE

                auto const& hookOn =
                    hook::getHookOn(hook, sleDef, sfHookOnIncoming);
                if (hook::canHook(ttCLAIM_REWARD, hookOn))
                {
                    hasClaimRewardHook = true;
                    break;
                }
            }
            if (!hasClaimRewardHook)
                return tecNO_TARGET;
        }
    }

    if (ctx.tx.isFieldPresent(sfClaimCurrency))
    {
        auto const claimCurrency = ctx.tx[sfClaimCurrency];
        bool const isMPT = claimCurrency.holds<MPTIssue>();

        if (isMPT)
            return tefINTERNAL;

        auto const claimIssue = claimCurrency.get<Issue>();

        if (!ctx.view.exists(
                keylet::line(id, claimIssue.account, claimIssue.currency)))
            return tecNO_LINE;
    }
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

    uint32_t lgrCur = view().seq();
    uint32_t lgrFirst = lgrCur;
    uint32_t lgrLast = lgrCur;
    uint64_t accumulator = 0ULL;
    uint32_t rewardTime = std::chrono::duration_cast<std::chrono::seconds>(
                              ctx_.app.getLedgerMaster()
                                  .getValidatedLedger()
                                  ->info()
                                  .parentCloseTime.time_since_epoch())
                              .count();

    if (ctx_.tx.isFieldPresent(sfClaimCurrency))
    {
        auto const claimCurrency = ctx_.tx[sfClaimCurrency];
        bool const isMPT = claimCurrency.holds<MPTIssue>();

        if (isMPT)
            return tefINTERNAL;

        auto const claimIssue = claimCurrency.get<Issue>();

        auto lineSle = view().peek(
            keylet::line(account_, claimIssue.account, claimIssue.currency));
        if (!lineSle)
            return tefINTERNAL;

        bool const isHigh = account_ > claimIssue.account;
        auto const& rewardField = isHigh ? sfHighReward : sfLowReward;

        if (isOptOut)
        {
            if (lineSle->isFieldPresent(rewardField))
                lineSle->makeFieldAbsent(rewardField);
        }
        else
        {
            auto const& rewardAccumulatorField =
                lineSle->getFieldAmount(isHigh ? sfHighLimit : sfLowLimit)
                    .zeroed();

            // all actual rewards are handled by the hook on the sfIssuer
            // the tt just resets the counters
            auto& reward = lineSle->peekFieldObject(rewardField);
            reward.setFieldU32(sfRewardLgrFirst, lgrFirst);
            reward.setFieldU32(sfRewardLgrLast, lgrLast);
            reward.setFieldAmount(
                sfTrustLineRewardAccumulator, rewardAccumulatorField);
            reward.setFieldU32(sfRewardTime, rewardTime);
        }

        view().update(lineSle);
        return tesSUCCESS;
    }

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
        sle->setFieldU32(sfRewardLgrFirst, lgrFirst);
        sle->setFieldU32(sfRewardLgrLast, lgrLast);
        sle->setFieldU64(sfRewardAccumulator, accumulator);
        sle->setFieldU32(sfRewardTime, rewardTime);
    }

    view().update(sle);
    return tesSUCCESS;
}

}  // namespace ripple
