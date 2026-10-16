//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <xrpld/app/main/Application.h>
#include <xrpld/app/tx/detail/NoteClaim.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Rate.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFlags.h>

#include <stdexcept>
#include <string_view>

namespace ripple {

// getFieldVL returns by value; the view is into the caller's copy.
static std::string_view
text(Blob const& raw)
{
    return std::string_view(
        reinterpret_cast<char const*>(raw.data()), raw.size());
}

CreditNote
NoteClaim::note(STTx const& tx)
{
    auto const raw = tx.getFieldVL(sfNote);
    return parseCreditNote(text(raw));
}

NotTEC
NoteClaim::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureCreditNote))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto const& tx = ctx.tx;

    if (tx.getFlags() & tfUniversalMask)
    {
        JLOG(ctx.j.warn()) << "NoteClaim: invalid flags set.";
        return temINVALID_FLAG;
    }

    // Everything below is a pure function of the transaction, so a failure
    // is malformed rather than a claimed fee: the bearer could have checked
    // it before submitting.

    auto const raw = tx.getFieldVL(sfNote);

    CreditNote n;
    try
    {
        n = parseCreditNote(text(raw));
    }
    catch (std::exception const& e)
    {
        JLOG(ctx.j.warn()) << "NoteClaim: " << e.what();
        return temMALFORMED;
    }

    // A note binds itself to one network. The transaction's own NetworkID
    // rules (preflight1) are not enough: on a legacy network a transaction
    // carries none, and the note must still not cross over from another.
    if (n.networkID != ctx.app.config().NETWORK_ID)
    {
        JLOG(ctx.j.warn()) << "NoteClaim: note is for network " << n.networkID;
        return temMALFORMED;
    }

    // Redeeming your own note moves nothing and would still retire every
    // earlier note you signed.
    if (n.account == tx[sfAccount])
        return temREDUNDANT;

    // Notes are ed25519, like JsonTx itself: it is what WebCrypto can sign.
    auto const pk = tx.getFieldVL(sfPublicKey);
    if (publicKeyType(makeSlice(pk)) != KeyType::ed25519)
    {
        JLOG(ctx.j.warn()) << "NoteClaim: PublicKey must be ed25519.";
        return temMALFORMED;
    }

    // The signature is over the note's bytes exactly as carried, under the
    // JsonTx prefix: the text the issuer read is the text being verified.
    if (!verify(
            PublicKey(makeSlice(pk)),
            makeSlice(jsontx_signing_data(text(raw))),
            makeSlice(tx.getFieldVL(sfNoteSignature))))
    {
        JLOG(ctx.j.warn()) << "NoteClaim: note signature does not verify.";
        return temBAD_SIGNATURE;
    }

    return preflight2(ctx);
}

TER
NoteClaim::checkFunds(
    ReadView const& view,
    CreditNote const& n,
    AccountID const& bearer,
    beast::Journal j)
{
    if (n.amount.native())
    {
        // The issuer pays from what is spendable above their reserve.
        if (xrpLiquid(view, n.account, 0, j) < n.amount)
        {
            JLOG(j.trace()) << "NoteClaim: issuer cannot cover the note.";
            return tecUNFUNDED_PAYMENT;
        }
        return tesSUCCESS;
    }

    Issue const& issue = n.amount.issue();
    AccountID const& gateway = issue.account;

    auto const sleGateway = view.read(keylet::account(gateway));
    if (!sleGateway)
        return tecNO_ISSUER;

    // The bearer receives exactly Amount. Unless one side is the gateway the
    // transfer transits it, and the issuer bears the gateway's transfer fee
    // on top - the same deal as a plain Payment of Amount with no SendMax.
    STAmount cost = n.amount;
    if (n.account != gateway && bearer != gateway)
        cost = multiply(n.amount, transferRate(view, gateway));

    if (accountFunds(view, n.account, cost, fhZERO_IF_FROZEN, j) < cost)
    {
        JLOG(j.trace()) << "NoteClaim: issuer cannot cover the note.";
        return tecUNFUNDED_PAYMENT;
    }

    // A gateway can always take back its own currency. Anyone else needs a
    // trust line already in place: a note never creates one, nor an account.
    if (bearer != gateway)
    {
        auto const sleLine =
            view.read(keylet::line(bearer, gateway, issue.currency));
        if (!sleLine)
        {
            JLOG(j.trace()) << "NoteClaim: bearer has no trust line.";
            return tecNO_LINE;
        }

        if (sleGateway->getFlags() & lsfRequireAuth)
        {
            // The gateway's authorization flag sits on the gateway's side of
            // the line, which is the low side iff the bearer sorts higher.
            bool const gatewayIsLow = bearer > gateway;
            if (!(sleLine->getFlags() &
                  (gatewayIsLow ? lsfLowAuth : lsfHighAuth)))
                return tecNO_AUTH;
        }

        if (isFrozen(view, bearer, issue.currency, gateway))
            return tecFROZEN;
    }

    // The trust line's limit is deliberately not consulted, as when cashing
    // a Check: the bearer is the one submitting, so they want the funds.

    return tesSUCCESS;
}

