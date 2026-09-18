//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES WHATSOEVER
    RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
    CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
    CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpl/protocol/Protocol.h>

#include <set>
#include <vector>

namespace ripple {
namespace test {

struct ExportLatch_test : beast::unit_test::suite
{
    static std::shared_ptr<SLE>
    makeLatch(AccountID const& account, std::uint32_t ordinal)
    {
        uint256 const origin{ordinal + 1};
        auto latch =
            std::make_shared<SLE>(keylet::exportLatch(account, origin));
        latch->setAccountID(sfAccount, account);
        latch->setFieldU32(sfTicketSequence, 50'000 + ordinal);
        latch->setFieldH256(sfTransactionHash, origin);
        latch->setFieldH256(sfDigest, uint256{10'000 + ordinal});
        latch->setFieldU32(sfLedgerSequence, 4'000'000 + ordinal);
        latch->setFieldH256(sfExportCommitteeHash, uint256{20'000 + ordinal});
        return latch;
    }

    void
    testDirectoryLifecycle()
    {
        testcase("enhanced latch directory lifecycle");

        using namespace jtx;
        Account const alice{"alice"};
        Env env{*this};
        env.fund(XRP(10'000), alice);
        env(ticket::create(alice, dirNodeMaxEntries));
        env.close();

        beast::Journal j{beast::Journal::getNullSink()};
        Sandbox sb{env.closed().get(), tapNONE};
        std::vector<Keylet> latches;
        latches.reserve(dirNodeMaxEntries + 1);

        for (std::uint32_t i = 0; i <= dirNodeMaxEntries; ++i)
        {
            auto latch = makeLatch(alice.id(), i);
            latches.emplace_back(keylet::unchecked(latch->key()));
            auto const ter =
                ExportLedgerOps::insertPendingExportLatch(sb, sb, latch, j);
            if (!BEAST_EXPECTS(
                    isTesSuccess(ter),
                    "ordinal=" + std::to_string(i) + " ter=" + transToken(ter)))
                return;
        }

        auto const account = sb.read(keylet::account(alice.id()));
        auto const pending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(account);
        BEAST_EXPECT(pending);
        if (!account || !pending)
            return;
        BEAST_EXPECT(account->getFieldU16(sfExportCount) == 33);
        BEAST_EXPECT(account->getFieldU32(sfOwnerCount) == 65);
        BEAST_EXPECT(pending->getFieldU16(sfExportCount) == 33);
        BEAST_EXPECT(pending->getFieldV256(sfIndexes).size() == 32);

        auto const first = sb.read(latches.front());
        auto const last = sb.read(latches.back());
        BEAST_EXPECT(first && first->getFieldU64(sfOwnerNode) == 1);
        BEAST_EXPECT(first && first->isFieldPresent(sfExportNode));
        BEAST_EXPECT(first && first->getFieldU64(sfExportNode) == 0);
        BEAST_EXPECT(last && last->getFieldU64(sfOwnerNode) == 2);
        BEAST_EXPECT(last && last->getFieldU64(sfExportNode) == 1);
        if (!first || !last)
            return;

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::removePendingExportLink(sb, latches.front(), j)));
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::removePendingExportLink(sb, latches.back(), j)));
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::removePendingExportLink(sb, latches.back(), j)));

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::eraseExportLatch(sb, sb, latches.front(), j)));
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::eraseExportLatch(sb, sb, latches[1], j)));

        auto const afterAccount = sb.read(keylet::account(alice.id()));
        auto const afterPending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(afterAccount->getFieldU16(sfExportCount) == 31);
        BEAST_EXPECT(afterAccount->getFieldU32(sfOwnerCount) == 63);
        BEAST_EXPECT(afterPending->getFieldU16(sfExportCount) == 30);

        Sandbox reopened{&sb};
        std::set<uint256> recovered;
        forEachItem(
            reopened,
            keylet::pendingExports(),
            [&](std::shared_ptr<SLE const> const& sle) {
                recovered.insert(sle->key());
            });
        BEAST_EXPECT(recovered.size() == 30);
        std::set<uint256> expected;
        for (std::size_t i = 2; i + 1 < latches.size(); ++i)
            expected.insert(latches[i].key);
        BEAST_EXPECT(recovered == expected);

        for (std::size_t i = 2; i < latches.size(); ++i)
            BEAST_EXPECT(isTesSuccess(
                ExportLedgerOps::eraseExportLatch(sb, sb, latches[i], j)));

