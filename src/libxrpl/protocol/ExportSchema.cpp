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

#include <xrpl/protocol/ExportSchema.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>

namespace ripple {
namespace {

//------------------------------------------------------------------------------
//
// THE TABLE IS CONSENSUS. Every row below decides whether a ttEXPORT applies,
// so no row may be added, removed or changed except behind an amendment: a
// new row carries `since(fixExportSchemaX)`, a dropped one `until(...)`, and
// the amendment is voted on like any other. Without that, validators on the
// old and new binary disagree on the first export that differs.
//
// What belongs here: the transactions the other network has that this one
// has under the same type code, and on them the fields the other network
// lists that this network defines with the same name, type and code (a code
// the two networks assigned to different fields is out, so neither can
// misread the other's), which of those it requires, and the flags it
// accepts. bin/export-schema-check.py compares this table with a live
// rippled's `server_definitions` and prints what has drifted.
//
// And what the type pays there (ExtraFee, below). The other network's fee rules
// are in its code, not its `server_definitions`, so the checker cannot read
// them: it carries a list of the types and fields known to be priced
// specially there and says when a row, or a row it proposes, has one. A fee
// the ledger cannot fill in correctly is an export that is signed, sent and
// never included, so a field whose price it cannot express stays out of
// its row.
//
// Left out on purpose: AccountDelete and SetRegularKey, which would take the
// exporter's account there out from under the signer list this ledger keeps
// for it; and anything that is not a plain account's business.

struct Rule
{
    SField const* field = nullptr;
    bool required = false;
    uint256 const* since = nullptr;  // enabled with the type if null
    uint256 const* until = nullptr;