TER
NoteClaim::preclaim(PreclaimContext const& ctx)
{
    auto const n = note(ctx.tx);

    auto const sleIssuer = ctx.view.read(keylet::account(n.account));
    if (!sleIssuer)
    {
        JLOG(ctx.j.warn()) << "NoteClaim: issuer does not exist.";
        return tecNO_TARGET;
    }

    // The key that signed the note must be the issuer's master key (unless
    // disabled) or their regular key, exactly as for a single-signed
    // transaction from that account. Signer lists are not honoured: a note
    // is one signature over one text.
    {
        auto const signer =
            calcAccountID(PublicKey(makeSlice(ctx.tx.getFieldVL(sfPublicKey))));
        bool const master =
            signer == n.account && !(sleIssuer->getFlags() & lsfDisableMaster);
        bool const regular = sleIssuer->isFieldPresent(sfRegularKey) &&
            signer == sleIssuer->getAccountID(sfRegularKey);
        if (!master && !regular)
        {
            JLOG(ctx.j.warn())
                << "NoteClaim: PublicKey is not authorised for the issuer.";
            return tecNO_PERMISSION;
        }
    }

    // The validity window, against the same clock sfTime uses: the parent
    // ledger's close time in milliseconds since the ripple epoch.
    std::uint64_t const close =
        static_cast<std::uint64_t>(
            ctx.view.parentCloseTime().time_since_epoch().count()) *
        1000;

    // Not valid yet. Retriable, as for a future Sequence: the issuer's clock
    // a little ahead of the network's is the common case.
    if (close < n.issueTime)
        return terPRE_SEQ;

    if (close >= n.expiration)
    {
        JLOG(ctx.j.warn()) << "NoteClaim: note has expired.";
        return tecEXPIRED;
    }

    // Replay. Strictly greater, so a note redeems at most once and no two
    // notes share an IssueTime. Not a claimed fee: the losing side of a race
    // for the same note did nothing wrong, as for tefPAST_SEQ on a Sequence.
    if (sleIssuer->isFieldPresent(sfLastNoteTime) &&
        n.issueTime <= sleIssuer->getFieldU64(sfLastNoteTime))
        return tefPAST_SEQ;

    return checkFunds(ctx.view, n, ctx.tx[sfAccount], ctx.j);
}

TER
NoteClaim::doApply()
{
    auto const n = note(ctx_.tx);
    auto& view = ctx_.view();

    {
        // Hooks ran between preclaim and here; nothing they can do moves
        // sfLastNoteTime, but the check is cheap and the invariant matters.
        auto const sleIssuer = view.read(keylet::account(n.account));
        if (!sleIssuer)
            return tefINTERNAL;
        if (sleIssuer->isFieldPresent(sfLastNoteTime) &&
            n.issueTime <= sleIssuer->getFieldU64(sfLastNoteTime))
            return tefPAST_SEQ;
    }

    if (auto const ter = checkFunds(view, n, account_, j_); !isTesSuccess(ter))
        return ter;

    // XRP to XRP is a balance adjustment; an issued currency goes through
    // accountSend, which handles direct issue, redemption and transit (with
    // the issuer paying the transfer fee) without touching paths or offers.
    TER const ter = n.amount.native()
        ? transferXRP(view, n.account, account_, n.amount, j_)
        : accountSend(view, n.account, account_, n.amount, j_);
    if (!isTesSuccess(ter))
        return ter;

    auto sleIssuer = view.peek(keylet::account(n.account));
    if (!sleIssuer)
        return tefINTERNAL;
    sleIssuer->setFieldU64(sfLastNoteTime, n.issueTime);
    view.update(sleIssuer);

    ctx_.deliver(n.amount);
    return tesSUCCESS;
}

}  // namespace ripple
