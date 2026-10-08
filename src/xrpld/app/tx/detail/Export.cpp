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

#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/core/Config.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/st.h>

namespace ripple {

static STObject const&
obj(STObject const& o, SField const& f)
{
    return o.peekAtField(f).downcast<STObject>();
}

// The exported transaction, if it parses under this network's formats and is
// shaped for multisigning by the UNL: unsigned, empty SigningPubKey, not
// emitted, not a pseudo-txn and not replayable here.
static std::optional<STTx>
exportedTx(STTx const& tx, std::uint32_t networkID)
{
    try
    {
        Serializer s;
        obj(tx, sfExportedTxn).add(s);
        STTx const t{SerialIter{s.slice()}};
        if (t[sfAccount] == tx[sfAccount] && t.getSigningPubKey().empty() &&
            !t.isFieldPresent(sfTxnSignature) && !t.isFieldPresent(sfSigners) &&
            !t.isFieldPresent(sfEmitDetails) && !isPseudoTx(t) &&
            t[~sfNetworkID] != networkID)
            return t;
    }
    catch (std::exception const&)
    {
    }
    return std::nullopt;
}

static Keylet
exportKeylet(STTx const& tx)
{
    return keylet::exportedTxn(tx[sfLedgerSequence], tx[sfTransactionHash]);
}

XRPAmount
Export::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    if (tx.getTxnType() != ttEXPORT)
        return XRPAmount{0};

    // an export fans out into one signature per active validator plus a final
    auto const unl = view.read(keylet::UNLReport());
    std::int64_t const n = unl && unl->isFieldPresent(sfActiveValidators)
        ? unl->getFieldArray(sfActiveValidators).size()
        : 0;
    return Transactor::calculateBaseFee(view, tx) + view.fees().base * (n + 1);
}

NotTEC
Export::preflight(PreflightContext const& ctx)
{
    auto const& tx = ctx.tx;
    if (!ctx.rules.enabled(featureExport) ||
        !ctx.rules.enabled(featureOnChainManifests))
        return temDISABLED;

    if (tx.getTxnType() == ttEXPORT)
    {
        if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
            return ret;
        if (!hook::isEmittedTxn(tx) ||
            !exportedTx(tx, ctx.app.config().NETWORK_ID))
            return temMALFORMED;
        return preflight2(ctx);
    }

    if (auto const ret = preflight0(ctx); !isTesSuccess(ret))
        return ret;

    // network generated: account zero, free, unsigned, unsequenced
    if (tx[sfAccount] != beast::zero || tx[sfFee] != beast::zero ||
        tx[sfSequence] != 0 || !tx.getSigningPubKey().empty() ||
        tx.isFieldPresent(sfTxnSignature) || tx.isFieldPresent(sfSigners) ||
        tx.isFieldPresent(sfTicketSequence) ||
        tx.isFieldPresent(sfPreviousTxnID))
        return temMALFORMED;

    if (tx.getTxnType() == ttEXPORT_SIGN)
    {
        auto const& s = obj(tx, sfSigner);
        auto const pk = s[sfSigningPubKey];
        if (!publicKeyType(pk) || s[sfAccount] != calcAccountID(PublicKey(pk)))
            return temMALFORMED;
    }

    return tesSUCCESS;
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    auto const& tx = ctx.tx;
    if (tx.getTxnType() == ttEXPORT)
        return tesSUCCESS;

    auto const sle = ctx.view.read(exportKeylet(tx));
    if (!sle)
        return tefFAILURE;

    if (tx.getTxnType() == ttEXPORT_FINAL)
        return tesSUCCESS;

    // The signer must be the current ephemeral key of an active UNL validator
    // according to the ledger (OnChainManifests), never the local manifest
    // cache, so every node reaches the same verdict.
    auto const& s = obj(tx, sfSigner);
    PublicKey const pk(s[sfSigningPubKey]);
    auto const man = ctx.view.read(keylet::manifest(pk));
    auto const unl = ctx.view.read(keylet::UNLReport());
    if (!man || (*man)[~sfSigningPubKey] != pk.slice() || !unl ||
        !unl->isFieldPresent(sfActiveValidators))
        return tefBAD_AUTH;

    auto const& avs = unl->getFieldArray(sfActiveValidators);
    if (std::none_of(avs.begin(), avs.end(), [&](STObject const& v) {
            return v[sfPublicKey] == (*man)[sfPublicKey];
        }))
        return tefBAD_AUTH;

    auto const& inner = obj(*sle, sfExportedTxn);
    if (inner.isFieldPresent(sfSigners))
    {
        auto const& signers = inner.getFieldArray(sfSigners);
        if (std::any_of(signers.begin(), signers.end(), [&](STObject const& o) {
                return o[sfAccount] == s[sfAccount];
            }))
            return tefALREADY;
        if (signers.size() >= STTx::maxMultiSigners())
            return tefTOO_BIG;
    }

    // XRPL requires fully canonical signatures, which also stops malleated
    // copies of one signature from becoming distinct transactions.
    if (!verify(
            pk,
            buildMultiSigningData(inner, s[sfAccount]).slice(),
            s[sfTxnSignature],
            true))
        return tefBAD_SIGNATURE;

    return tesSUCCESS;
}

