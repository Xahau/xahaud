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

#include <xrpl/beast/hash/uhash.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ExportSchema.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Rules.h>
#include <xrpl/protocol/STAccount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace ripple {

/** ExportSchema: what featureExport lets a hook send to the other network */
class ExportSchema_test : public beast::unit_test::suite
{
    // the fields every export carries, from `from`
    static STObject
    base(TxType type, AccountID const& from)
    {
        STObject t(sfExportedTxn);
        t.setFieldU16(sfTransactionType, type);
        t.setAccountID(sfAccount, from);
        t.setFieldAmount(sfFee, XRPAmount{0});
        t.setFieldU32(sfSequence, 0);
        t.setFieldVL(sfSigningPubKey, Slice{});
        t.setFieldU32(sfOperationLimit, 21337);
        return t;
    }

    static STObject
    payment(AccountID const& from)
    {
        auto t = base(ttPAYMENT, from);
        t.setAccountID(sfDestination, noAccount());
        t.setFieldAmount(sfAmount, XRPAmount{1});
        return t;
    }

    // Rules keeps a reference to its presets (in a node, the config's
    // features), so these must outlive every Rules built from them: a set
    // passed as a temporary dangles as soon as the Rules is returned.
    std::unordered_set<uint256, beast::uhash<>> const noFeatures_;
    std::unordered_set<uint256, beast::uhash<>> const exportFeature_{
        featureExport};

    Rules
    rules(bool exportOn) const
    {
        return Rules{exportOn ? exportFeature_ : noFeatures_};
    }

    void
    testTypes()
    {
        testcase("types");

        auto const on = rules(true);
        auto const off = rules(false);
        auto const alice = AccountID{1};

        // nothing is exportable before the amendment
        BEAST_EXPECT(ExportSchema::types(off).empty());
        BEAST_EXPECT(!ExportSchema::allowed(payment(alice), off));

        auto const types = ExportSchema::types(on);
        auto const has = [&](std::string_view name) {
            return std::find(types.begin(), types.end(), name) != types.end();
        };
        BEAST_EXPECT(has("Payment"));
        BEAST_EXPECT(has("TicketCreate"));
        BEAST_EXPECT(has("SignerListSet"));
        BEAST_EXPECT(!has("AccountDelete"));
        BEAST_EXPECT(!has("SetRegularKey"));

        BEAST_EXPECT(ExportSchema::allowed(payment(alice), on));

        // this network's own types, and shared names the networks number
        // differently (XChainCommit is 51 here, 42 there)
        for (auto const type :
             {ttURITOKEN_MINT, ttREMIT, ttINVOKE, ttIMPORT, ttXCHAIN_COMMIT})
            BEAST_EXPECT(!ExportSchema::allowed(base(type, alice), on));

        // what the ledger exports by itself: a TicketCreate, and a
        // SignerListSet whose entries are filled in as it applies
        {
            auto t = base(ttTICKET_CREATE, alice);
            t.setFieldU32(sfTicketCount, 50);
            BEAST_EXPECT(ExportSchema::allowed(t, on));
            // TicketCount is required there
            BEAST_EXPECT(
                !ExportSchema::allowed(base(ttTICKET_CREATE, alice), on));
        }
        {
            auto t = base(ttSIGNER_LIST_SET, alice);
            t.setFieldU32(sfSignerQuorum, 0);
            BEAST_EXPECT(ExportSchema::allowed(t, on));
            STArray entries(sfSignerEntries);
            entries.push_back(STObject::makeInnerObject(sfSignerEntry));
            entries.back()[sfAccount] = alice;
            entries.back()[sfSignerWeight] = 1;
            t.setFieldArray(sfSignerEntries, entries);
            BEAST_EXPECT(ExportSchema::allowed(t, on));
        }
    }

