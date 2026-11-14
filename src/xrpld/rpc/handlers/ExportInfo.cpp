//------------------------------------------------------------------------------
/*
    This file is part of xahaud: https://github.com/Xahau/xahaud
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
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/Import.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/jss.h>
#include <set>

namespace ripple {

// an export's transaction, in the few words that identify it
static void
exportInfoTxn(Json::Value& o, STObject const& inner)
{
    if (auto const f = TxFormats::getInstance().findByType(
            static_cast<TxType>(inner.getFieldU16(sfTransactionType))))
        o["transaction_type"] = f->getName();
    if (auto const t = inner[~sfTicketSequence])
        o["ticket"] = *t;
    else
        o["sequence"] = inner[~sfSequence].value_or(0);
    if (auto const l = inner[~sfLastLedgerSequence])
        o["last_ledger_sequence"] = *l;
    if (inner.isFieldPresent(sfFee) && inner.getFieldAmount(sfFee).native())
        o["fee"] = std::to_string(inner.getFieldAmount(sfFee).xrp().drops());
    o["signatures"] = inner.isFieldPresent(sfSigners)
        ? static_cast<Json::UInt>(inner.getFieldArray(sfSigners).size())
        : 0u;
}

// One ltEXPORTER: an account's standing on one network. With `detail`, its
// signers and every export it has in flight as well.
static Json::Value
exportInfoRecord(ReadView const& view, SLE const& ex, bool detail)
{
    Json::Value o{Json::objectValue};
    std::uint32_t const nid = ex[sfRemoteNetworkID];
    o[jss::account] = toBase58(ex.getAccountID(sfAccount));
    o[jss::network_id] = nid;
    o[jss::index] = to_string(ex.key());
    // listed: the ledger acts for it there (tickets, keys, expiry)
    o["listed"] = isExporter(ex);

    auto const pool = ExportTickets::load(ex);
    auto& tickets = o["tickets"] = Json::objectValue;
    tickets["count"] = pool.count();
    auto& ranges = tickets["ranges"] = Json::arrayValue;
    for (auto const& r : pool.ranges())
    {
        auto& e = ranges.append(Json::objectValue);
        e["first"] = r.first;
        e["count"] = r.count;
    }
    if (auto const v = ex[~sfExportTicketNext])
        tickets["next"] = *v;
    if (auto const v = ex[~sfExportTicketPending])
        tickets["refill_pending"] = *v;

    if (auto const v = ex[~sfExportSequence])
        o["sequence"] = *v;
    if (ex.isFieldPresent(sfBalance))
    {
        o["balance"] =
            std::to_string(ex.getFieldAmount(sfBalance).xrp().drops());
        o["balance_ledger_index"] = ex[~sfLedgerSequence].value_or(0);
    }

    if (ex.isFieldPresent(sfExportSignerList))
    {
        auto const& l = ex.peekAtField(sfExportSignerList).downcast<STObject>();
        auto const& entries = l.getFieldArray(sfSignerEntries);
        auto& sl = o["signer_list"] = Json::objectValue;
        sl["quorum"] = l[sfSignerQuorum];
        sl["signers"] = static_cast<Json::UInt>(entries.size());
        // one of ours: it names an export key of the UNL, so the ledger
        // can sign for it
        sl["ours"] = holdsOurList(view, ex);
        sl[jss::ledger_index] = l[sfLedgerSequence];
        if (detail)
        {
            auto& accounts = sl["accounts"] = Json::arrayValue;
            for (auto const& e : entries)
                accounts.append(toBase58(e.getAccountID(sfAccount)));
        }
    }
    if (auto const v = ex[~sfExportKeysSeq])
        o["keys_seq"] = *v;

    // exports in flight: made, and neither back nor closed as failed
    Json::UInt inFlight = 0;
    Json::Value exports{Json::arrayValue};
    if (ex.isFieldPresent(sfIndexes))
        for (auto const& key : ex.getFieldV256(sfIndexes))
        {
            auto const st = view.read(Keylet{ltSHADOW_TICKET, key});
            if (!st)
                continue;
            ++inFlight;
            if (!detail)
                continue;
            auto& e = exports.append(Json::objectValue);
            e[jss::index] = to_string(key);
            e["ticket"] = (*st)[sfTicketSequence];
            // the ttEXPORT that made it: what xport() gave the hook and
            // what its cbak sees as otxn_id
            if (auto const v = (*st)[~sfExportTxnID])
                e["export_txn_id"] = to_string(*v);
            // its id on the other network without signatures, and as
            // signed once its signing window has closed at quorum
            e["txn_hash"] = to_string((*st)[sfTransactionHash]);
            if (auto const v = (*st)[~sfRemoteTxnID])
                e["remote_txn_id"] = to_string(*v);
            if (auto const v = (*st)[~sfLastLedgerSequence])
                e["last_ledger_sequence"] = *v;
            if (auto const v = (*st)[~sfEmitHookHash])
                e["hook_hash"] = to_string(*v);
            // signed: at quorum and sent; otherwise still gathering
            // signatures (see `pending`)
            e["signed"] = st->isFieldPresent(sfExportedTxn);
            if (st->isFieldPresent(sfExportedTxn))
                exportInfoTxn(
                    e, st->peekAtField(sfExportedTxn).downcast<STObject>());
        }
    o["in_flight"] = inFlight;
    if (detail)
        o["exports"] = std::move(exports);
    return o;
}

/** export_info: the state of featureExport (see Export.h) in a ledger. Takes
    the usual ledger selectors, plus optional:

        account    only this account's records, one per network, each with
                   its signers and every export it has in flight
        limit      exporters per call (default 50, at most 400)
        marker     where the last call's exporters left off

    Without `account`: enabled, network_id, default_network_id
    (Import::soleNetwork), and
        keys        the UNL's export keys: export_keys_seq, the rotation
                    epoch and when it ends, how many UNLReport validators
                    hold a key and had one recorded this epoch, and the
                    signer list a new exporter would get
        validators  each one's account, current key and when it was recorded
        networks    per trusted network: VL keys, and what ttINGEST proved
        pending     exports in their signing window, with their signatures
        exporters   a page of the listed exporters (exporters_listed in all):
                    tickets as ranges, Sequence, balance, signer list and
                    whether it is ours, exports in flight
*/
Json::Value
doExportInfo(RPC::JsonContext& context)
{
    auto const& params = context.params;

    std::optional<AccountID> account;
    if (params.isMember(jss::account) &&
        (!params[jss::account].isString() ||
         !(account = parseBase58<AccountID>(params[jss::account].asString()))))
        return RPC::invalid_field_error(jss::account);

    Json::UInt limit = 50;
    if (params.isMember(jss::limit))
    {
        auto const& l = params[jss::limit];
        if (!(l.isUInt() || (l.isInt() && l.asInt() > 0)) || l.asUInt() == 0)
            return RPC::invalid_field_error(jss::limit);
        limit = std::min<Json::UInt>(l.asUInt(), 400);
    }
    std::optional<uint256> marker;
    if (params.isMember(jss::marker))
    {
        uint256 m;
        if (!params[jss::marker].isString() ||
            !m.parseHex(params[jss::marker].asString()))
            return RPC::invalid_field_error(jss::marker);
        marker = m;
    }

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);
    if (!ledger)
        return result;
    auto const& view = *ledger;
    auto const& config = context.app.config();

    result["enabled"] = view.rules().enabled(featureExport);
    result[jss::network_id] = config.NETWORK_ID;
    result["default_network_id"] = Import::soleNetwork(config, view);

    if (account)
    {
        auto& records = result["records"] = Json::arrayValue;
        for (auto const& ex : exportersOf(view, *account))
            records.append(exportInfoRecord(view, *ex, true));
        return result;
    }

    // the UNL's export keys and how they are turning over
    auto const unl = view.read(keylet::UNLReport());
    auto const now = static_cast<std::uint32_t>(
        view.parentCloseTime().time_since_epoch().count());
    auto const epoch = Export::rotationEpoch(now);
    {
        Json::UInt total = 0, withKey = 0, thisEpoch = 0;
        auto& validators = result["validators"] = Json::arrayValue;
        if (unl && unl->isFieldPresent(sfActiveValidators))
            for (auto const& v : unl->getFieldArray(sfActiveValidators))
            {
                if (!publicKeyType(v[sfPublicKey]))
                    continue;
                PublicKey const master(v[sfPublicKey]);
                auto const id = calcAccountID(master);
                auto const acc = view.read(keylet::account(id));
                auto& o = validators.append(Json::objectValue);
                o["validator"] = toBase58(TokenType::NodePublic, master);
                o[jss::account] = toBase58(id);
                ++total;
                // without an account its key reports have nowhere to go
                // (the next ttGENESIS_MINT makes it one)
                o["funded"] = static_cast<bool>(acc);
                if (!acc || !acc->isFieldPresent(sfExportKeys) ||
                    acc->getFieldArray(sfExportKeys).empty())
                {
                    o["keys"] = 0u;
                    continue;
                }
                auto const& keys = acc->getFieldArray(sfExportKeys);
                auto const& head = keys[0];
                auto const recorded = head.getFieldU32(sfCloseTime);
                PublicKey const key(makeSlice(head.getFieldVL(sfExportKey)));
                ++withKey;
                o["keys"] = static_cast<Json::UInt>(keys.size());
                o["export_key"] = strHex(key);
                o["export_account"] = toBase58(calcAccountID(key));
                o["recorded"] = recorded;
                bool const current = Export::rotationEpoch(recorded) >= epoch;
                o["this_epoch"] = current;
                thisEpoch += current;
            }

        auto const signers = exportSigners(view);
        auto& keys = result["keys"] = Json::objectValue;
        keys["export_keys_seq"] =
            unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0u;
        keys["rotation_period"] = Export::keyRotationPeriod;
        keys["epoch"] = epoch;
        keys["epoch_ends"] = (epoch + 1) * Export::keyRotationPeriod;
        keys["max_keys"] = static_cast<Json::UInt>(Export::maxExportKeys);
        keys["max_keys_per_epoch"] =
            static_cast<Json::UInt>(Export::maxKeysPerEpoch);
        keys["refresh_every"] = Export::keyRefreshEvery;
        keys["validators"] = total;
        keys["with_key"] = withKey;
        keys["rotated_this_epoch"] = thisEpoch;
        keys["signers"] = static_cast<Json::UInt>(signers.size());
        keys["quorum"] = Export::quorumOf(signers.size());
    }

    // the networks known here, and what has been proven of each
    {
        std::map<std::uint32_t, Json::UInt> nets{{0, 0}};
        for (auto const& [hex, pk] : config.IMPORT_VL_KEYS)
        {
            ++nets[config.importVLNetwork(hex)];
        }
        if (unl && unl->isFieldPresent(sfImportVLKeys))
            for (auto const& k : unl->getFieldArray(sfImportVLKeys))
                if (auto const hex = strHex(k[sfPublicKey]);
                    !config.IMPORT_VL_KEYS.count(hex))
                    ++nets[k[~sfRemoteNetworkID].value_or(0)];
        auto& networks = result["networks"] = Json::arrayValue;
        for (auto const& [nid, keys] : nets)
        {
            auto& o = networks.append(Json::objectValue);
            o[jss::network_id] = nid;
            o["vl_keys"] = keys;
            if (auto const fee = view.read(keylet::exportFee(nid)))
            {
                // the ledger exports expire by, and the fee they default to
                o[jss::ledger_index] = (*fee)[sfLedgerSequence];
                o["fee_unit"] = (*fee)[sfExportFeeUnit];
                o["sample_fee"] = (*fee)[sfSampleFee];
            }
        }
    }

    // exports in their signing window
    {
        auto& pending = result["pending"] = Json::arrayValue;
        auto const end = keylet::exportedTxn(view.seq() + 1, beast::zero).key;
        Json::UInt n = 0;
        for (auto k = keylet::exportedTxn(0, beast::zero).key;
             auto const next = view.succ(k, end);
             k = *next)
        {
            auto const sle = view.read(Keylet{ltEXPORTED_TXN, *next});
            if (!sle || ++n > 400)
                continue;
            auto& o = pending.append(Json::objectValue);
            o[jss::index] = to_string(*next);
            o["owner"] = toBase58(sle->getAccountID(sfOwner));
            o[jss::network_id] = (*sle)[sfRemoteNetworkID];
            o["created"] = (*sle)[sfLedgerSequence];
            o["closes"] = (*sle)[sfLedgerSequence] + Export::window;
            if (auto const v = (*sle)[~sfExportTxnID])
                o["export_txn_id"] = to_string(*v);
            o["txn_hash"] = to_string((*sle)[sfTransactionHash]);
            o["weight"] = exportWeight(*sle);
            o["quorum"] = (*sle)[sfSignerQuorum];
            exportInfoTxn(
                o, sle->peekAtField(sfExportedTxn).downcast<STObject>());
        }
    }

    // the directory of listed exporters, a page of it
    {
        auto& exporters = result["exporters"] = Json::arrayValue;
        Json::UInt listed = 0, given = 0;
        bool skipping = static_cast<bool>(marker);
        std::optional<uint256> last, more;
        forEachExporter(view, [&](uint256 const& key) {
            ++listed;
            if (skipping)
                skipping = key != *marker;
            else if (given == limit)
                more = last;
            else if (auto const ex = view.read(Keylet{ltEXPORTER, key}))
            {
                exporters.append(exportInfoRecord(view, *ex, false));
                ++given;
                last = key;
            }
        });
        result["exporters_listed"] = listed;
        if (more)
            result[jss::marker] = to_string(*more);
    }

    return result;
}

}  // namespace ripple
