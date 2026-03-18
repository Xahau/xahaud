//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <test/jtx.h>
#include <test/jtx/xpop.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/proof/LedgerProof.h>
#include <xrpld/app/proof/ProofBuilder.h>
#include <xrpld/app/proof/XPOPv1.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

struct XPOP_test : public beast::unit_test::suite
{
    void
    testBuildLedgerProof()
    {
        testcase("Build LedgerProof from a payment");

        using namespace jtx;

        Env env{*this};

        Account const alice{"alice"};
        Account const bob{"bob"};

        env.fund(XRP(10000), alice, bob);
        env.close();

        // Submit a payment and close the ledger.
        env(pay(alice, bob, XRP(100)));
        env.close();

        // Get the tx hash from the last closed ledger.
        auto const lcl = env.app().getLedgerMaster().getClosedLedger();
        BEAST_EXPECT(lcl);

        // Find a payment tx in the ledger.
        uint256 paymentHash;
        bool found = false;
        lcl->txMap().visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                if (!found)
                {
                    paymentHash = item->key();
                    found = true;
                }
            });
        BEAST_EXPECT(found);

        // Build the proof.
        auto const lp = proof::buildLedgerProof(*lcl, paymentHash);
        BEAST_EXPECT(lp.has_value());

        if (lp)
        {
            // Verify header fields are populated.
            BEAST_EXPECT(lp->ledgerIndex > 0);
            BEAST_EXPECT(lp->totalCoins > 0);
            BEAST_EXPECT(lp->parentHash != uint256{});
            BEAST_EXPECT(lp->txRoot != uint256{});
            BEAST_EXPECT(lp->accountRoot != uint256{});

            // Verify tx blob is non-empty.
            BEAST_EXPECT(!lp->txBlob.empty());
            BEAST_EXPECT(!lp->metaBlob.empty());

            // Verify merkle proof exists and is valid.
            BEAST_EXPECT(lp->txProof.has_value());
            if (lp->txProof)
            {
                auto const computedRoot = lp->txProof->computeRoot();
                BEAST_EXPECT(computedRoot.has_value());
                if (computedRoot)
                    BEAST_EXPECT(*computedRoot == lp->txRoot);
            }

            // Verify ledger hash reconstruction.
            auto const computedHash = lp->computeLedgerHash();
            BEAST_EXPECT(computedHash == lcl->info().hash);
        }
    }

    void
    testBuildXPOPv1()
    {
        testcase("Build XPOP v1 JSON from a payment");

        using namespace jtx;

        Env env{*this};

        Account const alice{"alice"};
        Account const bob{"bob"};

        env.fund(XRP(10000), alice, bob);
        env.close();

        env(pay(alice, bob, XRP(100)));
        env.close();

        auto const lcl = env.app().getLedgerMaster().getClosedLedger();
        BEAST_EXPECT(lcl);

        // Find a tx.
        uint256 txHash;
        lcl->txMap().visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                txHash = item->key();
            });

        // Build XPOP using the test helper.
        auto const xpop = xpop::buildTestXPOP(env, txHash, 3);
        BEAST_EXPECT(!xpop.isNull());

        // Verify structure.
        BEAST_EXPECT(xpop.isMember(jss::ledger));
        BEAST_EXPECT(xpop.isMember(jss::transaction));
        BEAST_EXPECT(xpop.isMember(jss::validation));

        // Ledger section.
        auto const& lgr = xpop[jss::ledger];
        BEAST_EXPECT(lgr.isMember(jss::index));
        BEAST_EXPECT(lgr.isMember(jss::coins));
        BEAST_EXPECT(lgr.isMember(jss::phash));
        BEAST_EXPECT(lgr.isMember(jss::txroot));
        BEAST_EXPECT(lgr.isMember(jss::acroot));
        BEAST_EXPECT(lgr.isMember(jss::close));
        BEAST_EXPECT(lgr.isMember(jss::pclose));
        BEAST_EXPECT(lgr.isMember(jss::cres));
        BEAST_EXPECT(lgr.isMember(jss::flags));

        // Transaction section.
        auto const& txn = xpop[jss::transaction];
        BEAST_EXPECT(txn.isMember(jss::blob));
        BEAST_EXPECT(txn.isMember(jss::meta));
        BEAST_EXPECT(txn.isMember(jss::proof));
        BEAST_EXPECT(txn[jss::blob].asString().size() > 0);
        BEAST_EXPECT(txn[jss::meta].asString().size() > 0);

        // Validation section.
        auto const& val = xpop[jss::validation];
        BEAST_EXPECT(val.isMember(jss::data));
        BEAST_EXPECT(val.isMember(jss::unl));
        BEAST_EXPECT(val[jss::data].size() == 3);  // 3 validators

        auto const& unl = val[jss::unl];
        BEAST_EXPECT(unl.isMember(jss::public_key));
        BEAST_EXPECT(unl.isMember(jss::manifest));
        BEAST_EXPECT(unl.isMember(jss::blob));
        BEAST_EXPECT(unl.isMember(jss::signature));
        BEAST_EXPECT(unl.isMember(jss::version));
    }

    void
    testMerkleProofVerification()
    {
        testcase("Merkle proof verifies against tx root");

        using namespace jtx;

        Env env{*this};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        // Multiple transactions to create a deeper trie.
        env(pay(alice, bob, XRP(10)));
        env(pay(bob, carol, XRP(5)));
        env(pay(carol, alice, XRP(1)));
        env.close();

        auto const lcl = env.app().getLedgerMaster().getClosedLedger();
        BEAST_EXPECT(lcl);

        // Verify proof for each transaction in the ledger.
        int proofCount = 0;
        lcl->txMap().visitLeaves(
            [&](boost::intrusive_ptr<SHAMapItem const> const& item) {
                auto const lp = proof::buildLedgerProof(*lcl, item->key());
                BEAST_EXPECT(lp.has_value());

                if (lp && lp->txProof)
                {
                    // Proof must verify against the ledger's tx root.
                    BEAST_EXPECT(lp->txProof->verify(lp->txRoot));

                    // JSON v1 serialization must round-trip.
                    auto const json = lp->txProof->toJsonV1();
                    BEAST_EXPECT(!json.isNull());
                    BEAST_EXPECT(json.isArray());

                    ++proofCount;
                }
            });

        // We should have proven at least 3 transactions.
        BEAST_EXPECT(proofCount >= 3);
    }

    void
    run() override
    {
        testBuildLedgerProof();
        testBuildXPOPv1();
        testMerkleProofVerification();
    }
};

BEAST_DEFINE_TESTSUITE(XPOP, app, ripple);

}  // namespace test
}  // namespace ripple