    void
    testFields()
    {
        testcase("fields");

        auto const on = rules(true);
        auto const alice = AccountID{1};

        // the other network's optional fields
        {
            auto t = payment(alice);
            t.setFieldU32(sfDestinationTag, 1);
            t.setFieldU32(sfLastLedgerSequence, 1);
            t.setFieldU32(sfTicketSequence, 7);
            t.setFieldAmount(sfSendMax, XRPAmount{2});
            BEAST_EXPECT(ExportSchema::allowed(t, on));
        }

        // Memos, nested, are made of shared fields
        {
            auto t = payment(alice);
            STArray memos(sfMemos);
            memos.push_back(STObject::makeInnerObject(sfMemo));
            memos.back().setFieldVL(sfMemoData, Slice{"hi", 2});
            t.setFieldArray(sfMemos, memos);
            BEAST_EXPECT(ExportSchema::allowed(t, on));
        }

        // Signers, which validators add as they sign
        {
            auto t = payment(alice);
            STArray signers(sfSigners);
            signers.push_back(STObject::makeInnerObject(sfSigner));
            signers.back()[sfAccount] = alice;
            signers.back().setFieldVL(sfSigningPubKey, Slice{});
            signers.back().setFieldVL(sfTxnSignature, Slice{});
            t.setFieldArray(sfSigners, signers);
            BEAST_EXPECT(ExportSchema::allowed(t, on));
        }

        // an inner object with a field it may not hold; one missing a field
        // it must hold (built free-form: makeInnerObject fills the
        // template's required fields in); the wrong object in an array
        {
            auto t = payment(alice);
            STArray memos(sfMemos);
            memos.push_back(STObject::makeInnerObject(sfMemo));
            memos.back().setFieldVL(sfMemoData, Slice{"hi", 2});
            memos.back().setFieldU32(sfSequence, 1);
            t.setFieldArray(sfMemos, memos);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }
        {
            auto t = payment(alice);
            STArray signers(sfSigners);
            STObject signer(sfSigner);
            signer[sfAccount] = alice;
            signers.push_back(std::move(signer));
            t.setFieldArray(sfSigners, signers);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }
        {
            auto t = payment(alice);
            STArray memos(sfMemos);
            memos.push_back(STObject::makeInnerObject(sfSignerEntry));
            memos.back()[sfAccount] = alice;
            memos.back()[sfSignerWeight] = 1;
            t.setFieldArray(sfMemos, memos);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }

        // this network's own fields, in a shared type
        {
            auto t = payment(alice);
            t.setFieldU32(sfFirstLedgerSequence, 1);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }
        {
            auto t = payment(alice);
            auto& d = t.peekFieldObject(sfEmitDetails);
            d.setFieldU32(sfEmitGeneration, 1);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }
        {
            auto t = payment(alice);
            STArray params(sfHookParameters);
            t.setFieldArray(sfHookParameters, params);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }

        // a field both networks have but number differently: DomainID is
        // Hash256/37 here and Hash256/34 there, so it reads as something
        // else on arrival
        {
            auto t = payment(alice);
            t.setFieldH256(sfDomainID, uint256{1});
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }

        // a shared field the other network does not list on this type
        {
            auto t = payment(alice);
            t.setFieldU32(sfTicketCount, 1);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }

        // a required field missing
        {
            auto t = payment(alice);
            t.makeFieldAbsent(sfDestination);
            BEAST_EXPECT(!ExportSchema::allowed(t, on));
        }
    }

    void
    testFlags()
    {
        testcase("flags");

        auto const on = rules(true);
        auto const alice = AccountID{1};

        auto const with = [&](std::uint32_t flags) {
            auto t = payment(alice);
            t.setFieldU32(sfFlags, flags);
            return ExportSchema::allowed(t, on);
        };
        BEAST_EXPECT(with(tfFullyCanonicalSig));
        BEAST_EXPECT(with(tfPartialPayment | tfNoRippleDirect));
        BEAST_EXPECT(!with(0x40000000));   // tfInnerBatchTxn there
        BEAST_EXPECT(!with(tfSetFreeze));  // TrustSet's, not Payment's
        BEAST_EXPECT(!with(0x1));
    }

public:
    void
    run() override
    {
        testTypes();
        testFields();
        testFlags();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSchema, protocol, ripple);

}  // namespace ripple
