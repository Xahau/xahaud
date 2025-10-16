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

#include <ripple/app/tx/impl/SetCron.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>

namespace ripple {

TxConsequences
SetCron::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
SetCron::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureCron))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;
    auto& j = ctx.j;

    if (tx.getFlags() & tfCronSetMask)
    {
        JLOG(j.warn()) << "SetCron: Invalid flags set.";
        return temINVALID_FLAG;
    }

    // DelaySeconds (D), RepeatCount (R)
    // DR - Set Cron with Delay and Repeat
    // D- - Set Cron (once off) with Delay only (repat implicitly 0)
    // -R - Invalid
    // -- - Clear any existing cron (succeeds even if there isn't one) / with
    // tfCronUnset flag set

    bool const hasDelay = tx.isFieldPresent(sfDelaySeconds);
    bool const hasRepeat = tx.isFieldPresent(sfRepeatCount);

    if (tx.isFlag(tfCronUnset))
    {
        if (hasDelay || hasRepeat)
        {
            JLOG(j.debug()) << "SetCron: tfCronUnset flag cannot be used with "
                               "DelaySeconds or RepeatCount.";
            return temMALFORMED;
        }
    }
    else
    {
        if (!hasDelay)
        {
            JLOG(j.debug()) << "SetCron: DelaySeconds must be "
                               "specified to create a cron.";
            return temMALFORMED;
        }

        // check delay is not too high
        auto delay = tx.getFieldU32(sfDelaySeconds);
        if (delay > 31536000UL /* 365 days in seconds */)
        {
            JLOG(j.debug()) << "SetCron: DelaySeconds was too high. (max 365 "
                               "days in seconds).";
            return temMALFORMED;
        }

        // check repeat is not too high
        if (hasRepeat)
        {
            auto recur = tx.getFieldU32(sfRepeatCount);
            if (recur > 256)
            {
                JLOG(j.debug())
                    << "SetCron: RepeatCount too high. Limit is 256. Issue "
                       "new SetCron to increase.";
                return temMALFORMED;
            }
        }
    }

    return preflight2(ctx);
}

TER
SetCron::preclaim(PreclaimContext const& ctx)
{
    return tesSUCCESS;
}

TER
SetCron::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;

    bool const isDelete = tx.isFlag(tfCronUnset);

    // delay can be zero, in which case the cron will usually execute next
    // ledger.
    uint32_t delay{0};
    uint32_t recur{0};

    if (!isDelete)
    {
        if (tx.isFieldPresent(sfDelaySeconds))
            delay = tx.getFieldU32(sfDelaySeconds);
        if (tx.isFieldPresent(sfRepeatCount))
            recur = tx.getFieldU32(sfRepeatCount);
    }

    uint32_t currentTime = view.parentCloseTime().time_since_epoch().count();

    // do all this sanity checking before we modify the ledger...
    // even for a delete operation this will fall through without incident

    uint32_t afterTime = currentTime + delay;
    if (afterTime < currentTime)
        return tefINTERNAL;

    AccountID const& id = tx.getAccountID(sfAccount);
    auto sle = view.peek(keylet::account(id));
    if (!sle)
        return tefINTERNAL;

    // in all cases whatsoever, this transaction will delete an existing
    // old cron object and return the owner reserve to the owner.

    if (sle->isFieldPresent(sfCron))
    {
        Keylet klOld{ltCRON, sle->getFieldH256(sfCron)};

        auto sleCron = view.peek(klOld);
        if (!sleCron)
        {
            JLOG(j_.warn()) << "SetCron: Cron object didn't exist.";
            return tefBAD_LEDGER;
        }

        if (safe_cast<LedgerEntryType>(
                sleCron->getFieldU16(sfLedgerEntryType)) != ltCRON)
        {
            JLOG(j_.warn()) << "SetCron: sfCron pointed to non-cron object!!";
            return tefBAD_LEDGER;
        }

        if (!view.dirRemove(
                keylet::ownerDir(id), (*sleCron)[sfOwnerNode], klOld, false))
        {
            JLOG(j_.warn()) << "SetCron: Ownerdir bad. " << id;
            return tefBAD_LEDGER;
        }

        view.erase(sleCron);
        adjustOwnerCount(view, sle, -1, j_);
        sle->makeFieldAbsent(sfCron);
    }

    // if the operation is a delete (no delay or recur specified then stop
    // here.)
    if (isDelete)
    {
        view.update(sle);
        return tesSUCCESS;
    }

    // execution to here means we're creating a new Cron object and adding it to
    // the user's owner dir

    Keylet klCron = keylet::cron(afterTime, id);

    std::shared_ptr<SLE> sleCron = std::make_shared<SLE>(klCron);

    STAmount const reserve{
        view.fees().accountReserve(sle->getFieldU32(sfOwnerCount) + 1)};

    STAmount const afterFee =
        mPriorBalance - ctx_.tx.getFieldAmount(sfFee).xrp();

    if (afterFee > mPriorBalance || afterFee < reserve)
        return tecINSUFFICIENT_RESERVE;

    // add to owner dir
    auto const page =
        view.dirInsert(keylet::ownerDir(id), klCron, describeOwnerDir(id));
    if (!page)
        return tecDIR_FULL;

    sleCron->setFieldU64(sfOwnerNode, *page);

    adjustOwnerCount(view, sle, 1, j_);

    // set the fields
    sleCron->setFieldU32(sfDelaySeconds, delay);
    sleCron->setFieldU32(sfRepeatCount, recur);
    sleCron->setAccountID(sfOwner, id);

    sle->setFieldH256(sfCron, klCron.key);

    view.update(sle);

    view.insert(sleCron);

    return tesSUCCESS;
}

XRPAmount
SetCron::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto const baseFee = Transactor::calculateBaseFee(view, tx);

    auto const hasRepeat = tx.isFieldPresent(sfRepeatCount);

    if (tx.isFlag(tfCronUnset))
        // delete cron
        return baseFee;

    // factor a cost based on the total number of txns expected
    // for RepeatCount of 0 we have this txn (SetCron) and the
    // single Cron txn (2). For a RepeatCount of 1 we have this txn,
    // the first time the cron executes, and the second time (3).
    uint32_t const additionalExpectedExecutions =
        hasRepeat ? tx.getFieldU32(sfRepeatCount) + 1 : 1;
    auto const additionalFee = baseFee * additionalExpectedExecutions;

    if (baseFee + additionalFee < baseFee)
        return baseFee;

    return baseFee + additionalFee;
}

}  // namespace ripple
