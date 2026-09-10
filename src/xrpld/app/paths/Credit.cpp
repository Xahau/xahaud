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

#include <xrpld/ledger/ReadView.h>
#include <xrpl/protocol/AmountConversions.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STAmount.h>

namespace ripple {

//@@start limit-contract
// Trust line limits are consulted here and in creditLimit2, by the payment
// engine's direct step, which applies them to intermediary hops and (with
// featureNoRecipientLimit) not to the issuer's step into the destination.
// The only other reader is trustTransferLockedBalance, gated the same way.
// A transactor that credits an account through accountSend or rippleCredit
// never reads a limit; keep it that way so the invariant "a limit governs an
// intermediary, never an account receiving its issuer's token" holds for
// every transaction type without per-transactor exceptions.
//@@end limit-contract
STAmount
creditLimit(
    ReadView const& view,
    AccountID const& account,
    AccountID const& issuer,
    Currency const& currency)
{
    STAmount result(Issue{currency, account});

    auto sleRippleState = view.read(keylet::line(account, issuer, currency));

    if (sleRippleState)
    {
        result = sleRippleState->getFieldAmount(
            account < issuer ? sfLowLimit : sfHighLimit);
        result.setIssuer(account);
    }

    XRPL_ASSERT(
        result.getIssuer() == account,
        "ripple::creditLimit : result issuer match");
    XRPL_ASSERT(
        result.getCurrency() == currency,
        "ripple::creditLimit : result currency match");
    return result;
}

IOUAmount
creditLimit2(
    ReadView const& v,
    AccountID const& acc,
    AccountID const& iss,
    Currency const& cur)
{
    return toAmount<IOUAmount>(creditLimit(v, acc, iss, cur));
}

STAmount
creditBalance(
    ReadView const& view,
    AccountID const& account,
    AccountID const& issuer,
    Currency const& currency)
{
    STAmount result(Issue{currency, account});

    auto sleRippleState = view.read(keylet::line(account, issuer, currency));

    if (sleRippleState)
    {
        result = sleRippleState->getFieldAmount(sfBalance);
        if (account < issuer)
            result.negate();
        result.setIssuer(account);
    }

    XRPL_ASSERT(
        result.getIssuer() == account,
        "ripple::creditBalance : result issuer match");
    XRPL_ASSERT(
        result.getCurrency() == currency,
        "ripple::creditBalance : result currency match");
    return result;
}

}  // namespace ripple