        auto const finalAccount = sb.read(keylet::account(alice.id()));
        auto const finalPending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(finalAccount->getFieldU16(sfExportCount) == 0);
        BEAST_EXPECT(finalAccount->getFieldU32(sfOwnerCount) == 32);
        BEAST_EXPECT(finalPending);
        BEAST_EXPECT(finalPending->getFieldU16(sfExportCount) == 0);
        BEAST_EXPECT(finalPending->getFieldV256(sfIndexes).empty());
        BEAST_EXPECT(finalPending->getFieldU64(sfIndexNext) == 0);
        BEAST_EXPECT(finalPending->getFieldU64(sfIndexPrevious) == 0);
    }

    void
    testWitnessedLatchReleasesGlobalPendingCap()
    {
        testcase("witnessed latch releases global pending cap");

        using namespace jtx;
        Account const alice{"alice"};
        Env env{*this};
        env.fund(XRP(10'000), alice);
        env.close();

        beast::Journal j{beast::Journal::getNullSink()};
        Sandbox sb{env.closed().get(), tapNONE};
        std::vector<Keylet> latches;
        latches.reserve(ExportLimits::maxLiveExportLatches);

        for (std::uint32_t i = 0; i < ExportLimits::maxLiveExportLatches; ++i)
        {
            auto latch = makeLatch(alice.id(), i);
            latches.emplace_back(keylet::unchecked(latch->key()));
            BEAST_EXPECT(isTesSuccess(
                ExportLedgerOps::insertPendingExportLatch(sb, sb, latch, j)));
        }

        auto const accountAtCap = sb.read(keylet::account(alice.id()));
        auto const rootAtCap = sb.read(keylet::pendingExports());
        BEAST_EXPECT(accountAtCap);
        BEAST_EXPECT(rootAtCap);
        if (!accountAtCap || !rootAtCap)
            return;
        BEAST_EXPECT(
            rootAtCap->getFieldU16(sfExportCount) ==
            ExportLimits::maxLiveExportLatches);
        auto const ownerCountAtCap = accountAtCap->getFieldU32(sfOwnerCount);

        auto replacement =
            makeLatch(alice.id(), ExportLimits::maxLiveExportLatches);
        auto const replacementKey = keylet::unchecked(replacement->key());
        BEAST_EXPECT(
            ExportLedgerOps::insertPendingExportLatch(sb, sb, replacement, j) ==
            tecDIR_FULL);
        auto const accountAfterRejection = sb.read(keylet::account(alice.id()));
        auto const rootAfterRejection = sb.read(keylet::pendingExports());
        BEAST_EXPECT(accountAfterRejection);
        BEAST_EXPECT(rootAfterRejection);
        if (!accountAfterRejection || !rootAfterRejection)
            return;
        BEAST_EXPECT(!sb.exists(replacementKey));
        BEAST_EXPECT(
            accountAfterRejection->getFieldU16(sfExportCount) ==
            ExportLimits::maxLiveExportLatches);
        BEAST_EXPECT(
            accountAfterRejection->getFieldU32(sfOwnerCount) ==
            ownerCountAtCap);
        BEAST_EXPECT(
            rootAfterRejection->getFieldU16(sfExportCount) ==
            ExportLimits::maxLiveExportLatches);

        uint256 const witnessHash{99'999};
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::recordExportWitness(
            sb, sb, latches.front(), witnessHash, j)));

        auto const witnessed = sb.read(latches.front());
        auto const accountAfterWitness = sb.read(keylet::account(alice.id()));
        auto const rootAfterWitness = sb.read(keylet::pendingExports());
        BEAST_EXPECT(witnessed);
        BEAST_EXPECT(accountAfterWitness);
        BEAST_EXPECT(rootAfterWitness);
        if (!witnessed || !accountAfterWitness || !rootAfterWitness)
            return;
        BEAST_EXPECT(!witnessed->isFieldPresent(sfExportNode));
        BEAST_EXPECT(
            witnessed->getFieldH256(sfExportSignatureHash) == witnessHash);
        BEAST_EXPECT(
            accountAfterWitness->getFieldU16(sfExportCount) ==
            ExportLimits::maxLiveExportLatches);
        BEAST_EXPECT(
            accountAfterWitness->getFieldU32(sfOwnerCount) == ownerCountAtCap);
        BEAST_EXPECT(
            rootAfterWitness->getFieldU16(sfExportCount) ==
            ExportLimits::maxLiveExportLatches - 1);

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::insertPendingExportLatch(sb, sb, replacement, j)));
        BEAST_EXPECT(
            sb.read(keylet::pendingExports())->getFieldU16(sfExportCount) ==
            ExportLimits::maxLiveExportLatches);
        BEAST_EXPECT(sb.exists(latches.front()));
    }

    void
    testRetainedLatchAcceptsBothFactOrders()
    {
        testcase("retained latch accepts XPOP and witness in either order");

        using namespace jtx;
        Account const alice{"alice"};
        Env env{*this};
        env.fund(XRP(10'000), alice);
        env.close();

        beast::Journal j{beast::Journal::getNullSink()};
        Sandbox sb{env.closed().get(), tapNONE};
        auto const baselineAccount = sb.read(keylet::account(alice.id()));
        BEAST_EXPECT(baselineAccount);
        if (!baselineAccount)
            return;
        auto const baselineExportCount =
            ExportLedgerOps::exportLatchCount(*baselineAccount);
        auto const baselineOwnerCount =
            baselineAccount->getFieldU32(sfOwnerCount);
        auto const baselineReserve =
            sb.fees().accountReserve(baselineOwnerCount);

        auto xpopFirst = makeLatch(alice.id(), 0);
        auto const xpopFirstKey = keylet::unchecked(xpopFirst->key());
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::insertPendingExportLatch(sb, sb, xpopFirst, j)));
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::controlExportLatch(
            sb,
            sb,
            alice.id(),
            xpopFirst->getFieldH256(sfTransactionHash),
            false,
            j)));

        auto const canceled = sb.read(xpopFirstKey);
        BEAST_EXPECT(canceled);
        if (!canceled)
            return;
        auto const canceledFlags = canceled->isFieldPresent(sfFlags)
            ? canceled->getFieldU32(sfFlags)
            : std::uint32_t{0};
        BEAST_EXPECT((canceledFlags & lsfExportCanceled) != 0);
        BEAST_EXPECT(!canceled->isFieldPresent(sfExportNode));

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::recordExportXpop(sb, sb, xpopFirstKey, j)));
        auto const afterXpop = sb.read(xpopFirstKey);
        BEAST_EXPECT(afterXpop);
        if (!afterXpop)
            return;
        auto const xpopFlags = afterXpop->getFieldU32(sfFlags);
        BEAST_EXPECT((xpopFlags & lsfExportCanceled) != 0);
        BEAST_EXPECT((xpopFlags & lsfExportXpopSeen) != 0);

        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::recordExportWitness(
            sb, sb, xpopFirstKey, uint256{99'999}, j)));
        BEAST_EXPECT(!sb.exists(xpopFirstKey));
        auto const accountAfterWitness = sb.read(keylet::account(alice.id()));
        auto const rootAfterWitness = sb.read(keylet::pendingExports());
        BEAST_EXPECT(accountAfterWitness);
        BEAST_EXPECT(rootAfterWitness);
        if (!accountAfterWitness || !rootAfterWitness)
            return;
        BEAST_EXPECT(
            ExportLedgerOps::exportLatchCount(*accountAfterWitness) ==
            baselineExportCount);
        BEAST_EXPECT(
            accountAfterWitness->getFieldU32(sfOwnerCount) ==
            baselineOwnerCount);
        BEAST_EXPECT(
            sb.fees().accountReserve(accountAfterWitness->getFieldU32(
                sfOwnerCount)) == baselineReserve);
        BEAST_EXPECT(ExportLedgerOps::exportLatchCount(*rootAfterWitness) == 0);

        auto witnessFirst = makeLatch(alice.id(), 1);
        auto const witnessFirstKey = keylet::unchecked(witnessFirst->key());
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::insertPendingExportLatch(
            sb, sb, witnessFirst, j)));
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::controlExportLatch(
            sb,
            sb,
            alice.id(),
            witnessFirst->getFieldH256(sfTransactionHash),
            false,
            j)));

        // The accepted witness may already have been materialized before
        // cancellation even when consensus orders its application afterward.
        uint256 const witnessHash{100'000};
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::recordExportWitness(
            sb, sb, witnessFirstKey, witnessHash, j)));
        auto const afterWitness = sb.read(witnessFirstKey);
        BEAST_EXPECT(afterWitness);
        if (!afterWitness)
            return;
        auto const witnessFlags = afterWitness->getFieldU32(sfFlags);
        BEAST_EXPECT((witnessFlags & lsfExportCanceled) != 0);
        BEAST_EXPECT(
            afterWitness->getFieldH256(sfExportSignatureHash) == witnessHash);
        BEAST_EXPECT(!afterWitness->isFieldPresent(sfExportNode));

        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::recordExportXpop(sb, sb, witnessFirstKey, j)));
        BEAST_EXPECT(!sb.exists(witnessFirstKey));
        auto const accountAfterXpop = sb.read(keylet::account(alice.id()));
        auto const rootAfterXpop = sb.read(keylet::pendingExports());
        BEAST_EXPECT(accountAfterXpop);
        BEAST_EXPECT(rootAfterXpop);
        if (!accountAfterXpop || !rootAfterXpop)
            return;
        BEAST_EXPECT(
            ExportLedgerOps::exportLatchCount(*accountAfterXpop) ==
            baselineExportCount);
        BEAST_EXPECT(
            accountAfterXpop->getFieldU32(sfOwnerCount) == baselineOwnerCount);
        BEAST_EXPECT(
            sb.fees().accountReserve(accountAfterXpop->getFieldU32(
                sfOwnerCount)) == baselineReserve);
        BEAST_EXPECT(ExportLedgerOps::exportLatchCount(*rootAfterXpop) == 0);
    }

    void
    testExplicitEraseAndExpiryRetention()
    {
        testcase("explicit erase and expiry retention");

        using namespace jtx;
        Account const alice{"alice"};
        Env env{*this};
        env.fund(XRP(10'000), alice);
        env.close();

        beast::Journal j{beast::Journal::getNullSink()};
        Sandbox sb{env.closed().get(), tapNONE};
        auto const baselineAccount = sb.read(keylet::account(alice.id()));
        auto const baselinePending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(baselineAccount);
        if (!baselineAccount)
            return;
        auto const baselineExportCount =
            ExportLedgerOps::exportLatchCount(*baselineAccount);
        auto const baselineOwnerCount =
            baselineAccount->getFieldU32(sfOwnerCount);
        auto const baselinePendingCount = baselinePending
            ? ExportLedgerOps::exportLatchCount(*baselinePending)
            : std::uint16_t{0};

        auto erased = makeLatch(alice.id(), 0);
        auto const erasedOrigin = erased->getFieldH256(sfTransactionHash);
        auto const erasedKey = keylet::exportLatch(alice.id(), erasedOrigin);
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::insertPendingExportLatch(sb, sb, erased, j)));
        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::controlExportLatch(
            sb, sb, alice.id(), erasedOrigin, true, j)));
        BEAST_EXPECT(!sb.exists(erasedKey));

        auto const afterEraseAccount = sb.read(keylet::account(alice.id()));
        auto const afterErasePending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(afterEraseAccount);
        BEAST_EXPECT(afterErasePending);
        if (!afterEraseAccount || !afterErasePending)
            return;
        BEAST_EXPECT(
            ExportLedgerOps::exportLatchCount(*afterEraseAccount) ==
            baselineExportCount);
        BEAST_EXPECT(
            afterEraseAccount->getFieldU32(sfOwnerCount) == baselineOwnerCount);
        BEAST_EXPECT(
            ExportLedgerOps::exportLatchCount(*afterErasePending) ==
            baselinePendingCount);

        auto expired = makeLatch(alice.id(), 1);
        expired->setFieldU32(sfLastLedgerSequence, 100);
        auto const expiredOrigin = expired->getFieldH256(sfTransactionHash);
        auto const expiredKey = keylet::exportLatch(alice.id(), expiredOrigin);
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::insertPendingExportLatch(sb, sb, expired, j)));
        BEAST_EXPECT(isTesSuccess(
            ExportLedgerOps::pruneExpiredExportLatches(sb, sb, 101, j)));

        auto const retained = sb.read(expiredKey);
        auto const afterExpiryAccount = sb.read(keylet::account(alice.id()));
        auto const afterExpiryPending = sb.read(keylet::pendingExports());
        BEAST_EXPECT(retained);
        BEAST_EXPECT(afterExpiryAccount);
        BEAST_EXPECT(afterExpiryPending);
        if (!retained || !afterExpiryAccount || !afterExpiryPending)
            return;
        BEAST_EXPECT(!retained->isFieldPresent(sfExportNode));
        BEAST_EXPECT((retained->getFieldU32(sfFlags) & lsfExportCanceled) != 0);
        BEAST_EXPECT(
            ExportLedgerOps::exportLatchCount(*afterExpiryAccount) ==
            baselineExportCount + 1);
        BEAST_EXPECT(
            afterExpiryAccount->getFieldU32(sfOwnerCount) ==
            baselineOwnerCount + 1);
        BEAST_EXPECT(
            ExportLedgerOps::exportLatchCount(*afterExpiryPending) ==
            baselinePendingCount);

        BEAST_EXPECT(isTesSuccess(ExportLedgerOps::controlExportLatch(
            sb, sb, alice.id(), expiredOrigin, true, j)));
        BEAST_EXPECT(!sb.exists(expiredKey));
    }

    void
    run() override
    {
        testDirectoryLifecycle();
        testWitnessedLatchReleasesGlobalPendingCap();
        testRetainedLatchAcceptsBothFactOrders();
        testExplicitEraseAndExpiryRetention();
    }
};

BEAST_DEFINE_TESTSUITE(ExportLatch, app, ripple);

}  // namespace test
}  // namespace ripple