TER
Export::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;

    if (tx.getTxnType() == ttEXPORT)
    {
        auto const t = exportedTx(tx, ctx_.app.config().NETWORK_ID);
        if (!t)
            return tefINTERNAL;

        auto const id = t->getTransactionID();
        auto const k = keylet::exportedTxn(view.seq(), id);
        if (view.exists(k))
            return tecDUPLICATE;

        auto const sle = std::make_shared<SLE>(k);
        sle->peekFieldObject(sfExportedTxn) = obj(tx, sfExportedTxn);
        sle->setFieldH256(sfTransactionHash, id);
        sle->setFieldU32(sfLedgerSequence, view.seq());
        view.insert(sle);

        if (!t->isFieldPresent(sfTicketSequence) ||
            (*t)[~sfOperationLimit] != ctx_.app.config().NETWORK_ID)
            return tesSUCCESS;

        // a ticketed txn bound back here may return via Import, once
        return hook::setHookState(
            ctx_,
            account_,
            shadowTicketNS,
            uint256(t->getFieldU32(sfTicketSequence)),
            Slice{id.data(), id.size()});
    }

    auto const sle = view.peek(exportKeylet(tx));
    if (!sle)
        return tefINTERNAL;

    if (tx.getTxnType() == ttEXPORT_FINAL)
    {
        view.erase(sle);
        return tesSUCCESS;
    }

    // XRPL requires Signers sorted by account
    auto& inner = sle->peekFieldObject(sfExportedTxn);
    STArray signers = inner.isFieldPresent(sfSigners)
        ? inner.getFieldArray(sfSigners)
        : STArray(sfSigners);
    signers.push_back(obj(tx, sfSigner));
    signers.sort([](STObject const& a, STObject const& b) {
        return a[sfAccount] < b[sfAccount];
    });
    inner.setFieldArray(sfSigners, signers);
    view.update(sle);
    return tesSUCCESS;
}

void
Export::preCompute()
{
    XRPL_ASSERT(
        (account_ == beast::zero) == (ctx_.tx.getTxnType() != ttEXPORT),
        "ripple::Export::preCompute : account matches txn type");
}

bool
Export::accept(Application& app, OpenView& view, beast::Journal j)
{
    if (!view.rules().enabled(featureExport))
        return false;

    auto const seq = view.seq();
    auto const nid = app.config().NETWORK_ID;
    auto const& keys = app.getValidatorKeys().keys;
    bool changed = false;

    auto const inject = [&](TxType type, auto&& assemble) {
        changed |=
            ripple::apply(app, view, STTx(type, assemble), tapNONE, j).applied;
    };

    // Only exports from closed ledgers: their keys sort below this ledger's.
    // Applying (rather than raw-inserting) lets preclaim reject duplicates.
    auto const end = keylet::exportedTxn(seq, beast::zero).key;
    for (auto k = keylet::exportedTxn(0, beast::zero).key;
         auto const next = view.succ(k, end);
         k = *next)
    {
        auto const sle = view.read(Keylet{ltEXPORTED_TXN, *next});
        if (!sle)
            continue;

        auto const created = (*sle)[sfLedgerSequence];
        auto const id = (*sle)[sfTransactionHash];
        auto const& inner = obj(*sle, sfExportedTxn);

        if (seq >= created + window)
            inject(ttEXPORT_FINAL, [&](STObject& o) {
                o[sfAccount] = AccountID();
                o[sfOwner] = inner[sfAccount];
                o[sfLedgerSequence] = created;
                o[sfTransactionHash] = id;
            });
        else if (keys)
            inject(ttEXPORT_SIGN, [&](STObject& o) {
                auto const acc = calcAccountID(keys->publicKey);
                o[sfAccount] = AccountID();
                if (nid > 1024)
                    o[sfNetworkID] = nid;
                o[sfLedgerSequence] = created;
                o[sfTransactionHash] = id;
                auto& s = o.peekFieldObject(sfSigner);
                s[sfAccount] = acc;
                s[sfSigningPubKey] = keys->publicKey.slice();
                s[sfTxnSignature] = sign(
                    keys->publicKey,
                    keys->secretKey,
                    buildMultiSigningData(inner, acc).slice());
            });
    }

    return changed;
}

}  // namespace ripple
