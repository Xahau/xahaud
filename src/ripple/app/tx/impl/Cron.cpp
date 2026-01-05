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

#include <ripple/app/tx/impl/Cron.h>
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
Cron::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
Cron::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureCron))
        return temDISABLED;

    auto const ret = preflight0(ctx);
    if (!isTesSuccess(ret))
        return ret;

    auto account = ctx.tx.getAccountID(sfAccount);
    if (account != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Cron: Bad source id";
        return temBAD_SRC_ACCOUNT;
    }

    // No point in going any further if the transaction fee is malformed.
    auto const fee = ctx.tx.getFieldAmount(sfFee);
    if (!fee.native() || fee != beast::zero)
    {
        JLOG(ctx.j.warn()) << "Cron: invalid fee";
        return temBAD_FEE;
    }

    if (!ctx.tx.getSigningPubKey().empty() || !ctx.tx.getSignature().empty() ||
        ctx.tx.isFieldPresent(sfSigners))
    {
        JLOG(ctx.j.warn()) << "Cron: Bad signature";
        return temBAD_SIGNATURE;
    }

    if (ctx.tx.getFieldU32(sfSequence) != 0 ||
        ctx.tx.isFieldPresent(sfPreviousTxnID))
    {
        JLOG(ctx.j.warn()) << "Cron: Bad sequence";
        return temBAD_SEQUENCE;
    }

    return tesSUCCESS;
}

TER
Cron::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureCron))
        return temDISABLED;

    return tesSUCCESS;
}

TER
Cron::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;

    if (view.rules().enabled(fixCronStacking))
    {
        if (auto const seq = tx.getFieldU32(sfLedgerSequence);
            seq != view.info().seq)
        {
            JLOG(j_.warn()) << "Cron: wrong ledger seq=" << seq;
            return tefFAILURE;
        }
    }

    AccountID const& id = tx.getAccountID(sfOwner);

    auto sle = view.peek(keylet::account(id));
    if (!sle)
    {
        // should never happen... but might in a race condition with acc delete
        JLOG(j_.warn()) << "Cron: sfOwner account missing. " << id;
        return tesSUCCESS;
    }

    if (!sle->isFieldPresent(sfCron))
    {
        JLOG(j_.warn()) << "Cron: sfCron missing from account " << id;
        return tefINTERNAL;
    }

    uint256 ptr = sle->getFieldH256(sfCron);
    Keylet klOld{ltCRON, ptr};
    auto sleCron = view.peek(klOld);
    if (!sleCron)
    {
        JLOG(j_.warn()) << "Cron: Cron object missing for account " << id;
        return tesSUCCESS;
    }

    uint32_t delay = sleCron->getFieldU32(sfDelaySeconds);
    uint32_t recur = sleCron->getFieldU32(sfRepeatCount);

    uint32_t lastStartTime = sleCron->getFieldU32(sfStartTime);

    // do all this sanity checking before we modify the ledger...
    uint32_t afterTime = lastStartTime + delay;
    if (afterTime < lastStartTime)
        return tefINTERNAL;

    // in all circumstances the Cron object is deleted...
    // if there are further crons to do then a new one is created at the next
    // time point

    if (!view.dirRemove(
            keylet::ownerDir(id), (*sleCron)[sfOwnerNode], klOld, false))
        return tefBAD_LEDGER;

    view.erase(sleCron);

    if (recur == 0)
    {
        // already at last execution, stop here
        adjustOwnerCount(view, sle, -1, j_);
        sle->makeFieldAbsent(sfCron);
        view.update(sle);
        return tesSUCCESS;
    }

    // more executions remain, so create a new object

    Keylet klCron = keylet::cron(afterTime, id);

    // insert into owner dir, we don't need to check reserve because we've just
    // deleted an object
    auto const page =
        view.dirInsert(keylet::ownerDir(id), klCron, describeOwnerDir(id));
    if (!page)
        return tecDIR_FULL;

    sleCron = std::make_shared<SLE>(klCron);

    sleCron->setFieldU64(sfOwnerNode, *page);
    sleCron->setFieldU32(sfDelaySeconds, delay);
    sleCron->setFieldU32(sfRepeatCount, recur - 1);
    sleCron->setFieldU32(sfStartTime, afterTime);
    sleCron->setAccountID(sfOwner, id);

    sle->setFieldH256(sfCron, klCron.key);

    view.insert(sleCron);
    view.update(sle);

    return tesSUCCESS;
}

void
Cron::preCompute()
{
    assert(account_ == beast::zero);
}

XRPAmount
Cron::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return XRPAmount{0};
}

}  // namespace ripple
