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
#include <ripple/core/Config.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/Quality.h>
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

    if (tx.getFlags() & tfUniversalMask)
    {
        JLOG(j.warn()) << "SetCron: Invalid flags set.";
        return temINVALID_FLAG;
    }

    // DelaySeconds (D), RepeatCount (R)
    // DR - Set Cron with Delay and Repeat
    // D- - Set Cron (once off) with Delay only (repat implicitly 0)
    // -R - Invalid
    // -- - Clear any existing cron (succeeds even if there isn't one)

    if (tx.isFieldPresent(sfRepeatCount) && !tx.isFieldPresent(sfDelaySeconds))
    {
        JLOG(j.warn()) << "SetCron: DelaySeconds must also be specified when "
                          "RepeatCount is present.";
        return temMALFORMED;
    }

    return preflight2(ctx);
}

TER
SetCron::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureCron))
        return temDISABLED;

    auto& j = ctx.j;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    bool const hasDelay = ctx.tx.isFieldPresent(sfDelaySeconds);
    bool const hasRepeat = ctx.tx.isFieldPresent(sfRepeatCount);

    // defensively enforce this even though we did it in preflight
    if (!hasDelay && hasRepeat)
        return tefINTERNAL;

    if (!hasDelay)
    {
        // delete operation always succeeds even if there's nothing to delete
        return tesSUCCESS;
    }

    // set operation

    auto delay = ctx.tx.getFieldU32(sfDelaySeconds);
    if (delay > 1209600UL /* 14 days in seconds */)
    {
        JLOG(j.debug())
            << "SetCron: DelaySeconds was too high. (max 14 days in seconds).";
        return tecDELAY_OR_REPEAT_COUNT_TOO_LARGE;
    }

    if (!hasRepeat)
        return tesSUCCESS;

    auto recur = ctx.tx.getFieldU32(sfRepeatCount);
    if (recur > 256)
    {
        JLOG(j.debug()) << "SetCron: RepeatCount too high. Limit is 256. Issue "
                           "new SetCron to increase.";
        return tecDELAY_OR_REPEAT_COUNT_TOO_LARGE;
    }

    return tesSUCCESS;
}

TER
SetCron::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;

    bool const isDelete = !tx.isFieldPresent(sfDelaySeconds);

    if (isDelete && tx.isFieldPresent(sfRepeatCount))
        return tefINTERNAL;

    // delay can be zero, in which case the cron will usually execute next
    // ledger.
    uint32_t delay{0};
    uint32_t recur{0};

    if (!isDelete)
    {
        delay = tx.getFieldU32(sfDelaySeconds);
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

        if (safe_cast<TxType>(sleCron->getFieldU16(sfLedgerEntryType)) !=
            ltCRON)
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

    bool const alreadyExists = view.exists(klCron);

    if (!alreadyExists)
    {
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

        adjustOwnerCount(view, sle, 1, j_);
    }

    std::shared_ptr<SLE> sleCron =
        alreadyExists ? view.peek(klCron) : std::make_shared<SLE>(klCron);

    // set the fields
    sleCron->setFieldU32(sfDelaySeconds, delay);
    sleCron->setFieldU32(sfRepeatCount, recur);
    sleCron->setAccountID(sfOwner, id);

    sle->setFieldH256(sfCron, klCron.key);

    view.update(sle);

    if (alreadyExists)
        view.update(sleCron);
    else
        view.insert(sleCron);

    return tesSUCCESS;
}

XRPAmount
SetCron::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto fee = Transactor::calculateBaseFee(view, tx);
    return fee;
}

}  // namespace ripple
