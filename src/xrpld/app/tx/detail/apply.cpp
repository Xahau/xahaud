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

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/applySteps.h>
#include <xrpld/app/tx/detail/SetManifest.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>

namespace ripple {

// These are the same flags defined as SF_PRIVATE1-4 in HashRouter.h
#define SF_SIGBAD SF_PRIVATE1     // Signature is bad
#define SF_SIGGOOD SF_PRIVATE2    // Signature is good
#define SF_LOCALBAD SF_PRIVATE3   // Local checks failed
#define SF_LOCALGOOD SF_PRIVATE4  // Local checks passed

//------------------------------------------------------------------------------

std::pair<Validity, std::string>
checkValidity(
    HashRouter& router,
    STTx const& tx,
    Rules const& rules,
    Config const& config,
    ApplyFlags applyFlags)
{
    auto const id = tx.getTransactionID();
    auto const flags = router.getFlags(id);

    if (rules.enabled(featureHooks) && tx.isFieldPresent(sfEmitDetails))
    {
        // emitted transactions do not contain signatures
        if (tx.isFieldPresent(sfTxnSignature))
            return {Validity::SigBad, "Emitted txn contains signature."};

        // and they must be either emitted here on this node
        // or in process of being preflighted during emission
        if (applyFlags & tapPREFLIGHT_EMIT)
        {
            // pass, this is a txn being preflighted emit api
        }
        else if (flags & SF_EMITTED)
        {
            // pass, this txn came out of the emission directory
        }
        else
            return {
                Validity::SigBad,
                "Emitted txn was not marked for preflight nor out of the "
                "emission directory"};

        std::string reason;
        if (!passesLocalChecks(tx, reason))
            return {Validity::SigGoodOnly, reason};

        return {Validity::Valid, ""};
    }

    bool const manifestAuthorized = rules.enabled(featureOnChainManifests) &&
        hasManifestAuthorityMarkers(tx);
    if (manifestAuthorized)
    {
        // This path runs at RPC/overlay ingress, before transactor preflight.
        // The DoS ordering is deliberate: structural nonsense must not buy
        // either manifest-signature checks or attached multisign work.
        if (!hasCanonicalUnsignedSetManifestShape(tx, config.NETWORK_ID))
            return {
                Validity::SigBad,
                "Manifest-authorized envelope is not canonical"};

        // Fee is the last otherwise-malleable outer field, but its canonical
        // value depends on ledger fee state, which this function does not
        // have. Ingress callers pin it first with checkManifestIngressFee();
        // the transactor pins it again against the applying view in checkFee.
    }

    if (flags & SF_SIGBAD)
        // Signature is known bad
        return {Validity::SigBad, "Transaction has bad signature."};

    if (!(flags & SF_SIGGOOD))
    {
        if (manifestAuthorized)
        {
            // The canonical txid makes the ordinary HashRouter signature
            // receipt safe to reuse when PeerImp hands the same transaction
            // to NetworkOPs.
            STObject const& manObj = const_cast<ripple::STTx&>(tx)
                                         .getField(sfManifest)
                                         .downcast<STObject>();
            auto man = deserializeManifest(manObj);
            if (!man || !man->verify())
            {
                router.setFlags(id, SF_SIGBAD);
                return {Validity::SigBad, "Manifest signature is bad"};
            }
        }
        else
        {
            // Don't know signature state. Check it.
            auto const requireCanonicalSig =
                rules.enabled(featureRequireFullyCanonicalSig)
                ? STTx::RequireFullyCanonicalSig::yes
                : STTx::RequireFullyCanonicalSig::no;

            auto const sigVerify = tx.checkSign(requireCanonicalSig, rules);
            if (!sigVerify)
            {
                router.setFlags(id, SF_SIGBAD);
                return {Validity::SigBad, sigVerify.error()};
            }
        }
        router.setFlags(id, SF_SIGGOOD);
    }

    // Signature is now known good
    if (flags & SF_LOCALBAD)
        // ...but the local checks
        // are known bad.
        return {Validity::SigGoodOnly, "Local checks failed."};

    if (flags & SF_LOCALGOOD)
        // ...and the local checks
        // are known good.
        return {Validity::Valid, ""};

    // Do the local checks
    std::string reason;
    if (!passesLocalChecks(tx, reason))
    {
        router.setFlags(id, SF_LOCALBAD);
        return {Validity::SigGoodOnly, reason};
    }
    router.setFlags(id, SF_LOCALGOOD);
    return {Validity::Valid, ""};
}

void
forceValidity(HashRouter& router, uint256 const& txid, Validity validity)
{
    int flags = 0;
    switch (validity)
    {
        case Validity::Valid:
            flags |= SF_LOCALGOOD;
            [[fallthrough]];
        case Validity::SigGoodOnly:
            flags |= SF_SIGGOOD;
            [[fallthrough]];
        case Validity::SigBad:
            // would be silly to call directly
            break;
    }
    if (flags)
        router.setFlags(txid, flags);
}

std::pair<ManifestIngressFee, std::string>
checkManifestIngressFee(Application& app, STTx const& tx)
{
    auto const open = app.openLedger().current();
    if (!open->rules().enabled(featureOnChainManifests) ||
        !hasManifestAuthorityMarkers(tx))
        return {ManifestIngressFee::NotApplicable, {}};

    // A non-canonical shape can never become valid, so it is not a fee miss:
    // leave it to checkValidity(), which rejects it as a bad envelope.
    if (!hasCanonicalUnsignedSetManifestShape(tx, app.config().NETWORK_ID))
        return {ManifestIngressFee::NotApplicable, {}};

    // An amendment-blocked node's open ledger may sit on history the network
    // is not validating, and with no validated ledger there is no agreed base
    // at all. Neither is a reason to spend manifest crypto finding out.
    if (app.getOPs().isAmendmentBlocked())
        return {ManifestIngressFee::Refused, "server is amendment blocked"};
    auto const validated = app.getLedgerMaster().getValidatedLedger();
    if (!validated)
        return {ManifestIngressFee::Refused, "no validated ledger"};

    try
    {
        auto const fee = tx[sfFee].xrp();
        auto const& manifest =
            const_cast<STTx&>(tx).getField(sfManifest).downcast<STObject>();

        // Exactly two snapshots: no intermediate fee setting between them and
        // no remembered earlier ones, so a relayer cannot widen the set of
        // txids that reach manifest signature checks.
        for (auto const base : {validated->fees().base, open->fees().base})
        {
            if (auto const canonical =
                    canonicalUnsignedSetManifestFee(base, manifest);
                canonical && *canonical == fee)
                return {ManifestIngressFee::Canonical, {}};
        }
    }
    catch (std::exception const&)
    {
        // Malformed Fee or manifest: leave it to checkValidity()'s shape
        // check, which rejects it as a bad envelope.
        return {ManifestIngressFee::NotApplicable, {}};
    }

    return {
        ManifestIngressFee::Refused,
        "Fee is not the canonical SetManifest fee for this server's "
        "validated or open ledger"};
}

ApplyResult
apply(
    Application& app,
    OpenView& view,
    STTx const& tx,
    ApplyFlags flags,
    beast::Journal j)
{
    STAmountSO stAmountSO{view.rules().enabled(fixSTAmountCanonicalize)};
    NumberSO stNumberSO{view.rules().enabled(fixUniversalNumber)};

    auto pfresult = preflight(app, view.rules(), tx, flags, j);
    auto pcresult = preclaim(pfresult, app, view);
    return doApply(pcresult, app, view);
}

ApplyTransactionResult
applyTransaction(
    Application& app,
    OpenView& view,
    STTx const& txn,
    bool retryAssured,
    ApplyFlags flags,
    beast::Journal j)
{
    // Returns false if the transaction has need not be retried.
    if (retryAssured)
        flags = flags | tapRETRY;

    JLOG(j.debug()) << "TXN " << txn.getTransactionID()
                    << (retryAssured ? "/retry" : "/final");

    try
    {
        auto const result = apply(app, view, txn, flags, j);
        if (result.applied)
        {
            JLOG(j.debug())
                << "Transaction applied: " << transHuman(result.ter);
            return ApplyTransactionResult::Success;
        }

        if (isTefFailure(result.ter) || isTemMalformed(result.ter) ||
            isTelLocal(result.ter))
        {
            // failure
            JLOG(j.debug())
                << "Transaction failure: " << transHuman(result.ter);
            return ApplyTransactionResult::Fail;
        }

        JLOG(j.debug()) << "Transaction retry: " << transHuman(result.ter);
        return ApplyTransactionResult::Retry;
    }
    catch (std::exception const& ex)
    {
        JLOG(j.warn()) << "Throws: " << ex.what();
        return ApplyTransactionResult::Fail;
    }
}

}  // namespace ripple
