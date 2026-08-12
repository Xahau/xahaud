//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
*/
//==============================================================================

#ifndef RIPPLE_TX_VALIDATOR_IDENTITY_H_INCLUDED
#define RIPPLE_TX_VALIDATOR_IDENTITY_H_INCLUDED

#include <xrpld/app/misc/DomainTrustAnchor.h>
#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>

namespace ripple {

//------------------------------------------------------------------------------
// Deterministic domain-proof helpers.
//
// ValidatorDomainSet::preclaim always uses Application::domainTrustAnchor(),
// which is fixed at Application construction (ISRG Root X1 in production;
// optional private test root for jtx Env). There is no runtime-selectable
// production root.
//
// Proof signatures use sfDomainSignature and sfValidatorMasterSignature,
// both covered by the carrier's outer TxnSignature.
//------------------------------------------------------------------------------

/** Production trust anchor: repository-owned, CMake-embedded ISRG Root X1. */
DomainTrustAnchor
productionDomainTrustAnchor();

/** Canonical domain-separated statement signed by leaf key and master. */
Serializer
domainBindingStatement(STTx const& tx, std::uint32_t networkID);

/** Verify the self-contained domain proof against an explicit trust anchor. */
bool
verifyDomainProof(
    STTx const& tx,
    std::uint32_t networkID,
    NetClock::time_point parentCloseTime,
    DomainTrustAnchor const& trustAnchor);

/** True when the stored binding is the validator's active graph edge.
 *
 * Requires the claimed [NotBefore, NotAfter) window to contain ledgerTime,
 * the named validator to exist and not be revoked, and its backlink to name
 * this domain object. Inactive objects remain as CAS predecessor state.
 */
bool
isDomainBindingActive(
    ReadView const& view,
    SLE const& domainObject,
    NetClock::time_point ledgerTime);

/** CAS + dual-link domain state transition used by doApply. */
TER
applyValidatorDomainBinding(
    ApplyView& view,
    STTx const& tx,
    std::uint32_t networkID);

class ValidatorManifestSet : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit ValidatorManifestSet(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    TER
    doApply() override;
};

class ValidatorDomainSet : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};

    explicit ValidatorDomainSet(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static NotTEC
    preflight(PreflightContext const& ctx);

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

}  // namespace ripple

#endif