    constexpr Rule() = default;
    constexpr Rule(SField const& f) : field(&f)
    {
    }
};

constexpr Rule
req(SField const& f)
{
    Rule r{f};
    r.required = true;
    return r;
}

[[maybe_unused]] constexpr Rule
since(Rule r, uint256 const& amendment)
{
    r.since = &amendment;
    return r;
}

[[maybe_unused]] constexpr Rule
until(Rule r, uint256 const& amendment)
{
    r.until = &amendment;
    return r;
}

constexpr std::size_t maxRules = 12;

// What a type pays the other network on top of the ordinary fee, in that
// network's reference fees. The ordinary fee is one reference fee for the
// transaction and one more per signature (its Transactor::calculateBaseFee);
// this is what the type's own calculateBaseFee adds:
//   - `flat`, whatever the transaction holds; and
//   - if it carries `field`, a blob: `present`, plus one for every `per`
//     bytes of it (rounded down; none if `per` is 0).
//
// Part of the row, so consensus like the rest of it (the fee xport_prepare()
// fills in comes from here, and ttEXPORT holds a hook's own Fee to it): a
// change goes in a new row behind an amendment, the old one `until` it.
//
// What this cannot express has no row, or not the field that brings it:
//   - a fee that is not in reference fees: an owner reserve (AccountDelete,
//     AMMCreate and LedgerStateFix there), drops per byte (EscrowCreate's
//     Bytecode) or gas (EscrowFinish's Gas). The ledger knows the other
//     network's fee level (ltEXPORT_FEE) but not its reserve or gas price;
//   - a fee that depends on that network's state or on other signatures
//     (Batch, a sponsor's or counterparty's signers).
// Opening any of those needs this struct, and what ttINGEST proves, to grow
// first.
struct ExtraFee
{
    std::uint32_t flat = 0;
    SField const* field = nullptr;
    std::uint32_t present = 0;
    std::uint32_t per = 0;
};

struct Type
{
    TxType type;
    std::uint32_t flags;  // what the other network accepts, less tfUniversal
    std::array<Rule, maxRules> fields;
    ExtraFee fee = {};
    uint256 const* since = &featureExport;
    uint256 const* until = nullptr;
};

// The other network's flags this network has no name for
constexpr std::uint32_t tfSponsorCreatedAccountThere = 0x00080000;  // Payment
constexpr std::uint32_t tfHybridThere = 0x00100000;  // OfferCreate

// clang-format off
constexpr Type typeRows[] = {
    {ttPAYMENT,
     tfNoRippleDirect | tfPartialPayment | tfLimitQuality | tfSponsorCreatedAccountThere,
     {req(sfDestination), req(sfAmount), sfSendMax, sfPaths, sfInvoiceID,
      sfDestinationTag, sfDeliverMin}},
    {ttESCROW_CREATE, 0,
     {req(sfDestination), req(sfAmount), sfCondition, sfCancelAfter,
      sfFinishAfter, sfDestinationTag}},
    // a Fulfillment is checked there at 32 reference fees and one more per
    // 16 bytes of it (EscrowFinish::calculateBaseFee)
    {ttESCROW_FINISH, 0,
     {req(sfOwner), req(sfOfferSequence), sfFulfillment, sfCondition},
     {0, &sfFulfillment, 32, 16}},
    {ttACCOUNT_SET,
     tfRequireDestTag | tfOptionalDestTag | tfRequireAuth | tfOptionalAuth |
         tfDisallowXRP | tfAllowXRP,
     {sfEmailHash, sfWalletLocator, sfWalletSize, sfMessageKey, sfDomain,
      sfTransferRate, sfSetFlag, sfClearFlag, sfTickSize, sfNFTokenMinter}},
    {ttESCROW_CANCEL, 0,
     {req(sfOwner), req(sfOfferSequence)}},
    {ttOFFER_CREATE,
     tfPassive | tfImmediateOrCancel | tfFillOrKill | tfSell | tfHybridThere,
     {req(sfTakerPays), req(sfTakerGets), sfExpiration, sfOfferSequence}},
    {ttOFFER_CANCEL, 0,
     {req(sfOfferSequence)}},
    {ttTICKET_CREATE, 0,
     {req(sfTicketCount)}},
    {ttSIGNER_LIST_SET, 0,
     {req(sfSignerQuorum), sfSignerEntries}},
    {ttPAYCHAN_CREATE, 0,
     {req(sfDestination), req(sfAmount), req(sfSettleDelay), req(sfPublicKey),
      sfCancelAfter, sfDestinationTag}},
    {ttPAYCHAN_FUND, 0,
     {req(sfChannel), req(sfAmount), sfExpiration}},
    {ttPAYCHAN_CLAIM, tfRenew | tfClose,
     {req(sfChannel), sfAmount, sfBalance, sfSignature, sfPublicKey}},
    {ttCHECK_CREATE, 0,
     {req(sfDestination), req(sfSendMax), sfExpiration, sfDestinationTag,
      sfInvoiceID}},
    {ttCHECK_CASH, 0,
     {req(sfCheckID), sfAmount, sfDeliverMin}},
    {ttCHECK_CANCEL, 0,
     {req(sfCheckID)}},
    {ttDEPOSIT_PREAUTH, 0,
     {sfAuthorize, sfUnauthorize}},  // not the credential forms: no shared Credential
    {ttTRUST_SET,
     tfSetfAuth | tfSetNoRipple | tfClearNoRipple | tfSetFreeze | tfClearFreeze |
         tfSetDeepFreeze | tfClearDeepFreeze,
     {sfLimitAmount, sfQualityIn, sfQualityOut}},
};

// The fields every export may carry: the other network's common transaction
// fields, less what an export must not have (a signature of its own,
// PreviousTxnID, delegation, batching). Signers is in because validators add
// it as they sign. Export.cpp checks Account, SigningPubKey and friends.
constexpr std::array<Rule, 14> universal = {
    sfTransactionType, sfFlags, sfSourceTag, sfAccount, sfSequence,
    sfLastLedgerSequence, sfAccountTxnID, sfFee, sfOperationLimit, sfMemos,
    sfSigningPubKey, sfTicketSequence, sfSigners, sfNetworkID,
};

// The arrays an export may carry, the one object each may hold, and what
// that object may hold. Nothing nests deeper.
struct Inner
{
    SField const* array;
    SField const* object;
    std::array<Rule, 4> fields;
};

constexpr Inner inners[] = {
    {&sfMemos,         &sfMemo,        {sfMemoType, sfMemoData, sfMemoFormat}},
    {&sfSigners,       &sfSigner,      {req(sfAccount), req(sfSigningPubKey), req(sfTxnSignature)}},
    {&sfSignerEntries, &sfSignerEntry, {req(sfAccount), req(sfSignerWeight), sfWalletLocator}},
};
// clang-format on

//------------------------------------------------------------------------------

template <class Row>
bool
enabled(Row const& r, Rules const& rules)
{
    return (!r.since || rules.enabled(*r.since)) &&
        !(r.until && rules.enabled(*r.until));
}

bool
same(SField const* f, SField const& g)
{
    return f && f->fieldCode == g.fieldCode;
}

template <std::size_t N>
Rule const*
find(std::array<Rule, N> const& rows, SField const& f, Rules const& rules)
{
    for (auto const& r : rows)
        if (same(r.field, f) && enabled(r, rules))
            return &r;
    return nullptr;
}

Type const*
findType(TxType t, Rules const& rules)
{
    for (auto const& row : typeRows)
        if (row.type == t && enabled(row, rules))
            return &row;
    return nullptr;
}

Inner const*
findInner(SField const& array)
{
    for (auto const& i : inners)
        if (same(i.array, array))
            return &i;
    return nullptr;
}

// Every enabled, required rule in `rows` is present in `o`
template <std::size_t N>
std::optional<std::string>
missing(std::array<Rule, N> const& rows, STObject const& o, Rules const& rules)
{
    for (auto const& r : rows)
        if (r.field && r.required && enabled(r, rules) &&
            !o.isFieldPresent(*r.field))
            return "the other network requires " + r.field->getName();
    return std::nullopt;
}

// `o` is an element of the array `inner` describes
std::optional<std::string>
rejectInner(Inner const& inner, STObject const& o, Rules const& rules)
{
    if (!same(inner.object, o.getFName()))
        return "object " + o.getFName().getName() + " is not exportable in " +
            inner.array->getName();
    for (auto const& e : o)
    {
        if (e.getSType() == STI_NOTPRESENT)
            continue;
        if (e.getSType() == STI_OBJECT || e.getSType() == STI_ARRAY)
            return "field " + e.getFName().getName() + " nests too deep";
        if (!find(inner.fields, e.getFName(), rules))
            return "field " + e.getFName().getName() + " is not exportable";
    }
    return missing(inner.fields, o, rules);
}

}  // namespace

//------------------------------------------------------------------------------

std::optional<std::string>
ExportSchema::reject(STObject const& tx, Rules const& rules)
{
    if (!rules.enabled(featureExport))
        return "export is not enabled";
    if (!tx.isFieldPresent(sfTransactionType))
        return "no TransactionType";
    auto const t =
        findType(static_cast<TxType>(tx.getFieldU16(sfTransactionType)), rules);
    if (!t)
        return "transaction type is not exportable";

    if (auto const flags = tx[~sfFlags].value_or(0);
        flags & ~(t->flags | tfUniversal))
        return "flags the other network does not accept on it";

    for (auto const& e : tx)
    {
        if (e.getSType() == STI_NOTPRESENT)
            continue;
        auto const& f = e.getFName();
        if (!find(t->fields, f, rules) && !find(universal, f, rules))
            return "field " + f.getName() + " is not exportable";
        if (e.getSType() == STI_OBJECT)
            return "field " + f.getName() + " is not exportable";
        if (e.getSType() == STI_ARRAY)
        {
            auto const inner = findInner(f);
            if (!inner)
                return "field " + f.getName() + " is not exportable";
            for (auto const& o : e.downcast<STArray>())
                if (auto r = rejectInner(*inner, o, rules))
                    return r;
        }
    }

    return missing(t->fields, tx, rules);
}

std::uint64_t
ExportSchema::extraFeeUnits(STObject const& tx, Rules const& rules)
{
    if (!rules.enabled(featureExport) || !tx.isFieldPresent(sfTransactionType))
        return 0;
    auto const t =
        findType(static_cast<TxType>(tx.getFieldU16(sfTransactionType)), rules);
    if (!t)
        return 0;

    std::uint64_t units = t->fee.flat;
    if (auto const f = t->fee.field; f && tx.isFieldPresent(*f))
    {
        units += t->fee.present;
        // (only a blob has a size to charge for; the table names no other)
        if (t->fee.per != 0 && f->fieldType == STI_VL)
            units += tx.getFieldVL(*f).size() / t->fee.per;
    }
    return units;
}

std::uint64_t
ExportSchema::feeUnits(
    STObject const& tx,
    std::size_t signers,
    Rules const& rules)
{
    return 1 + std::uint64_t{signers} + extraFeeUnits(tx, rules);
}

std::vector<std::string_view>
ExportSchema::types(Rules const& rules)
{
    std::vector<std::string_view> out;
    if (!rules.enabled(featureExport))
        return out;
    for (auto const& row : typeRows)
        if (enabled(row, rules))
            if (auto const f = TxFormats::getInstance().findByType(row.type))
                out.push_back(f->getName());
    return out;
}

}  // namespace ripple
