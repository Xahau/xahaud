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

#include <xrpl/basics/contract.h>
#include <xrpl/protocol/ExportSchema.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/XrplDefinitions.h>
#include <libxrpl/protocol/xrpl_server_definitions.h>
#include <stdexcept>

namespace ripple {

namespace xrpl_defs {
namespace detail {

void
malformed(char const* what)
{
    // only reached at run time, which the constexpr reads below never are
    Throw<std::logic_error>(
        std::string("xrpl_server_definitions.json: ") + what);
}

}  // namespace detail
}  // namespace xrpl_defs

namespace {

//------------------------------------------------------------------------------
//
// Policy: which transaction types may be exported, and since which
// amendment. This is the only hand-maintained part of the schema. A row
// names a transaction type as both networks name it; the build checks it
// has the same code on both and that this network can read every field the
// other requires on it, so a row for a type only one network has, or that
// the two number differently, does not compile.
//
// To open a type later: add a row gated on the feature that brings it in,
// refreshing xrpl_server_definitions.json first if the other network has
// added it since. To close one: set removedBy.
//
// Left out on purpose: AccountDelete and SetRegularKey, which would take the
// exporter's account there out from under the signer list this ledger keeps
// for it; anything that is not a plain account's business.

struct Policy
{
    std::string_view tx;
    uint256 const& addedBy;
    uint256 const* removedBy;
};

constexpr Policy policy[] = {
    {"Payment", featureExport, nullptr},
    {"TrustSet", featureExport, nullptr},
    {"AccountSet", featureExport, nullptr},
    {"OfferCreate", featureExport, nullptr},
    {"OfferCancel", featureExport, nullptr},
    {"EscrowCreate", featureExport, nullptr},
    {"EscrowFinish", featureExport, nullptr},
    {"EscrowCancel", featureExport, nullptr},
    {"CheckCreate", featureExport, nullptr},
    {"CheckCash", featureExport, nullptr},
    {"CheckCancel", featureExport, nullptr},
    {"PaymentChannelCreate", featureExport, nullptr},
    {"PaymentChannelFund", featureExport, nullptr},
    {"PaymentChannelClaim", featureExport, nullptr},
    {"DepositPreauth", featureExport, nullptr},
    {"TicketCreate", featureExport, nullptr},
    {"SignerListSet", featureExport, nullptr},
};

// The fields every transaction on the other network may carry that an export
// may carry too (its TxFormats common fields, less what an export must not
// have: a signature of its own, PreviousTxnID, delegation, batching).
// Signers is included because validators add it as they sign.
constexpr std::string_view universalFields[] = {
    "TransactionType",
    "Flags",
    "SourceTag",
    "Account",
    "Sequence",
    "LastLedgerSequence",
    "AccountTxnID",
    "Fee",
    "OperationLimit",
    "Memos",
    "SigningPubKey",
    "TicketSequence",
    "Signers",
    "NetworkID",
};

//------------------------------------------------------------------------------

using namespace xrpl_defs;

constexpr auto xrpl = parseXrplDefinitions<xrplServerDefinitionsJson>();

// This network's fields, from the table SField.cpp is built from
struct LocalField
{
    std::string_view name;
    int type;
    int nth;
};

#pragma push_macro("UNTYPED_SFIELD")
#undef UNTYPED_SFIELD
#pragma push_macro("TYPED_SFIELD")
#undef TYPED_SFIELD

#define UNTYPED_SFIELD(sfName, stiSuffix, fieldValue, ...) \
    LocalField{                                            \
        std::string_view(#sfName).substr(2), STI_##stiSuffix, fieldValue},
#define TYPED_SFIELD(sfName, stiSuffix, fieldValue, ...) \
    LocalField{                                          \
        std::string_view(#sfName).substr(2), STI_##stiSuffix, fieldValue},

constexpr LocalField localFields[] = {
#include <xrpl/protocol/detail/sfields.macro>
};

#undef TYPED_SFIELD
#pragma pop_macro("TYPED_SFIELD")
#undef UNTYPED_SFIELD
#pragma pop_macro("UNTYPED_SFIELD")

// and its transaction types, from the table TxFormats.cpp is built from
struct LocalTxType
{
    std::string_view name;
    int code;
};

#pragma push_macro("TRANSACTION")
#undef TRANSACTION
#define TRANSACTION(tag, value, name, ...) LocalTxType{#name, value},

constexpr LocalTxType localTxTypes[] = {
#include <xrpl/protocol/detail/transactions.macro>
};

#undef TRANSACTION
#pragma pop_macro("TRANSACTION")

constexpr LocalField const*
localField(std::string_view name)
{
    for (auto const& f : localFields)
        if (f.name == name)
            return &f;
    return nullptr;
}

constexpr LocalTxType const*
localTxType(std::string_view name)
{
    for (auto const& t : localTxTypes)
        if (t.name == name)
            return &t;
    return nullptr;
}

//------------------------------------------------------------------------------

struct FieldCode
{
    int type;
    int nth;

    constexpr bool
    operator==(FieldCode const&) const = default;
};

// A field both networks define identically: same name, type and code. Only
// such a field reads the same on both sides of an export.
constexpr std::optional<FieldCode>
shared(std::string_view name)
{
    auto const theirs = xrpl.field(name);
    auto const ours = localField(name);
    if (!theirs || !ours || theirs->type != ours->type ||
        theirs->nth != ours->nth || !theirs->serialized)
        return std::nullopt;
    return FieldCode{ours->type, ours->nth};
}

constexpr std::size_t maxFields = 64;

struct ExportType
{
    std::string_view name;
    std::uint16_t code;
    Policy const* policy;
    std::uint32_t flagsMask;
    std::array<FieldCode, maxFields> allowed;
    std::size_t nAllowed;
    std::array<FieldCode, maxFields> required;
    std::size_t nRequired;

    constexpr bool
    allows(FieldCode f) const
    {
        for (std::size_t i = 0; i < nAllowed; ++i)
            if (allowed[i] == f)
                return true;
        return false;
    }
};

constexpr ExportType
exportType(Policy const& p)
{
    ExportType t{};
    t.name = p.tx;
    t.policy = &p;

    auto const theirs = xrpl.txType(p.tx);
    if (!theirs)
        xrpl_defs::detail::malformed("policy names a transaction they lack");
    auto const ours = localTxType(p.tx);
    if (!ours)
        xrpl_defs::detail::malformed("policy names a transaction we lack");
    if (theirs->code != ours->code || theirs->code < 0 || theirs->code > 0xFFFF)
        xrpl_defs::detail::malformed(
            "policy names a transaction numbered differently");
    t.code = static_cast<std::uint16_t>(ours->code);

    auto const fmt = xrpl.format(p.tx);
    if (!fmt)
        xrpl_defs::detail::malformed(
            "they list no format for a policy transaction");

    t.flagsMask = xrpl.flagsMask(p.tx) | tfFullyCanonicalSig;

    auto const allow = [&](FieldCode f) {
        if (t.nAllowed == maxFields)
            xrpl_defs::detail::malformed("too many fields");
        t.allowed[t.nAllowed++] = f;
    };

    for (auto const name : universalFields)
    {
        auto const f = shared(name);
        if (!f)
            xrpl_defs::detail::malformed("a universal field is not shared");
        allow(*f);
    }

    for (std::size_t i = 0; i < fmt->count; ++i)
    {
        auto const& ff = xrpl.formatFields[fmt->first + i];
        auto const f = shared(ff.name);
        if (!f)
        {
            // theirs alone, or numbered differently here: not exportable,
            // unless they require it, in which case neither is the type
            if (ff.optionality == 0)
                xrpl_defs::detail::malformed(
                    "they require a field we cannot send");
            continue;
        }
        allow(*f);
        if (ff.optionality == 0)
            t.required[t.nRequired++] = *f;
    }
    return t;
}

constexpr auto exportTypes = [] {
    std::array<ExportType, std::size(policy)> out{};
    for (std::size_t i = 0; i < std::size(policy); ++i)
        out[i] = exportType(policy[i]);
    return out;
}();

// Every field the two networks define identically, for what is nested
constexpr auto sharedFields = [] {
    std::array<FieldCode, std::size(localFields)> out{};
    std::size_t n = 0;
    for (auto const& f : localFields)
        if (auto const s = shared(f.name))
            out[n++] = *s;
    return std::pair{out, n};
}();

constexpr bool
isShared(FieldCode f)
{
    for (std::size_t i = 0; i < sharedFields.second; ++i)
        if (sharedFields.first[i] == f)
            return true;
    return false;
}

// the build itself is the check: a policy row that does not hold fails above
static_assert(exportTypes.size() == std::size(policy));
static_assert(!xrpl.hash.empty(), "definitions carry no hash");

//------------------------------------------------------------------------------

constexpr ExportType const*
find(std::uint16_t code)
{
    for (auto const& t : exportTypes)
        if (t.code == code)
            return &t;
    return nullptr;
}

bool
enabled(Policy const& p, Rules const& rules)
{
    return rules.enabled(p.addedBy) &&
        !(p.removedBy && rules.enabled(*p.removedBy));
}

FieldCode
codeOf(STBase const& e)
{
    auto const& f = e.getFName();
    return {f.fieldType, f.fieldValue};
}

// the first field at any depth below `o` that the networks do not share
std::optional<std::string>
unsharedWithin(STBase const& e)
{
    if (e.getSType() == STI_OBJECT)
    {
        for (auto const& inner : e.downcast<STObject>())
        {
            if (inner.getSType() == STI_NOTPRESENT)
                continue;
            if (!isShared(codeOf(inner)))
                return inner.getFName().getName();
            if (auto r = unsharedWithin(inner))
                return r;
        }
    }
    else if (e.getSType() == STI_ARRAY)
    {
        for (auto const& inner : e.downcast<STArray>())
        {
            if (!isShared(codeOf(inner)))
                return inner.getFName().getName();
            if (auto r = unsharedWithin(inner))
                return r;
        }
    }
    return std::nullopt;
}

}  // namespace

//------------------------------------------------------------------------------

std::string_view
ExportSchema::xrplDefinitionsHash()
{
    return xrpl.hash;
}

std::optional<std::string>
ExportSchema::reject(STObject const& tx, Rules const& rules)
{
    if (!tx.isFieldPresent(sfTransactionType))
        return "no TransactionType";
    auto const t = find(tx.getFieldU16(sfTransactionType));
    if (!t)
        return "transaction type is not exportable";
    if (!enabled(*t->policy, rules))
        return "transaction type is not exportable yet";

    if (auto const flags = tx[~sfFlags].value_or(0); flags & ~t->flagsMask)
        return "flags the other network does not accept on it";

    for (auto const& e : tx)
    {
        if (e.getSType() == STI_NOTPRESENT)
            continue;
        if (!t->allows(codeOf(e)))
            return "field " + e.getFName().getName() + " is not exportable";
        if (auto const f = unsharedWithin(e))
            return "field " + *f + " is not exportable";
    }

    for (std::size_t i = 0; i < t->nRequired; ++i)
    {
        auto const& f = t->required[i];
        if (!tx.isFieldPresent(SField::getField(f.type, f.nth)))
            return "a field the other network requires is missing";
    }

    return std::nullopt;
}

std::vector<std::string_view>
ExportSchema::types(Rules const& rules)
{
    std::vector<std::string_view> out;
    for (auto const& t : exportTypes)
        if (enabled(*t.policy, rules))
            out.push_back(t.name);
    return out;
}

}  // namespace ripple
