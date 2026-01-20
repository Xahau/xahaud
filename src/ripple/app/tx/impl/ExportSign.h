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

#ifndef RIPPLE_TX_EXPORTSIGN_H_INCLUDED
#define RIPPLE_TX_EXPORTSIGN_H_INCLUDED

#include <ripple/app/misc/Manifest.h>
#include <ripple/app/tx/impl/Transactor.h>
#include <ripple/basics/Log.h>
#include <ripple/core/Config.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Indexes.h>

namespace ripple {

class OpenView;

//@@start exportsign-class
/**
 * ExportSign is a UVTxn (UNL Validator Transaction) that allows validators
 * on the UNLReport to submit their signatures for exported cross-chain
 * transactions without requiring a funded account.
 */
class ExportSign : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Custom};

    explicit ExportSign(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx)
    {
        return XRPAmount{0};
    }

    static TxConsequences
    makeTxConsequences(PreflightContext const& ctx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;
};

/**
 * If this validator is on the UNLReport, generate signed ttEXPORT_SIGN
 * transactions for any exported transactions that need signing.
 */
std::vector<std::shared_ptr<STTx const>>
makeExportSignTxns(OpenView& view, Application& app, beast::Journal const& j);
//@@end exportsign-class

}  // namespace ripple

#endif
