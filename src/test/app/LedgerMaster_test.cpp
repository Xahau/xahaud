//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPLF

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
#include <test/jtx/Env.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/detail/PublishGap.h>
#include <xrpl/basics/RangeSet.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

class LedgerMaster_test : public beast::unit_test::suite
{
    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->NETWORK_ID = networkID;
            return cfg;
        });
    }

    void
    testTxnIdFromIndex(FeatureBitset features)
    {
        testcase("tx_id_from_index");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(11111)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // build ledgers
        std::vector<std::shared_ptr<STTx const>> txns;
        std::vector<std::shared_ptr<STObject const>> metas;
        auto const startLegSeq = env.current()->info().seq;
        for (int i = 0; i < 2; ++i)
        {
            env(noop(alice));
            txns.emplace_back(env.tx());
            env.close();
            metas.emplace_back(
                env.closed()->txRead(env.tx()->getTransactionID()).second);
        }
        // add last (empty) ledger
        env.close();
        auto const endLegSeq = env.closed()->info().seq;

        // test invalid range
        {
            std::uint32_t ledgerSeq = -1;
            std::uint32_t txnIndex = 0;
            auto result =
                env.app().getLedgerMaster().txnIdFromIndex(ledgerSeq, txnIndex);
            BEAST_EXPECT(!result);
        }
        // test not in ledger
        {
            uint32_t txnIndex = metas[0]->getFieldU32(sfTransactionIndex);
            auto result =
                env.app().getLedgerMaster().txnIdFromIndex(0, txnIndex);
            BEAST_EXPECT(!result);
        }
        // test empty ledger
        {
            auto result =
                env.app().getLedgerMaster().txnIdFromIndex(endLegSeq, 0);
            BEAST_EXPECT(!result);
        }
        // ended without result
        {
            uint32_t txnIndex = metas[0]->getFieldU32(sfTransactionIndex);
            auto result = env.app().getLedgerMaster().txnIdFromIndex(
                endLegSeq + 1, txnIndex);
            BEAST_EXPECT(!result);
        }
        // success (first tx)
        {
            uint32_t txnIndex = metas[0]->getFieldU32(sfTransactionIndex);
            auto result = env.app().getLedgerMaster().txnIdFromIndex(
                startLegSeq, txnIndex);
            BEAST_EXPECT(
                *result ==
                uint256("0CC11AD1AD89661689F6B6148D82CB6A7101DA4D66EC843670262D"
                        "0B618F5745"));
        }
        // success (second tx)
        {
            uint32_t txnIndex = metas[1]->getFieldU32(sfTransactionIndex);
            auto result = env.app().getLedgerMaster().txnIdFromIndex(
                startLegSeq + 1, txnIndex);
            BEAST_EXPECT(
                *result ==
                uint256("DCAECE52F028B9D0F7CA43CBE15AB4EF7B84A8EF5D455238992E1E"
                        "DF2BDA1637"));
        }
    }

    void
    testCanSkipPinnedGap()
    {
        // detail::canSkipPinnedGap is the lifted decision used by
        // findNewLedgersToPublish to decide whether pubSeq can leap
        // across a gap in the publish-target set without violating
        // contiguous publication of non-pinned ledgers. In production
        // this guard rarely fires (pinned ranges are old historical
        // catalogue data, pubSeq is at the live tip) — it matters in
        // test/standalone catalogue-load scenarios where pinned seqs
        // can sit near pubSeq. The invariant is small and crisp;
        // unit-testing it directly is far cheaper than reconstructing
        // a believable findNewLedgersToPublish state through Env.
        testcase("detail::canSkipPinnedGap");

        using detail::canSkipPinnedGap;

        // No gap (pubSeq is already at intervalStart) — always OK.
        {
            RangeSet<std::uint32_t> empty;
            BEAST_EXPECT(canSkipPinnedGap(10, 10, empty));
            BEAST_EXPECT(canSkipPinnedGap(10, 5, empty));  // past
        }

        // One-element gap, exactly pinned: can skip.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(10u, 10u));
            BEAST_EXPECT(canSkipPinnedGap(10, 11, pinned));
        }

        // One-element gap, NOT pinned: must halt.
        {
            RangeSet<std::uint32_t> empty;
            BEAST_EXPECT(!canSkipPinnedGap(10, 11, empty));
        }

        // Multi-element gap entirely covered by pinned: can skip.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(11u, 15u));
            BEAST_EXPECT(canSkipPinnedGap(11, 16, pinned));
        }

        // Multi-element gap, pinned covers most but leaves one
        // non-pinned seq exposed: must halt. This is the bug the
        // guard exists to prevent — silently publishing seq 16
        // while seq 15 (non-pinned) is unfetched.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(11u, 14u));  // missing 15
            BEAST_EXPECT(!canSkipPinnedGap(11, 16, pinned));
        }

        // Pinned set extends beyond the gap — only the gap window
        // matters for the decision.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(0u, 1000u));
            BEAST_EXPECT(canSkipPinnedGap(50, 100, pinned));
        }

        // Empty pinned, with a gap: must halt.
        {
            RangeSet<std::uint32_t> empty;
            BEAST_EXPECT(!canSkipPinnedGap(50, 100, empty));
        }

        // Disjoint pinned that doesn't intersect the gap: must halt.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(200u, 300u));
            BEAST_EXPECT(!canSkipPinnedGap(50, 100, pinned));
        }

        // Pinned partially intersects gap from the left only.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(50u, 75u));
            BEAST_EXPECT(!canSkipPinnedGap(50, 100, pinned));
        }

        // Pinned partially intersects gap from the right only.
        {
            RangeSet<std::uint32_t> pinned;
            pinned.insert(range(75u, 99u));
            BEAST_EXPECT(!canSkipPinnedGap(50, 100, pinned));
        }
    }

    void
    testPinUnpinSymmetry()
    {
        testcase("pin/unpin symmetry");

        using namespace test::jtx;
        Env env{*this, makeNetworkConfig(11111)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        auto& lm = env.app().getLedgerMaster();

        // The most recently closed ledger is the one we'll pin.
        auto const ledger = lm.getClosedLedger();
        BEAST_EXPECT(ledger);
        if (!ledger)
            return;
        auto const seq = ledger->info().seq;
        BEAST_EXPECT(seq > 0);

        // Baseline — not pinned.
        BEAST_EXPECT(!lm.isPinned(seq));

        // Pin via storeLedger(ledger, pin=true). This is the same path
        // catalogue_load uses just before saveValidatedLedger, which is why
        // a save failure must roll back via unpinLedger() to keep the
        // in-memory pinned-set consistent with state.db.
        lm.storeLedger(ledger, /*pin=*/true);
        BEAST_EXPECT(lm.isPinned(seq));
        BEAST_EXPECT(boost::icl::contains(lm.getPinnedLedgersRangeSet(), seq));

        // Unpin reverses the pin (the rollback contract used by
        // catalogue_load's RAII guard on save failure).
        lm.unpinLedger(seq);
        BEAST_EXPECT(!lm.isPinned(seq));
        BEAST_EXPECT(!boost::icl::contains(lm.getPinnedLedgersRangeSet(), seq));

        // Unpinning a seq that isn't pinned is a no-op (idempotent).
        lm.unpinLedger(seq);
        BEAST_EXPECT(!lm.isPinned(seq));
    }

    void
    testSetPinnedRangesImmediateMerge()
    {
        // Regression test: when setPinnedLedgersRangeSet runs and
        // mCompleteLedgers is already non-empty (LOAD/standalone startup
        // already called setFullLedger), the deferred merge in
        // setFullLedger never fires because the one-shot guard relies on
        // a flag, not on emptiness. Without the immediate-merge branch
        // in setPinnedLedgersRangeSet, pinned ranges restored from
        // state.db would never appear in mCompleteLedgers and historical
        // pinned ledgers would not be queryable until the next
        // validation. This test asserts the immediate-merge behavior.
        testcase("setPinnedLedgersRangeSet immediate merge");

        using namespace test::jtx;
        Env env{*this, makeNetworkConfig(11111)};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        auto& lm = env.app().getLedgerMaster();

        // Why mCompleteLedgers is non-empty here:
        //   env.close() above runs synchronous standalone consensus.
        //   That hits LedgerMaster::switchLCL, which (in standalone)
        //   calls setFullLedger directly on the calling thread, which
        //   inserts the closed ledger's seq into mCompleteLedgers.
        //   So by the time we get here, mCompleteLedgers has the
        //   genesis + funded seqs in it.
        //
        // This is the precondition that triggers the immediate-merge
        // branch in setPinnedLedgersRangeSet (the LOAD/standalone
        // startup case). On a NORMAL/NETWORK production startup,
        // mCompleteLedgers would still be empty when state.db is read
        // and setPinnedLedgersRangeSet is called, and the merge would
        // be deferred to the first validated ledger via setFullLedger.
        // We're specifically testing the *other* branch here.
        auto const completeBefore = lm.getCompleteLedgersRangeSet();
        BEAST_EXPECT(!completeBefore.empty());

        // Pinned ranges are far outside the existing range so we can
        // unambiguously detect them being merged in.
        std::uint32_t const pinLo = 100000;
        std::uint32_t const pinHi = 100099;
        RangeSet<std::uint32_t> pinned;
        pinned.insert(range(pinLo, pinHi));

        // Restore pinned ranges (mirrors what SHAMapStoreImp::start does
        // after reading state.db's PinnedLedgers row).
        lm.setPinnedLedgersRangeSet(pinned);

        // Pinned set is restored.
        BEAST_EXPECT(lm.isPinned(pinLo));
        BEAST_EXPECT(lm.isPinned(pinHi));
        BEAST_EXPECT(lm.isPinned((pinLo + pinHi) / 2));

        // Critical invariant: pinned ranges are now in mCompleteLedgers
        // immediately, not deferred to the first validated ledger.
        // This is what makes haveLedger() return true for pinned seqs
        // after a LOAD/standalone restart, without which the historical
        // pinned data is on disk but the system reports "we don't have
        // it".
        BEAST_EXPECT(lm.haveLedger(pinLo));
        BEAST_EXPECT(lm.haveLedger(pinHi));
        BEAST_EXPECT(lm.haveLedger((pinLo + pinHi) / 2));

        // The pre-existing complete range is still complete (merge,
        // not replace).
        for (auto const& interval : completeBefore)
            for (auto s = interval.lower(); s <= interval.upper(); ++s)
                BEAST_EXPECT(lm.haveLedger(s));
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments() - featureXahauGenesis};
        testCanSkipPinnedGap();
        testWithFeats(all);
        testPinUnpinSymmetry();
        testSetPinnedRangesImmediateMerge();
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testTxnIdFromIndex(features);
    }
};

BEAST_DEFINE_TESTSUITE(LedgerMaster, app, ripple);

}  // namespace test
}  // namespace ripple
