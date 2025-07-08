//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class PreviousTxnID_test : public beast::unit_test::suite
{
public:
    void
    testPreviousTxnID(FeatureBitset features)
    {
        using namespace test::jtx;
        Env env{
            *this, envconfig(), features, nullptr, beast::severities::kNone};
        auto j = env.app().logs().journal("PreviousTxnID_test");

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        auto const USD = bob["USD"];

        env.fund(XRP(10000), alice, bob);
        env.close();

        // Create a trustline
        env(trust(alice, USD(1000)));
        env.close();

        // Get the transaction metadata and ID
        auto const meta1 = env.meta();
        auto const trustCreateTxID = env.tx()->getTransactionID();
        BEAST_EXPECT(meta1);

        // Check if ModifiedNode has PreviousTxnID at root level
        auto const& affectedNodes = meta1->getFieldArray(sfAffectedNodes);
        bool foundPreviousTxnID = false;
        bool foundModifiedRippleState = false;
        bool foundCreatedRippleState = false;

        for (auto const& node : affectedNodes)
        {
            if (node.getFieldU16(sfLedgerEntryType) == ltRIPPLE_STATE)
            {
                if (node.getFName() == sfModifiedNode)
                {
                    BEAST_EXPECT(false);
                }
                else if (node.getFName() == sfCreatedNode)
                {
                    foundCreatedRippleState = true;
                    foundPreviousTxnID = node.isFieldPresent(sfPreviousTxnID);
                }
            }
        }

        BEAST_EXPECT(foundCreatedRippleState);
        // Why we expect NO PreviousTxnID in newly created trustline metadata:
        //
        // PreviousTxnID in metadata indicates which transaction PREVIOUSLY
        // modified an object, not which transaction created it. For a newly
        // created object, there is no previous transaction - the current
        // transaction is the first one to touch this object.
        //
        // While the SLE itself will have PreviousTxnID set to the creating
        // transaction (by ApplyStateTable::threadItem), the metadata correctly
        // omits PreviousTxnID from CreatedNode because there was no previous
        // modification to reference.
        //
        // Technical detail: trustCreate() creates the RippleState with
        // PreviousTxnID as a defaultObject placeholder (since it's soeREQUIRED
        // in LedgerFormats.cpp). ApplyStateTable::generateTxMeta skips fields
        // with zero/empty values when generating metadata, so the unset
        // PreviousTxnID doesn't appear in the CreatedNode output.
        BEAST_EXPECT(foundPreviousTxnID == false);
        BEAST_EXPECT(foundModifiedRippleState == false);

        // Now let's check the actual ledger entry to see if PreviousTxnID was
        // set Get the trustline from the ledger
        auto const trustlineKey = keylet::line(alice, bob, USD.currency);
        auto const sleTrustline = env.le(trustlineKey);
        BEAST_EXPECT(sleTrustline);

        if (sleTrustline)
        {
            // Check if the SLE itself has PreviousTxnID set
            // Even though it didn't appear in metadata, the SLE should have it
            // because ApplyStateTable::threadItem() sets it after creation
            bool sleHasPrevTxnID =
                sleTrustline->isFieldPresent(sfPreviousTxnID);
            bool sleHasPrevTxnSeq =
                sleTrustline->isFieldPresent(sfPreviousTxnLgrSeq);

            JLOG(j.info()) << "SLE has PreviousTxnID: " << sleHasPrevTxnID;
            JLOG(j.info()) << "SLE has PreviousTxnLgrSeq: " << sleHasPrevTxnSeq;

            if (sleHasPrevTxnID && sleHasPrevTxnSeq)
            {
                auto const prevTxnID =
                    sleTrustline->getFieldH256(sfPreviousTxnID);
                auto const prevTxnSeq =
                    sleTrustline->getFieldU32(sfPreviousTxnLgrSeq);
                auto const creatingTxnID = env.tx()->getTransactionID();

                JLOG(j.info()) << "SLE PreviousTxnID: " << prevTxnID;
                JLOG(j.info()) << "Creating TxnID: " << creatingTxnID;
                JLOG(j.info()) << "SLE PreviousTxnLgrSeq: " << prevTxnSeq;

                // The PreviousTxnID in the SLE should match the transaction
                // that created it
                BEAST_EXPECT(prevTxnID == creatingTxnID);
                BEAST_EXPECT(prevTxnSeq == env.closed()->seq());
            }
            else
            {
                // This would indicate that ApplyStateTable::threadItem() didn't
                // set PreviousTxnID on the newly created trustline
                JLOG(j.warn())
                    << "Newly created trustline SLE missing PreviousTxnID!";
                BEAST_EXPECT(false);
            }
        }

        // Now modify the trustline with a payment
        env(pay(bob, alice, USD(100)));
        env.close();

        // Get the second transaction metadata
        auto const meta2 = env.meta();
        BEAST_EXPECT(meta2);

        // Check ModifiedNode for PreviousTxnID
        auto const& affectedNodes2 = meta2->getFieldArray(sfAffectedNodes);
        bool foundPreviousTxnIDInModified = false;
        bool foundPreviousTxnIDInPreviousFields = false;

        for (auto const& node : affectedNodes2)
        {
            if (node.getFName() == sfModifiedNode &&
                node.getFieldU16(sfLedgerEntryType) == ltRIPPLE_STATE)
            {
                auto json = node.getJson({});
                JLOG(j.trace()) << json;

                // Why we expect PreviousTxnID in ModifiedNode metadata:
                //
                // When a transaction modifies an existing RippleState, the
                // ApplyView tracks the modification. During metadata
                // generation, ApplyStateTable::generateTxMeta compares the
                // before and after states of the SLE.
                //
                // For ModifiedNode entries, the metadata should include
                // PreviousTxnID and PreviousTxnLgrSeq at the root level to
                // indicate which transaction last modified this object.
                // This is different from PreviousFields, which shows what
                // field values changed.
                //
                // The bug fixed by the `fixProvisionalDoubleThreading`
                // amendment was that ApplyStateTable::threadItem() was
                // modifying the original SLE during provisional metadata
                // generation. This contaminated the "before" state used for
                // comparison, so when final metadata was generated, the
                // comparison didn't see PreviousTxnID as a change because both
                // states had the new value.
                bool expectPreviousTxnID =
                    features[fixProvisionalDoubleThreading];

                if (node.isFieldPresent(sfPreviousTxnID))
                {
                    foundPreviousTxnIDInModified = true;
                    auto prevTxnID = node.getFieldH256(sfPreviousTxnID);
                    auto prevLgrSeq = node.getFieldU32(sfPreviousTxnLgrSeq);

                    BEAST_EXPECT(node.isFieldPresent(sfPreviousTxnLgrSeq));
                    BEAST_EXPECT(prevTxnID != beast::zero);
                    BEAST_EXPECT(prevLgrSeq > 0);

                    JLOG(j.info()) << "Found PreviousTxnID: " << prevTxnID
                                   << " at ledger: " << prevLgrSeq << std::endl;

                    // When the fix is enabled, we should see the trustline
                    // creation transaction ID as the previous transaction
                    if (expectPreviousTxnID)
                    {
                        BEAST_EXPECT(prevTxnID == trustCreateTxID);
                    }
                }
                else
                {
                    // Without the fix, we expect PreviousTxnID to be missing
                    // due to the provisional metadata contamination bug
                    JLOG(j.info()) << "PreviousTxnID missing in metadata";
                }

                // Check if PreviousTxnID appears in PreviousFields
                // (it shouldn't - PreviousTxnID is a root-level field)
                if (node.isFieldPresent(sfPreviousFields))
                {
                    auto prevFields = dynamic_cast<STObject const*>(
                        node.peekAtPField(sfPreviousFields));
                    if (prevFields &&
                        prevFields->isFieldPresent(sfPreviousTxnID))
                    {
                        foundPreviousTxnIDInPreviousFields = true;
                        JLOG(j.warn()) << "Found PreviousTxnID in "
                                          "PreviousFields (unexpected)";
                    }
                }
            }
        }

        // PreviousTxnID should never appear in PreviousFields
        BEAST_EXPECT(!foundPreviousTxnIDInPreviousFields);

        // With the fix enabled, we expect to find PreviousTxnID
        // Without the fix, we expect it to be missing (the bug)
        if (features[fixProvisionalDoubleThreading])
        {
            BEAST_EXPECT(foundPreviousTxnIDInModified);
        }
        else
        {
            BEAST_EXPECT(!foundPreviousTxnIDInModified);
        }

        // Additional check: Verify the SLE state after the payment
        auto const sleTrustlineAfter = env.le(trustlineKey);
        BEAST_EXPECT(sleTrustlineAfter);

        if (sleTrustlineAfter)
        {
            // The SLE should always have PreviousTxnID set after modification
            BEAST_EXPECT(sleTrustlineAfter->isFieldPresent(sfPreviousTxnID));
            BEAST_EXPECT(
                sleTrustlineAfter->isFieldPresent(sfPreviousTxnLgrSeq));

            auto const currentPrevTxnID =
                sleTrustlineAfter->getFieldH256(sfPreviousTxnID);
            auto const currentPrevTxnSeq =
                sleTrustlineAfter->getFieldU32(sfPreviousTxnLgrSeq);

            // The PreviousTxnID should now point to the payment transaction
            auto const paymentTxID = env.tx()->getTransactionID();
            BEAST_EXPECT(currentPrevTxnID == paymentTxID);
            BEAST_EXPECT(currentPrevTxnSeq == env.closed()->seq());

            // When the bug is present (feature disabled), the metadata won't
            // show the change, but the SLE will still be updated correctly
            if (!features[fixProvisionalDoubleThreading])
            {
                JLOG(j.info())
                    << "Bug confirmed: SLE has correct PreviousTxnID ("
                    << currentPrevTxnID
                    << ") but metadata doesn't show the change";
            }
        }

        // Check account objects were threaded correctly
        auto const aliceAccount = env.le(keylet::account(alice));
        auto const bobAccount = env.le(keylet::account(bob));

        BEAST_EXPECT(aliceAccount);
        BEAST_EXPECT(bobAccount);

        if (aliceAccount && bobAccount)
        {
            // Both accounts should have been threaded by the payment
            BEAST_EXPECT(aliceAccount->isFieldPresent(sfPreviousTxnID));
            BEAST_EXPECT(bobAccount->isFieldPresent(sfPreviousTxnID));

            auto const alicePrevTxnID =
                aliceAccount->getFieldH256(sfPreviousTxnID);
            auto const bobPrevTxnID = bobAccount->getFieldH256(sfPreviousTxnID);
            auto const paymentTxID = env.tx()->getTransactionID();

            // Both should point to the payment transaction
            BEAST_EXPECT(alicePrevTxnID == paymentTxID);
            BEAST_EXPECT(bobPrevTxnID == paymentTxID);
        }
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();

        testcase("With fixProvisionalDoubleThreading enabled");
        testPreviousTxnID(sa);

        testcase("Without fixProvisionalDoubleThreading (bug present)");
        testPreviousTxnID(sa - fixProvisionalDoubleThreading);
    }
};

BEAST_DEFINE_TESTSUITE(PreviousTxnID, app, ripple);

}  // namespace test
}  // namespace ripple
